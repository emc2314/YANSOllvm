#include "MFLAPass.h"
#include "Utils.h"
#include "YANSOllvmCommon.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <vector>

using namespace llvm;

namespace {
constexpr StringLiteral PassName = "mfla";

struct StorageRef {
  Type *Ty = nullptr;
  uint64_t Offset = 0;
};

static StorageRef staticRef(Type *Ty, uint64_t Offset) { return {Ty, Offset}; }

struct MFLAArtifacts {
  Function *Mega = nullptr;
  GlobalVariable *Frame = nullptr;
  GlobalVariable *RetCont = nullptr;
  GlobalVariable *TargetTable = nullptr;
  SmallVector<GlobalVariable *, 8> RemappedGlobals;

  void eraseFromParent() {
    for (GlobalVariable *GV : reverse(RemappedGlobals)) {
      if (GV)
        GV->eraseFromParent();
    }
    RemappedGlobals.clear();
    if (TargetTable) {
      TargetTable->eraseFromParent();
      TargetTable = nullptr;
    }
    if (Mega) {
      Mega->eraseFromParent();
      Mega = nullptr;
    }
    if (Frame) {
      Frame->eraseFromParent();
      Frame = nullptr;
    }
    if (RetCont) {
      RetCont->eraseFromParent();
      RetCont = nullptr;
    }
  }
};

struct FunctionLayout {
  unsigned EntryID = 0;
  Function *Owner = nullptr;
  SmallVector<StorageRef, 4> ArgOffsets;
  DenseMap<Argument *, StorageRef> ArgOffsetFor;
  StorageRef RetOffset;
  DenseMap<PHINode *, StorageRef> PhiOffsets;
  DenseMap<CallInst *, StorageRef> CallResultOffsets;
  DenseMap<Instruction *, StorageRef> SpilledValueOffsets;
};

struct FramePlan {
  DenseMap<Function *, FunctionLayout> Layouts;
  uint64_t FrameSize = 0;
};

struct StaticLocalPlan {
  Function *F = nullptr;
  FunctionLayout Layout;
  uint64_t LocalSize = 0;
  uint64_t Base = 0;
};

struct CallGraphInfo {
  DenseMap<Function *, SmallVector<Function *, 4>> Succs;
};

struct SCCInfo {
  SmallVector<Function *, 4> Members;
  bool Recursive = false;
};

static bool hasUnsupportedCallBr(Function &F) {
  for (BasicBlock &BB : F)
    if (isa<CallBrInst>(BB.getTerminator()))
      return true;
  return false;
}

static Function *directCalledFunction(CallInst *Call) {
  if (!Call)
    return nullptr;
  Function *Callee = Call->getCalledFunction();
  if (!Callee || Callee->getReturnType() != Call->getType())
    return nullptr;
  return Callee;
}

static bool isFrameScalar(Type *Ty) {
  if (Ty->isIntegerTy() || Ty->isPointerTy() || Ty->isFloatingPointTy())
    return true;
  auto *VTy = dyn_cast<FixedVectorType>(Ty);
  return VTy && isFrameScalar(VTy->getElementType());
}

static uint64_t alignOffset(uint64_t Offset, Type *Ty, const DataLayout &DL) {
  return llvm::alignTo(Offset, DL.getABITypeAlign(Ty).value());
}

static uint64_t slotSize(Type *Ty, const DataLayout &DL) {
  return DL.getTypeStoreSize(Ty);
}

static uint64_t reserveSlot(uint64_t &NextOffset, Type *Ty,
                            const DataLayout &DL) {
  uint64_t Offset = alignOffset(NextOffset, Ty, DL);
  NextOffset = Offset + slotSize(Ty, DL);
  return Offset;
}

static bool constantContainsBlockAddress(Constant *C) {
  if (!C)
    return false;
  if (auto *BA = dyn_cast<BlockAddress>(C))
    return true;
  if (isa<GlobalValue>(C))
    return false;
  for (Use &U : C->operands()) {
    auto *OpC = dyn_cast<Constant>(U.get());
    if (!OpC)
      continue;
    if (OpC == C)
      continue;
    if (constantContainsBlockAddress(OpC))
      return true;
  }
  return false;
}

static bool constantUsesOnlyBlockAddressesInFunction(Constant *C, Function &F) {
  if (!C)
    return true;
  if (auto *BA = dyn_cast<BlockAddress>(C))
    return BA->getFunction() == &F;
  for (Use &U : C->operands()) {
    auto *OpC = dyn_cast<Constant>(U.get());
    if (!OpC)
      continue;
    if (OpC == C)
      continue;
    if (!constantUsesOnlyBlockAddressesInFunction(OpC, F))
      return false;
  }
  return true;
}

static bool isLocalGlobalPtr(Value *V) {
  if (auto *GV = dyn_cast<GlobalVariable>(V->stripPointerCasts()))
    return GV->hasLocalLinkage();
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return isLocalGlobalPtr(GEP->getPointerOperand());
  return false;
}

static bool instructionBlockAddressUseIsSupported(Instruction &I, Function &F) {
  bool HasBA = false;
  for (Use &U : I.operands()) {
    auto *C = dyn_cast<Constant>(U.get());
    if (!C)
      continue;
    if (!constantUsesOnlyBlockAddressesInFunction(C, F))
      return false;
    HasBA |= constantContainsBlockAddress(C);
  }
  if (!HasBA)
    return true;
  if (isa<CallBase>(&I) || isa<ReturnInst>(&I))
    return false;
  if (isa<IndirectBrInst>(&I))
    return true;
  if (auto *Store = dyn_cast<StoreInst>(&I)) {
    auto *StoredC = dyn_cast<Constant>(Store->getValueOperand());
    if (StoredC && constantContainsBlockAddress(StoredC))
      return isLocalGlobalPtr(Store->getPointerOperand());
    return true;
  }
  return true;
}

static bool indirectBrDestinationsHavePhis(Function &F) {
  for (BasicBlock &BB : F) {
    auto *IB = dyn_cast<IndirectBrInst>(BB.getTerminator());
    if (!IB)
      continue;
    for (unsigned I = 0, E = IB->getNumDestinations(); I != E; ++I)
      if (isa<PHINode>(&IB->getDestination(I)->front()))
        return true;
  }
  return false;
}

static bool hasSupportedBlockAddressDomain(Function &F) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (!instructionBlockAddressUseIsSupported(I, F))
        return false;
  return true;
}

static bool hasUnsupportedABIAttrs(Function &F) {
  auto BadParamAttr = [](AttributeSet Attrs) {
    return Attrs.hasAttribute(Attribute::StructRet) ||
           Attrs.hasAttribute(Attribute::ByVal) ||
           Attrs.hasAttribute(Attribute::InAlloca) ||
           Attrs.hasAttribute(Attribute::SwiftError) ||
           Attrs.hasAttribute(Attribute::Preallocated);
  };
  for (unsigned I = 0, E = F.arg_size(); I != E; ++I)
    if (BadParamAttr(F.getAttributes().getParamAttrs(I)))
      return true;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call)
        continue;
      if (Call->isMustTailCall())
        return true;
      for (unsigned ArgNo = 0, E = Call->arg_size(); ArgNo != E; ++ArgNo)
        if (BadParamAttr(Call->getAttributes().getParamAttrs(ArgNo)))
          return true;
    }
  }
  return false;
}

static bool isCandidate(Function &F) {
  if (F.isDeclaration())
    return false;
  if (F.isVarArg()) {
    YANSO_WARN_FUNCTION(PassName, F, "vararg function");
    return false;
  }
  if (hasUnsupportedABIAttrs(F)) {
    YANSO_WARN_FUNCTION(PassName, F, "unsupported ABI parameter attribute");
    return false;
  }
  if (hasUnsupportedCallBr(F)) {
    YANSO_ERROR_FUNCTION(PassName, F, "contains callbr");
    return false;
  }
  if (!hasSupportedBlockAddressDomain(F)) {
    YANSO_WARN_FUNCTION(PassName, F, "unsupported blockaddress use");
    return false;
  }
  if (indirectBrDestinationsHavePhis(F)) {
    YANSO_WARN_FUNCTION(PassName, F, "indirectbr destination PHI nodes");
    return false;
  }
  if (yansollvm_has_dynamic_stack_state(F)) {
    YANSO_WARN_FUNCTION(PassName, F, "dynamic stack state");
    return false;
  }
  if (!F.hasLocalLinkage())
    YANSO_WARN_FUNCTION(PassName, F,
                        "non-local function will be kept as ABI wrapper");
  if (F.empty())
    return false;
  return true;
}

static bool supportsCurrentLowering(
    Function &F, const DenseMap<Function *, unsigned> &CandidateIDs) {
  Type *RetTy = F.getReturnType();
  if (!RetTy->isVoidTy() && !isFrameScalar(RetTy)) {
    YANSO_WARN_FUNCTION(PassName, F,
                        "current lowering supports only scalar return functions");
    return false;
  }
  for (Argument &Arg : F.args()) {
    if (!isFrameScalar(Arg.getType())) {
      YANSO_WARN_FUNCTION(PassName, F, "current lowering supports only scalar arguments");
      return false;
    }
  }
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<PHINode>(&I)) {
        auto *Phi = cast<PHINode>(&I);
        if (!isFrameScalar(Phi->getType())) {
          YANSO_WARN_FUNCTION(PassName, F,
                              "current lowering supports only scalar PHI nodes");
          return false;
        }
        continue;
      }
      if (auto *Call = dyn_cast<CallInst>(&I)) {
        Function *RawCallee = Call->getCalledFunction();
        if (RawCallee && CandidateIDs.count(RawCallee) &&
            RawCallee->getReturnType() != Call->getType()) {
          YANSO_WARN_FUNCTION(PassName, F,
                              "callee wrapper return type changed");
          return false;
        }
        if (Function *Callee = directCalledFunction(Call)) {
          if (CandidateIDs.count(Callee)) {
            if (!Call->getType()->isVoidTy() &&
                !isFrameScalar(Call->getType())) {
              YANSO_WARN_FUNCTION(
                  PassName, F,
                  "current lowering supports only void or scalar internal call results");
              return false;
            }
            if (Call->mayThrow() &&
                !Callee->hasFnAttribute(Attribute::NoUnwind) &&
                !F.hasFnAttribute(Attribute::NoUnwind)) {
              YANSO_WARN_FUNCTION(
                  PassName, F,
                  "current lowering does not support throwing internal calls");
              return false;
            }
          }
        }
      }
      if (!I.getType()->isVoidTy() && !isFrameScalar(I.getType())) {
        YANSO_WARN_FUNCTION(PassName, F,
                            "current lowering supports only scalar instruction results");
        return false;
      }
      if (isa<LandingPadInst>(&I) || isa<CatchPadInst>(&I) ||
          isa<CleanupPadInst>(&I)) {
        YANSO_WARN_FUNCTION(PassName, F,
                            "current lowering keeps EH for a later region pass");
        return false;
      }
    }
    Instruction *Term = BB.getTerminator();
    if (!isa<ReturnInst>(Term) && !isa<BranchInst>(Term) &&
        !isa<SwitchInst>(Term) && !isa<IndirectBrInst>(Term)) {
      YANSO_WARN_FUNCTION(
          PassName, F,
          "current lowering supports only branch, switch, indirectbr, and return terminators");
      return false;
    }
  }
  return true;
}

static CallGraphInfo buildCandidateCallGraph(ArrayRef<Function *> Candidates) {
  DenseSet<Function *> CandidateSet;
  for (Function *F : Candidates)
    CandidateSet.insert(F);

  CallGraphInfo Graph;
  for (Function *F : Candidates) {
    SmallVector<Function *, 4> &Succs = Graph.Succs[F];
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        Function *Callee = directCalledFunction(dyn_cast<CallInst>(&I));
        if (Callee && CandidateSet.count(Callee) &&
            llvm::find(Succs, Callee) == Succs.end())
          Succs.push_back(Callee);
      }
    }
  }
  return Graph;
}

struct TarjanState {
  unsigned NextIndex = 0;
  DenseMap<Function *, unsigned> Index;
  DenseMap<Function *, unsigned> LowLink;
  SmallVector<Function *, 16> Stack;
  DenseSet<Function *> OnStack;
  SmallVector<SCCInfo, 8> SCCs;
};

static void tarjanVisit(Function *F, const CallGraphInfo &Graph,
                        TarjanState &State) {
  State.Index[F] = State.NextIndex;
  State.LowLink[F] = State.NextIndex;
  ++State.NextIndex;
  State.Stack.push_back(F);
  State.OnStack.insert(F);

  auto It = Graph.Succs.find(F);
  if (It != Graph.Succs.end()) {
    for (Function *Succ : It->second) {
      if (!State.Index.count(Succ)) {
        tarjanVisit(Succ, Graph, State);
        State.LowLink[F] = std::min(State.LowLink[F], State.LowLink[Succ]);
      } else if (State.OnStack.count(Succ)) {
        State.LowLink[F] = std::min(State.LowLink[F], State.Index[Succ]);
      }
    }
  }

  if (State.LowLink[F] != State.Index[F])
    return;

  SCCInfo SCC;
  while (true) {
    Function *Member = State.Stack.pop_back_val();
    State.OnStack.erase(Member);
    SCC.Members.push_back(Member);
    if (Member == F)
      break;
  }

  if (SCC.Members.size() > 1) {
    SCC.Recursive = true;
  } else {
    Function *Only = SCC.Members.front();
    auto GI = Graph.Succs.find(Only);
    if (GI != Graph.Succs.end())
      SCC.Recursive = llvm::find(GI->second, Only) != GI->second.end();
  }
  State.SCCs.push_back(std::move(SCC));
}

static SmallVector<SCCInfo, 8> findCandidateSCCs(ArrayRef<Function *> Candidates,
                                                 const CallGraphInfo &Graph) {
  TarjanState State;
  for (Function *F : Candidates)
    if (!State.Index.count(F))
      tarjanVisit(F, Graph, State);
  return std::move(State.SCCs);
}

static DenseSet<Function *> recursiveMembers(ArrayRef<SCCInfo> SCCs) {
  DenseSet<Function *> Recursive;
  for (const SCCInfo &SCC : SCCs)
    if (SCC.Recursive)
      for (Function *F : SCC.Members)
        Recursive.insert(F);
  return Recursive;
}

static MFLAArtifacts createArtifacts(Module &M, uint64_t FrameSize,
                                     unsigned NumFunctions) {
  LLVMContext &Ctx = M.getContext();
  MFLAArtifacts A;

  auto *I8 = Type::getInt8Ty(Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *Ptr = PointerType::get(Ctx, 0);
  auto *FrameTy = ArrayType::get(I8, FrameSize);
  auto *RetContTy = ArrayType::get(Ptr, NumFunctions);

  A.Frame = new GlobalVariable(M, FrameTy, false, GlobalValue::InternalLinkage,
                               Constant::getNullValue(FrameTy),
                               "__yansollvm_mfla_frame");
  A.Frame->setAlignment(Align(16));

  A.RetCont = new GlobalVariable(
      M, RetContTy, false, GlobalValue::InternalLinkage,
      Constant::getNullValue(RetContTy), "__yansollvm_mfla_ret_cont");
  A.RetCont->setAlignment(Align(8));

  FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx), {I32}, false);
  A.Mega = Function::Create(FTy, GlobalValue::InternalLinkage,
                            "__yansollvm_mfla_main", M);
  A.Mega->addFnAttr(Attribute::NoInline);
  return A;
}

static Value *framePtr(IRBuilder<> &B, GlobalVariable *Frame, uint64_t Offset) {
  auto *FrameTy = cast<ArrayType>(Frame->getValueType());
  LLVMContext &Ctx = Frame->getContext();
  Value *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  Value *Off = ConstantInt::get(Type::getInt32Ty(Ctx), Offset);
  return B.CreateInBoundsGEP(FrameTy, Frame, {Zero, Off});
}

static Value *storagePtr(IRBuilder<> &B, MFLAArtifacts &A, StorageRef Ref) {
  return framePtr(B, A.Frame, Ref.Offset);
}

static LoadInst *loadSlot(IRBuilder<> &B, Type *Ty, GlobalVariable *Frame,
                          uint64_t Offset, StringRef Name = "") {
  return B.CreateLoad(Ty, framePtr(B, Frame, Offset), Name);
}

static LoadInst *loadSlot(IRBuilder<> &B, Type *Ty, MFLAArtifacts &A,
                          StorageRef Ref, StringRef Name = "") {
  return B.CreateLoad(Ty, storagePtr(B, A, Ref), Name);
}

static Constant *remapConstant(Constant *C, Function *OldF, Function &Mega,
                               DenseMap<BasicBlock *, BasicBlock *> &BBMap,
                               ValueToValueMapTy &VMap) {
  if (!C)
    return nullptr;
  if (auto *BA = dyn_cast<BlockAddress>(C)) {
    if (BA->getFunction() == OldF) {
      BasicBlock *NewBB = BBMap.lookup(BA->getBasicBlock());
      if (NewBB)
        return BlockAddress::get(&Mega, NewBB);
    }
    return C;
  }
  if (auto *GV = dyn_cast<GlobalValue>(C)) {
    auto It = VMap.find(GV);
    if (It != VMap.end())
      if (auto *Mapped = dyn_cast<Constant>(It->second))
        return Mapped;
    return C;
  }
  if (isa<ConstantData>(C) || isa<ConstantAggregateZero>(C) || isa<UndefValue>(C))
    return C;
  bool Changed = false;
  SmallVector<Constant *, 8> Ops;
  for (Use &U : C->operands()) {
    auto *OpC = dyn_cast<Constant>(U.get());
    if (!OpC)
      return C;
    if (OpC == C)
      return C;
    Constant *Mapped = remapConstant(OpC, OldF, Mega, BBMap, VMap);
    Ops.push_back(Mapped);
    Changed |= Mapped != OpC;
  }
  if (!Changed)
    return C;
  if (auto *CE = dyn_cast<ConstantExpr>(C))
    return CE->getWithOperands(Ops);
  if (auto *CA = dyn_cast<ConstantArray>(C))
    return ConstantArray::get(CA->getType(), Ops);
  if (auto *CS = dyn_cast<ConstantStruct>(C))
    return ConstantStruct::get(CS->getType(), Ops);
  if (isa<ConstantVector>(C))
    return ConstantVector::get(Ops);
  return C;
}

static GlobalVariable *remapGlobalInitializerForFunction(
    GlobalVariable &GV, Function *OldF, Function &Mega,
    DenseMap<BasicBlock *, BasicBlock *> &BBMap, ValueToValueMapTy &VMap,
    MFLAArtifacts &A) {
  if (!GV.hasInitializer() || !GV.hasLocalLinkage() ||
      !constantContainsBlockAddress(GV.getInitializer()) ||
      !constantUsesOnlyBlockAddressesInFunction(GV.getInitializer(), *OldF))
    return nullptr;

  Constant *Init = remapConstant(GV.getInitializer(), OldF, Mega, BBMap, VMap);
  auto *NewGV = new GlobalVariable(
      *Mega.getParent(), GV.getValueType(), GV.isConstant(),
      GlobalValue::InternalLinkage, Init, (GV.getName() + ".mfla").str(),
      nullptr, GV.getThreadLocalMode(), GV.getAddressSpace(),
      GV.isExternallyInitialized());
  NewGV->copyAttributesFrom(&GV);
  NewGV->setLinkage(GlobalValue::InternalLinkage);
  NewGV->setName((GV.getName() + ".mfla").str());
  VMap[&GV] = NewGV;
  A.RemappedGlobals.push_back(NewGV);
  return NewGV;
}

static void remapLocalBlockAddressGlobals(Function *OldF, Function &Mega,
                                          DenseMap<BasicBlock *, BasicBlock *> &BBMap,
                                          ValueToValueMapTy &VMap,
                                          MFLAArtifacts &A) {
  for (GlobalVariable &GV : OldF->getParent()->globals())
    remapGlobalInitializerForFunction(GV, OldF, Mega, BBMap, VMap, A);
}

static Value *mapValueForUse(Value *V, IRBuilder<> &B, ValueToValueMapTy &VMap,
                             const FunctionLayout &Layout, MFLAArtifacts &A,
                             Function *OldF = nullptr,
                             DenseMap<BasicBlock *, BasicBlock *> *BBMap = nullptr) {
  if (auto *C = dyn_cast<Constant>(V))
    if (OldF && BBMap)
      return remapConstant(C, OldF, *A.Mega, *BBMap, VMap);
  if (auto *Call = dyn_cast<CallInst>(V)) {
    auto It = Layout.CallResultOffsets.find(Call);
    if (It != Layout.CallResultOffsets.end())
      return loadSlot(B, Call->getType(), A, It->second, "mfla.call.use");
  }
  if (auto *I = dyn_cast<Instruction>(V)) {
    auto It = Layout.SpilledValueOffsets.find(I);
    if (It != Layout.SpilledValueOffsets.end())
      return loadSlot(B, I->getType(), A, It->second, I->getName());
  }
  if (auto *Arg = dyn_cast<Argument>(V)) {
    auto It = Layout.ArgOffsetFor.find(Arg);
    if (It != Layout.ArgOffsetFor.end())
      return loadSlot(B, Arg->getType(), A, It->second, Arg->getName());
  }
  return MapValue(V, VMap, RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
}

static void storeSlot(IRBuilder<> &B, Value *V, GlobalVariable *Frame,
                      uint64_t Offset) {
  B.CreateStore(V, framePtr(B, Frame, Offset));
}

static void storeSlot(IRBuilder<> &B, Value *V, MFLAArtifacts &A,
                      StorageRef Ref) {
  B.CreateStore(V, storagePtr(B, A, Ref));
}

static Value *retContPtr(IRBuilder<> &B, GlobalVariable *RetCont,
                         unsigned EntryID) {
  auto *ContTy = cast<ArrayType>(RetCont->getValueType());
  Value *Zero = ConstantInt::get(Type::getInt32Ty(RetCont->getContext()), 0);
  Value *Index = ConstantInt::get(Type::getInt32Ty(RetCont->getContext()), EntryID);
  return B.CreateInBoundsGEP(ContTy, RetCont, {Zero, Index});
}

static bool reachesFunction(Function *From, Function *To,
                            const CallGraphInfo &Graph,
                            DenseSet<Function *> &Seen) {
  auto It = Graph.Succs.find(From);
  if (It == Graph.Succs.end())
    return false;
  for (Function *Succ : It->second) {
    if (Succ == To)
      return true;
    if (!Seen.insert(Succ).second)
      continue;
    if (reachesFunction(Succ, To, Graph, Seen))
      return true;
  }
  return false;
}

static bool reachesFunction(Function *From, Function *To,
                            const CallGraphInfo &Graph) {
  DenseSet<Function *> Seen;
  Seen.insert(From);
  return reachesFunction(From, To, Graph, Seen);
}

static bool functionsInterfere(Function *A, Function *B,
                               const CallGraphInfo &Graph) {
  if (A == B)
    return true;
  return reachesFunction(A, B, Graph) || reachesFunction(B, A, Graph);
}

static void addBaseToRef(StorageRef &Ref, uint64_t Base) { Ref.Offset += Base; }

static void addBaseToLayout(FunctionLayout &L, uint64_t Base) {
  for (StorageRef &Ref : L.ArgOffsets)
    addBaseToRef(Ref, Base);
  for (auto &Entry : L.ArgOffsetFor)
    addBaseToRef(Entry.second, Base);
  if (L.RetOffset.Ty)
    addBaseToRef(L.RetOffset, Base);
  for (auto &Entry : L.PhiOffsets)
    addBaseToRef(Entry.second, Base);
  for (auto &Entry : L.CallResultOffsets)
    addBaseToRef(Entry.second, Base);
  for (auto &Entry : L.SpilledValueOffsets)
    addBaseToRef(Entry.second, Base);
}

static bool isInternalCallee(CallInst *Call,
                             const DenseMap<Function *, unsigned> &CandidateIDs) {
  Function *Callee = directCalledFunction(Call);
  return Callee && CandidateIDs.count(Callee);
}

static bool isCPSLoweredCall(CallInst *Call,
                             const DenseMap<Function *, unsigned> &CandidateIDs) {
  return isInternalCallee(Call, CandidateIDs);
}

static FramePlan makeFramePlan(ArrayRef<Function *> Candidates,
                               const DenseMap<Function *, unsigned> &CandidateIDs,
                               CallGraphInfo CandidateGraph,
                               const CallGraphInfo &FullGraph,
                               const DataLayout &DL) {
  FramePlan Plan;
  SmallVector<StaticLocalPlan, 8> Locals;

  for (unsigned I = 0, E = Candidates.size(); I != E; ++I) {
    Function *F = Candidates[I];
    FunctionLayout L;
    L.EntryID = I;
    L.Owner = F;
    uint64_t NextOffset = 0;

    for (Argument &Arg : F->args()) {
      uint64_t Offset = reserveSlot(NextOffset, Arg.getType(), DL);
      StorageRef Ref = staticRef(Arg.getType(), Offset);
      L.ArgOffsets.push_back(Ref);
      L.ArgOffsetFor[&Arg] = Ref;
    }
    if (!F->getReturnType()->isVoidTy()) {
      uint64_t Offset = reserveSlot(NextOffset, F->getReturnType(), DL);
      L.RetOffset = staticRef(F->getReturnType(), Offset);
    }
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          if (isCPSLoweredCall(Call, CandidateIDs) && !Call->getType()->isVoidTy()) {
            uint64_t Offset = reserveSlot(NextOffset, Call->getType(), DL);
            L.CallResultOffsets[Call] = staticRef(Call->getType(), Offset);
            continue;
          }
        }
        auto *Phi = dyn_cast<PHINode>(&I);
        if (Phi) {
          uint64_t Offset = reserveSlot(NextOffset, Phi->getType(), DL);
          L.PhiOffsets[Phi] = staticRef(Phi->getType(), Offset);
          continue;
        }
        if (!I.getType()->isVoidTy()) {
          uint64_t Offset = reserveSlot(NextOffset, I.getType(), DL);
          L.SpilledValueOffsets[&I] = staticRef(I.getType(), Offset);
        }
      }
    }

    StaticLocalPlan Local;
    Local.F = F;
    Local.Layout = std::move(L);
    Local.LocalSize = std::max<uint64_t>(1, llvm::alignTo(NextOffset, 16));
    Locals.push_back(std::move(Local));
  }

  DenseSet<Function *> CandidateSet;
  for (Function *F : Candidates)
    CandidateSet.insert(F);
  for (StaticLocalPlan &Local : Locals) {
    bool HasNativeInternalCall = false;
    auto It = FullGraph.Succs.find(Local.F);
    if (It != FullGraph.Succs.end()) {
      for (Function *Callee : It->second) {
        if (!CandidateSet.count(Callee)) {
          HasNativeInternalCall = true;
          break;
        }
      }
    }
    if (!HasNativeInternalCall)
      continue;
    for (Function *Other : Candidates) {
      if (Other == Local.F)
        continue;
      if (functionsInterfere(Local.F, Other, CandidateGraph))
        continue;
      if (reachesFunction(Local.F, Other, FullGraph))
        CandidateGraph.Succs[Local.F].push_back(Other);
    }
  }

  llvm::sort(Locals, [](const StaticLocalPlan &A, const StaticLocalPlan &B) {
    if (A.LocalSize != B.LocalSize)
      return A.LocalSize > B.LocalSize;
    return A.F->getName() < B.F->getName();
  });

  struct Color {
    uint64_t Base = 0;
    uint64_t Size = 0;
    SmallVector<Function *, 4> Occupants;
  };
  SmallVector<Color, 8> Colors;

  for (StaticLocalPlan &Local : Locals) {
    Color *Chosen = nullptr;
    for (Color &C : Colors) {
      bool Interferes = false;
      for (Function *Other : C.Occupants) {
        if (functionsInterfere(Local.F, Other, CandidateGraph)) {
          Interferes = true;
          break;
        }
      }
      if (!Interferes) {
        Chosen = &C;
        break;
      }
    }
    if (!Chosen) {
      Color C;
      C.Base = Plan.FrameSize;
      C.Size = Local.LocalSize;
      Plan.FrameSize += C.Size;
      Colors.push_back(std::move(C));
      Chosen = &Colors.back();
    } else {
      Chosen->Size = std::max(Chosen->Size, Local.LocalSize);
    }
    Local.Base = Chosen->Base;
    Chosen->Occupants.push_back(Local.F);
  }

  for (StaticLocalPlan &Local : Locals) {
    addBaseToLayout(Local.Layout, Local.Base);
    Plan.Layouts[Local.F] = std::move(Local.Layout);
  }
  Plan.FrameSize = std::max<uint64_t>(1, llvm::alignTo(Plan.FrameSize, 16));
  return Plan;
}

static Instruction *cloneMapped(Instruction &I, IRBuilder<> &B,
                                ValueToValueMapTy &VMap,
                                const FunctionLayout &Layout, MFLAArtifacts &A,
                                Function *OldF,
                                DenseMap<BasicBlock *, BasicBlock *> &BBMap) {
  Instruction *Clone = I.clone();
  RemapInstruction(Clone, VMap,
                   RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
  for (Use &U : I.operands()) {
    Value *Mapped = mapValueForUse(U.get(), B, VMap, Layout, A, OldF, &BBMap);
    if (Mapped)
      Clone->setOperand(U.getOperandNo(), Mapped);
  }
  VMap[&I] = Clone;
  return Clone;
}

static bool storeIncomingPhis(IRBuilder<> &B, BasicBlock *Pred,
                              BasicBlock *Succ, ValueToValueMapTy &VMap,
                              const FunctionLayout &Layout, MFLAArtifacts &A,
                              Function *OldF,
                              DenseMap<BasicBlock *, BasicBlock *> &BBMap) {
  for (Instruction &I : *Succ) {
    auto *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      break;
    Value *Incoming = Phi->getIncomingValueForBlock(Pred);
    Value *Mapped = mapValueForUse(Incoming, B, VMap, Layout, A, OldF, &BBMap);
    if (!Mapped)
      return false;
    auto It = Layout.PhiOffsets.find(Phi);
    if (It == Layout.PhiOffsets.end())
      return false;
    storeSlot(B, Mapped, A, It->second);
  }
  return true;
}

static IndirectBrInst *createStaticIndirectBr(Function &Mega, BasicBlock *From,
                                              BasicBlock *To) {
  auto *IB = IndirectBrInst::Create(BlockAddress::get(&Mega, To), 1, From);
  IB->addDestination(To);
  return IB;
}

static Value *muxInt(IRBuilder<> &B, Value *Cond, Value *TrueV, Value *FalseV) {
  Type *Ty = TrueV->getType();
  Value *Mask = B.CreateSExt(Cond, Ty);
  Value *NotMask = B.CreateNot(Mask);
  return B.CreateOr(B.CreateAnd(TrueV, Mask), B.CreateAnd(FalseV, NotMask));
}

static Value *muxPtr(IRBuilder<> &B, const DataLayout &DL, Value *Cond,
                     Value *TrueV, Value *FalseV) {
  Type *PtrIntTy = DL.getIntPtrType(TrueV->getType());
  Value *TrueI = B.CreatePtrToInt(TrueV, PtrIntTy);
  Value *FalseI = B.CreatePtrToInt(FalseV, PtrIntTy);
  return B.CreateIntToPtr(muxInt(B, Cond, TrueI, FalseI), TrueV->getType());
}

static Value *muxValue(IRBuilder<> &B, const DataLayout &DL, Value *Cond,
                       Value *TrueV, Value *FalseV) {
  Type *Ty = TrueV->getType();
  if (Ty->isIntegerTy())
    return muxInt(B, Cond, TrueV, FalseV);
  if (Ty->isPointerTy())
    return muxPtr(B, DL, Cond, TrueV, FalseV);
  return B.CreateSelect(Cond, TrueV, FalseV);
}

static bool muxIncomingPhis(IRBuilder<> &B, const DataLayout &DL, BasicBlock *Pred,
                            BasicBlock *Succ, Value *TakeEdge,
                            ValueToValueMapTy &VMap, const FunctionLayout &Layout,
                            MFLAArtifacts &A, Function *OldF,
                            DenseMap<BasicBlock *, BasicBlock *> &BBMap) {
  for (Instruction &I : *Succ) {
    auto *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      break;
    Value *Incoming = Phi->getIncomingValueForBlock(Pred);
    Value *Mapped = mapValueForUse(Incoming, B, VMap, Layout, A, OldF, &BBMap);
    if (!Mapped)
      return false;
    auto It = Layout.PhiOffsets.find(Phi);
    if (It == Layout.PhiOffsets.end())
      return false;
    Value *Old = loadSlot(B, Phi->getType(), A, It->second, "mfla.phi.old");
    Value *New = muxValue(B, DL, TakeEdge, Mapped, Old);
    storeSlot(B, New, A, It->second);
  }
  return true;
}

static bool hasPhiNodes(BasicBlock *BB) {
  return isa<PHINode>(&BB->front());
}

static bool createThreadedTerminator(
    IRBuilder<> &B, Function &Mega, Instruction *OldTerm,
    ValueToValueMapTy &VMap, DenseMap<BasicBlock *, BasicBlock *> &BBMap,
    BasicBlock *Exit, MFLAArtifacts &A, const FunctionLayout &Layout,
    DenseMap<Function *, SmallVector<IndirectBrInst *, 4>> &ReturnDispatches,
    Function *OldF) {
  BasicBlock *Pred = OldTerm->getParent();

  if (auto *Ret = dyn_cast<ReturnInst>(OldTerm)) {
    if (Value *RV = Ret->getReturnValue()) {
      Value *Mapped = mapValueForUse(RV, B, VMap, Layout, A, OldF, &BBMap);
      if (!Mapped)
        return false;
      storeSlot(B, Mapped, A, Layout.RetOffset);
    }
    Type *PtrTy = PointerType::get(Mega.getContext(), 0);
    Value *ContSlot = retContPtr(B, A.RetCont, Layout.EntryID);
    Value *Target = B.CreateLoad(PtrTy, ContSlot, "mfla.ret.cont");
    Value *HasContinuation = B.CreateICmpNE(
        Target, ConstantPointerNull::get(cast<PointerType>(PtrTy)));
    BasicBlock *Cur = B.GetInsertBlock();
    BasicBlock *ReturnBB = BasicBlock::Create(
        Mega.getContext(), Cur->getName() + ".ret.cont", &Mega, Exit);
    B.CreateCondBr(HasContinuation, ReturnBB, Exit);

    IRBuilder<> ReturnB(ReturnBB);
    ReturnB.CreateStore(ConstantPointerNull::get(cast<PointerType>(PtrTy)),
                        retContPtr(ReturnB, A.RetCont, Layout.EntryID));
    auto *End = IndirectBrInst::Create(Target, 0, ReturnBB);
    ReturnDispatches[Layout.Owner].push_back(End);
    return true;
  }

  auto *Br = dyn_cast<BranchInst>(OldTerm);
  if (!Br) {
    if (auto *IB = dyn_cast<IndirectBrInst>(OldTerm)) {
      Value *Target = mapValueForUse(IB->getAddress(), B, VMap, Layout, A,
                                     OldF, &BBMap);
      if (!Target)
        return false;
      auto *End = IndirectBrInst::Create(Target, IB->getNumDestinations(),
                                         B.GetInsertBlock());
      for (unsigned I = 0, E = IB->getNumDestinations(); I != E; ++I) {
        BasicBlock *NewDest = BBMap.lookup(IB->getDestination(I));
        if (!NewDest)
          return false;
        End->addDestination(NewDest);
      }
      return true;
    }

    auto *Sw = dyn_cast<SwitchInst>(OldTerm);
    if (!Sw)
      return false;
    Value *Cond = mapValueForUse(Sw->getCondition(), B, VMap, Layout, A,
                                 OldF, &BBMap);
    BasicBlock *OldDefault = Sw->getDefaultDest();
    BasicBlock *NewDefault = BBMap.lookup(OldDefault);
    if (!Cond || !NewDefault)
      return false;

    Value *Target = BlockAddress::get(&Mega, NewDefault);
    SmallVector<std::pair<BasicBlock *, Value *>, 8> SuccConds;
    SuccConds.push_back({OldDefault, ConstantInt::getFalse(Mega.getContext())});

    for (auto Case : Sw->cases()) {
      BasicBlock *OldSucc = Case.getCaseSuccessor();
      BasicBlock *NewSucc = BBMap.lookup(OldSucc);
      if (!NewSucc)
        return false;
      Value *TakeCase = B.CreateICmpEQ(Cond, Case.getCaseValue());
      Target = muxPtr(B, Mega.getParent()->getDataLayout(), TakeCase,
                      BlockAddress::get(&Mega, NewSucc), Target);

      bool Found = false;
      for (auto &Entry : SuccConds) {
        if (Entry.first == OldSucc) {
          Entry.second = B.CreateOr(Entry.second, TakeCase);
          Found = true;
          break;
        }
      }
      if (!Found)
        SuccConds.push_back({OldSucc, TakeCase});
    }

    Value *AnyCase = ConstantInt::getFalse(Mega.getContext());
    for (auto &Entry : SuccConds) {
      if (Entry.first == OldDefault)
        continue;
      AnyCase = B.CreateOr(AnyCase, Entry.second);
    }
    Value *TakeDefault = B.CreateNot(AnyCase);
    for (auto &Entry : SuccConds)
      if (Entry.first == OldDefault)
        Entry.second = TakeDefault;

    SmallVector<BasicBlock *, 8> Destinations;
    auto addDest = [&](BasicBlock *BB) {
      BasicBlock *NewSucc = BBMap.lookup(BB);
      if (!NewSucc)
        return false;
      if (llvm::find(Destinations, NewSucc) == Destinations.end())
        Destinations.push_back(NewSucc);
      return true;
    };
    if (!addDest(OldDefault))
      return false;
    for (auto Case : Sw->cases())
      if (!addDest(Case.getCaseSuccessor()))
        return false;

    for (auto &Entry : SuccConds) {
      if (hasPhiNodes(Entry.first)) {
        if (!muxIncomingPhis(B, Mega.getParent()->getDataLayout(), Pred,
                             Entry.first, Entry.second, VMap, Layout, A,
                             OldF, BBMap))
          return false;
      }
    }

    auto *End = IndirectBrInst::Create(Target, Destinations.size(), B.GetInsertBlock());
    for (BasicBlock *Dest : Destinations)
      End->addDestination(Dest);
    return true;
  }

  if (Br->isUnconditional()) {
    BasicBlock *OldSucc = Br->getSuccessor(0);
    BasicBlock *NewSucc = BBMap.lookup(OldSucc);
    if (!NewSucc || !storeIncomingPhis(B, Pred, OldSucc, VMap, Layout, A,
                                       OldF, BBMap))
      return false;
    createStaticIndirectBr(Mega, B.GetInsertBlock(), NewSucc);
    return true;
  }

  Value *Cond = mapValueForUse(Br->getCondition(), B, VMap, Layout, A,
                               OldF, &BBMap);
  BasicBlock *OldTrue = Br->getSuccessor(0);
  BasicBlock *OldFalse = Br->getSuccessor(1);
  BasicBlock *TrueBB = BBMap.lookup(OldTrue);
  BasicBlock *FalseBB = BBMap.lookup(OldFalse);
  if (!Cond || !TrueBB || !FalseBB)
    return false;

  Value *TrueAddr = BlockAddress::get(&Mega, TrueBB);
  Value *FalseAddr = BlockAddress::get(&Mega, FalseBB);
  Value *Target = muxPtr(B, Mega.getParent()->getDataLayout(), Cond, TrueAddr, FalseAddr);
  if (!muxIncomingPhis(B, Mega.getParent()->getDataLayout(), Pred, OldTrue,
                       Cond, VMap, Layout, A, OldF, BBMap))
    return false;
  Value *NotCond = B.CreateNot(Cond);
  if (!muxIncomingPhis(B, Mega.getParent()->getDataLayout(), Pred, OldFalse,
                       NotCond, VMap, Layout, A, OldF, BBMap))
    return false;
  auto *End = IndirectBrInst::Create(Target, 2, B.GetInsertBlock());
  End->addDestination(TrueBB);
  End->addDestination(FalseBB);
  return true;
}

struct LoweringContext {
  Function &Mega;
  MFLAArtifacts &Artifacts;
  DenseMap<Function *, FunctionLayout> &Layouts;
  const DenseMap<Function *, unsigned> &CandidateIDs;
  DenseMap<Function *, BasicBlock *> &EntryBlockFor;
  BasicBlock *Exit = nullptr;
  DenseMap<Function *, SmallVector<BasicBlock *, 4>> &ContinuationsByCallee;
};

static bool lowerInternalCall(CallInst *Call, IRBuilder<> &B,
                              ValueToValueMapTy &VMap,
                              const FunctionLayout &CallerLayout,
                              DenseMap<BasicBlock *, BasicBlock *> &TerminatorBlockFor,
                              BasicBlock &OldBB, LoweringContext &LCtx) {
  Function *Callee = directCalledFunction(Call);
  if (!Callee || !LCtx.CandidateIDs.count(Callee))
    return false;

  const FunctionLayout &CalleeLayout = LCtx.Layouts[Callee];
  unsigned ArgNo = 0;
  for (Value *Arg : Call->args()) {
    Value *MappedArg =
        mapValueForUse(Arg, B, VMap, CallerLayout, LCtx.Artifacts);
    if (!MappedArg || ArgNo >= CalleeLayout.ArgOffsets.size())
      return false;
    storeSlot(B, MappedArg, LCtx.Artifacts, CalleeLayout.ArgOffsets[ArgNo++]);
  }

  BasicBlock *Cont = BasicBlock::Create(LCtx.Mega.getContext(),
                                        "mfla.call.cont", &LCtx.Mega,
                                        LCtx.Exit);
  LCtx.ContinuationsByCallee[Callee].push_back(Cont);
  Value *ContAddr = BlockAddress::get(&LCtx.Mega, Cont);
  B.CreateStore(ContAddr,
                retContPtr(B, LCtx.Artifacts.RetCont, CalleeLayout.EntryID));
  auto *Jump = IndirectBrInst::Create(
      BlockAddress::get(&LCtx.Mega, LCtx.EntryBlockFor[Callee]), 1,
      B.GetInsertBlock());
  Jump->addDestination(LCtx.EntryBlockFor[Callee]);

  B.SetInsertPoint(Cont);
  TerminatorBlockFor[&OldBB] = Cont;
  if (!Call->getType()->isVoidTy()) {
    Value *RetVal = loadSlot(B, Call->getType(), LCtx.Artifacts,
                             CalleeLayout.RetOffset, "mfla.call.ret");
    auto It = CallerLayout.CallResultOffsets.find(Call);
    if (It == CallerLayout.CallResultOffsets.end())
      return false;
    storeSlot(B, RetVal, LCtx.Artifacts, It->second);
    VMap[Call] = RetVal;
  }
  return true;
}

static bool
buildStructuralMega(MFLAArtifacts &A, ArrayRef<Function *> Candidates,
                    DenseMap<Function *, FunctionLayout> &Layouts,
                    const DenseMap<Function *, unsigned> &CandidateIDs) {
  Function &Mega = *A.Mega;
  Module &M = *Mega.getParent();
  LLVMContext &Ctx = Mega.getContext();
  Argument *EntryID = Mega.getArg(0);
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", &Mega);
  BasicBlock *Exit = BasicBlock::Create(Ctx, "exit", &Mega);
  DenseMap<Function *, SmallVector<BasicBlock *, 4>> ContinuationsByCallee;
  DenseMap<Function *, SmallVector<IndirectBrInst *, 4>> ReturnDispatches;

  IRBuilder<> ExitB(Exit);
  ExitB.CreateRetVoid();

  SmallVector<BasicBlock *, 16> EntryRegions;
  DenseMap<Function *, BasicBlock *> EntryBlockFor;
  DenseMap<Function *, DenseMap<BasicBlock *, BasicBlock *>> FunctionBBMaps;

  for (Function *F : Candidates) {
    DenseMap<BasicBlock *, BasicBlock *> &BBMap = FunctionBBMaps[F];
    for (BasicBlock &BB : *F) {
      std::string Name = (F->getName() + "." + BB.getName()).str();
      if (BB.getName().empty())
        Name = (F->getName() + ".bb").str();
      BasicBlock *NewBB = BasicBlock::Create(Ctx, Name, &Mega, Exit);
      BBMap[&BB] = NewBB;
      if (&BB == &F->getEntryBlock()) {
        EntryBlockFor[F] = NewBB;
        EntryRegions.push_back(NewBB);
      }
    }
  }

  IRBuilder<> EntryB(Entry);
  Type *PtrTy = PointerType::get(Ctx, 0);
  auto *TableTy = ArrayType::get(PtrTy, EntryRegions.size());
  std::vector<Constant *> Addrs;
  for (BasicBlock *R : EntryRegions)
    Addrs.push_back(BlockAddress::get(&Mega, R));
  A.TargetTable = new GlobalVariable(
      M, TableTy, true, GlobalValue::InternalLinkage,
      ConstantArray::get(TableTy, Addrs), "__yansollvm_mfla_targets");
  A.TargetTable->setAlignment(Align(8));
  Value *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  Value *Index = EntryB.CreateZExtOrTrunc(EntryID, Type::getInt32Ty(Ctx));
  Value *GEP = EntryB.CreateInBoundsGEP(TableTy, A.TargetTable, {Zero, Index});
  Value *Target = EntryB.CreateLoad(PtrTy, GEP);
  auto *IB = IndirectBrInst::Create(Target, EntryRegions.size(), Entry);
  for (BasicBlock *R : EntryRegions)
    IB->addDestination(R);

  LoweringContext LCtx{Mega, A, Layouts, CandidateIDs, EntryBlockFor, Exit,
                       ContinuationsByCallee};

  for (Function *F : Candidates) {
    ValueToValueMapTy VMap;
    DenseMap<BasicBlock *, BasicBlock *> &BBMap = FunctionBBMaps[F];
    remapLocalBlockAddressGlobals(F, Mega, BBMap, VMap, A);
    DenseMap<BasicBlock *, BasicBlock *> TerminatorBlockFor;
    const FunctionLayout &L = Layouts[F];

    for (BasicBlock &BB : *F) {
      BasicBlock *NewBB = BBMap[&BB];
      TerminatorBlockFor[&BB] = NewBB;
      IRBuilder<> B(NewBB);
      if (&BB != &F->getEntryBlock())
        B.SetInsertPoint(NewBB);
      for (Instruction &I : BB) {
        if (I.isTerminator())
          break;
        if (auto *Phi = dyn_cast<PHINode>(&I)) {
          auto It = L.PhiOffsets.find(Phi);
          if (It == L.PhiOffsets.end())
            return false;
          VMap[Phi] =
              loadSlot(B, Phi->getType(), A, It->second, Phi->getName());
          continue;
        }
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          if (isCPSLoweredCall(Call, CandidateIDs)) {
            if (!lowerInternalCall(Call, B, VMap, L, TerminatorBlockFor, BB,
                                   LCtx))
              return false;
            continue;
          }
        }
        Instruction *Clone = cloneMapped(I, B, VMap, L, A, F, BBMap);
        B.Insert(Clone);
        if (!I.getType()->isVoidTy()) {
          auto It = L.SpilledValueOffsets.find(&I);
          if (It == L.SpilledValueOffsets.end())
            return false;
          storeSlot(B, Clone, A, It->second);
        }
      }
    }

    for (BasicBlock &BB : *F) {
      BasicBlock *NewBB = TerminatorBlockFor.lookup(&BB);
      if (!NewBB)
        return false;
      if (NewBB->getTerminator())
        continue;
      IRBuilder<> B(NewBB);
      if (!createThreadedTerminator(B, Mega, BB.getTerminator(), VMap, BBMap,
                                    Exit, A, L, ReturnDispatches, F))
        return false;
    }
  }

  for (auto &Entry : ReturnDispatches)
    for (IndirectBrInst *IB : Entry.second)
      for (BasicBlock *Cont : ContinuationsByCallee[Entry.first])
        IB->addDestination(Cont);

  return true;
}

static void markNoUnwindIfNoThrowingCalls(Function &F) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (CB->mayThrow())
          return;
  F.addFnAttr(Attribute::NoUnwind);
}

static void sanitizeWrapperAttributes(Function &F) {
  F.removeFnAttr(Attribute::AlwaysInline);
  F.removeFnAttr(Attribute::InlineHint);
  F.removeFnAttr(Attribute::NoCallback);
  F.removeFnAttr(Attribute::NoFree);
  F.removeFnAttr(Attribute::NoRecurse);
  F.removeFnAttr(Attribute::NoSync);
  F.removeFnAttr(Attribute::NoUnwind);
  F.removeFnAttr(Attribute::ReadNone);
  F.removeFnAttr(Attribute::ReadOnly);
  F.removeFnAttr(Attribute::WillReturn);
  F.removeFnAttr(Attribute::MustProgress);
  F.removeFnAttr(Attribute::Memory);
}

static void rewriteAsWrapper(Function &F, MFLAArtifacts &A,
                             const FunctionLayout &L) {
  LLVMContext &Ctx = F.getContext();
  sanitizeWrapperAttributes(F);
  SmallVector<Argument *, 8> Args;
  for (Argument &Arg : F.args())
    Args.push_back(&Arg);

  F.dropAllReferences();
  while (!F.empty())
    F.begin()->eraseFromParent();

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", &F);
  IRBuilder<> B(Entry);
  for (unsigned I = 0, E = Args.size(); I != E; ++I)
    storeSlot(B, Args[I], A, L.ArgOffsets[I]);
  B.CreateStore(
      ConstantPointerNull::get(PointerType::get(Ctx, 0)),
      retContPtr(B, A.RetCont, L.EntryID));
  B.CreateCall(A.Mega, {ConstantInt::get(Type::getInt32Ty(Ctx), L.EntryID)});
  if (F.getReturnType()->isVoidTy())
    B.CreateRetVoid();
  else
    B.CreateRet(loadSlot(B, F.getReturnType(), A, L.RetOffset, ""));
}
} // namespace

PreservedAnalyses MFLAPass::run(Module &M, ModuleAnalysisManager &) {
  if (!Enabled)
    return PreservedAnalyses::all();

  std::vector<Function *> InitialCandidates;
  for (Function &F : M) {
    if (!isCandidate(F))
      continue;
    InitialCandidates.push_back(&F);
  }

  CallGraphInfo InitialGraph = buildCandidateCallGraph(InitialCandidates);
  SmallVector<SCCInfo, 8> InitialSCCs =
      findCandidateSCCs(InitialCandidates, InitialGraph);
  DenseSet<Function *> Recursive = recursiveMembers(InitialSCCs);
  for (Function *F : InitialCandidates) {
    if (Recursive.count(F))
      YANSO_WARN_FUNCTION(PassName, *F, "recursive SCC not supported");
  }
  std::vector<Function *> NonRecursiveCandidates;
  DenseMap<Function *, unsigned> NonRecursiveIDs;
  for (Function *F : InitialCandidates) {
    if (Recursive.count(F))
      continue;
    NonRecursiveIDs[F] = NonRecursiveCandidates.size();
    NonRecursiveCandidates.push_back(F);
  }

  std::vector<Function *> Candidates;
  DenseMap<Function *, unsigned> CandidateIDs;
  for (Function *F : NonRecursiveCandidates) {
    if (!supportsCurrentLowering(*F, NonRecursiveIDs))
      continue;
    CandidateIDs[F] = Candidates.size();
    Candidates.push_back(F);
  }

  if (Candidates.size() < 2) {
    YANSO_WARN_MODULE(PassName, M, "fewer than two eligible functions");
    return PreservedAnalyses::all();
  }

  CallGraphInfo CandidateGraph = buildCandidateCallGraph(Candidates);
  FramePlan Plan = makeFramePlan(Candidates, CandidateIDs, CandidateGraph,
                                 InitialGraph, M.getDataLayout());
  MFLAArtifacts Artifacts = createArtifacts(M, Plan.FrameSize, Candidates.size());
  if (!buildStructuralMega(Artifacts, Candidates, Plan.Layouts, CandidateIDs)) {
    Artifacts.eraseFromParent();
    return PreservedAnalyses::none();
  }
  markNoUnwindIfNoThrowingCalls(*Artifacts.Mega);

  for (Function *F : Candidates)
    rewriteAsWrapper(*F, Artifacts, Plan.Layouts[F]);

  return PreservedAnalyses::none();
}
