#include "MFLAPass.h"
#include "Utils.h"
#include "YANSOllvmCommon.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
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

struct MFLAArtifacts {
  Function *Mega = nullptr;
  GlobalVariable *Frame = nullptr;
  GlobalVariable *RetCont = nullptr;
  GlobalVariable *TargetTable = nullptr;

  void eraseFromParent() {
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
    if (TargetTable) {
      TargetTable->eraseFromParent();
      TargetTable = nullptr;
    }
  }
};

struct FunctionLayout {
  unsigned EntryID = 0;
  Function *Owner = nullptr;
  SmallVector<uint64_t, 4> ArgOffsets;
  DenseMap<Argument *, uint64_t> ArgOffsetFor;
  uint64_t RetOffset = 0;
  DenseMap<PHINode *, uint64_t> PhiOffsets;
  DenseMap<CallInst *, uint64_t> CallResultOffsets;
  DenseMap<Instruction *, uint64_t> SpilledValueOffsets;
};

struct FramePlan {
  DenseMap<Function *, FunctionLayout> Layouts;
  uint64_t FrameSize = 0;
};

static bool hasUnsupportedIndirectTerminator(Function &F) {
  for (BasicBlock &BB : F) {
    Instruction *Term = BB.getTerminator();
    if (isa<IndirectBrInst>(Term) || isa<CallBrInst>(Term))
      return true;
  }
  return false;
}

static Function *directCalledFunction(CallInst *Call) {
  if (!Call)
    return nullptr;
  return Call->getCalledFunction();
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

static bool containsBlockAddress(Function &F) {
  for (BasicBlock &BB : F)
    if (BB.hasAddressTaken())
      return true;
  return false;
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
  if (hasUnsupportedIndirectTerminator(F)) {
    YANSO_ERROR_FUNCTION(PassName, F, "contains indirectbr/callbr");
    return false;
  }
  if (containsBlockAddress(F)) {
    YANSO_WARN_FUNCTION(PassName, F, "contains blockaddress constant");
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
                !Callee->hasFnAttribute(Attribute::NoUnwind)) {
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
        !isa<SwitchInst>(Term)) {
      YANSO_WARN_FUNCTION(
          PassName, F,
          "current lowering supports only branch, switch, and return terminators");
      return false;
    }
  }
  return true;
}

static void markRecursiveDFS(
    Function *Root, Function *Cur,
    const DenseMap<Function *, SmallVector<Function *, 4>> &Graph,
    DenseSet<Function *> &Visiting, DenseSet<Function *> &Recursive) {
  auto It = Graph.find(Cur);
  if (It == Graph.end())
    return;
  for (Function *Succ : It->second) {
    if (Succ == Root) {
      Recursive.insert(Root);
      continue;
    }
    if (!Visiting.insert(Succ).second)
      continue;
    markRecursiveDFS(Root, Succ, Graph, Visiting, Recursive);
    Visiting.erase(Succ);
  }
}

static DenseSet<Function *> findRecursiveFunctions(ArrayRef<Function *> Candidates) {
  DenseSet<Function *> CandidateSet;
  for (Function *F : Candidates)
    CandidateSet.insert(F);

  DenseMap<Function *, SmallVector<Function *, 4>> Graph;
  for (Function *F : Candidates) {
    SmallVector<Function *, 4> &Succs = Graph[F];
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        auto *Call = dyn_cast<CallInst>(&I);
        Function *Callee = directCalledFunction(Call);
        if (Callee && CandidateSet.count(Callee))
          Succs.push_back(Callee);
      }
    }
  }

  DenseSet<Function *> Recursive;
  for (Function *F : Candidates) {
    DenseSet<Function *> Visiting;
    Visiting.insert(F);
    markRecursiveDFS(F, F, Graph, Visiting, Recursive);
  }
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

static LoadInst *loadSlot(IRBuilder<> &B, Type *Ty, GlobalVariable *Frame,
                          uint64_t Offset, StringRef Name = "") {
  return B.CreateLoad(Ty, framePtr(B, Frame, Offset), Name);
}

static Value *mapValueForUse(Value *V, IRBuilder<> &B, ValueToValueMapTy &VMap,
                             const FunctionLayout &Layout,
                             GlobalVariable *Frame) {
  if (auto *Call = dyn_cast<CallInst>(V)) {
    auto It = Layout.CallResultOffsets.find(Call);
    if (It != Layout.CallResultOffsets.end())
      return loadSlot(B, Call->getType(), Frame, It->second, "mfla.call.use");
  }
  if (auto *I = dyn_cast<Instruction>(V)) {
    auto It = Layout.SpilledValueOffsets.find(I);
    if (It != Layout.SpilledValueOffsets.end())
      return loadSlot(B, I->getType(), Frame, It->second, I->getName());
  }
  if (auto *Arg = dyn_cast<Argument>(V)) {
    auto It = Layout.ArgOffsetFor.find(Arg);
    if (It != Layout.ArgOffsetFor.end())
      return loadSlot(B, Arg->getType(), Frame, It->second, Arg->getName());
  }
  return MapValue(V, VMap, RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
}

static void storeSlot(IRBuilder<> &B, Value *V, GlobalVariable *Frame,
                      uint64_t Offset) {
  B.CreateStore(V, framePtr(B, Frame, Offset));
}

static Value *retContPtr(IRBuilder<> &B, GlobalVariable *RetCont,
                         unsigned EntryID) {
  auto *ContTy = cast<ArrayType>(RetCont->getValueType());
  Value *Zero = ConstantInt::get(Type::getInt32Ty(RetCont->getContext()), 0);
  Value *Index = ConstantInt::get(Type::getInt32Ty(RetCont->getContext()), EntryID);
  return B.CreateInBoundsGEP(ContTy, RetCont, {Zero, Index});
}

static FramePlan makeFramePlan(ArrayRef<Function *> Candidates,
                               const DenseMap<Function *, unsigned> &CandidateIDs,
                               const DataLayout &DL) {
  FramePlan Plan;
  uint64_t NextOffset = 0;
  for (unsigned I = 0, E = Candidates.size(); I != E; ++I) {
    Function *F = Candidates[I];
    FunctionLayout L;
    L.EntryID = I;
    L.Owner = F;
    for (Argument &Arg : F->args()) {
      uint64_t Offset = reserveSlot(NextOffset, Arg.getType(), DL);
      L.ArgOffsets.push_back(Offset);
      L.ArgOffsetFor[&Arg] = Offset;
    }
    if (!F->getReturnType()->isVoidTy()) {
      L.RetOffset = reserveSlot(NextOffset, F->getReturnType(), DL);
    }
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          if (Function *Callee = directCalledFunction(Call)) {
            if (CandidateIDs.count(Callee) && !Call->getType()->isVoidTy()) {
              L.CallResultOffsets[Call] =
                  reserveSlot(NextOffset, Call->getType(), DL);
              continue;
            }
          }
        }
        auto *Phi = dyn_cast<PHINode>(&I);
        if (Phi) {
          L.PhiOffsets[Phi] = reserveSlot(NextOffset, Phi->getType(), DL);
          continue;
        }
        if (!I.getType()->isVoidTy()) {
          L.SpilledValueOffsets[&I] = reserveSlot(NextOffset, I.getType(), DL);
        }
      }
    }
    Plan.Layouts[F] = std::move(L);
  }
  Plan.FrameSize = std::max<uint64_t>(1, llvm::alignTo(NextOffset, 16));
  return Plan;
}

static Instruction *cloneMapped(Instruction &I, IRBuilder<> &B,
                                ValueToValueMapTy &VMap,
                                const FunctionLayout &Layout,
                                GlobalVariable *Frame) {
  Instruction *Clone = I.clone();
  RemapInstruction(Clone, VMap,
                   RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
  for (Use &U : I.operands()) {
    Value *Mapped = mapValueForUse(U.get(), B, VMap, Layout, Frame);
    if (Mapped)
      Clone->setOperand(U.getOperandNo(), Mapped);
  }
  VMap[&I] = Clone;
  return Clone;
}

static bool storeIncomingPhis(IRBuilder<> &B, BasicBlock *Pred,
                              BasicBlock *Succ, ValueToValueMapTy &VMap,
                              const FunctionLayout &Layout,
                              GlobalVariable *Frame) {
  for (Instruction &I : *Succ) {
    auto *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      break;
    Value *Incoming = Phi->getIncomingValueForBlock(Pred);
    Value *Mapped = mapValueForUse(Incoming, B, VMap, Layout, Frame);
    if (!Mapped)
      return false;
    auto It = Layout.PhiOffsets.find(Phi);
    if (It == Layout.PhiOffsets.end())
      return false;
    storeSlot(B, Mapped, Frame, It->second);
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

static Value *muxPtr(IRBuilder<> &B, Value *Cond, Value *TrueV, Value *FalseV) {
  Type *I64 = Type::getInt64Ty(TrueV->getContext());
  Value *TrueI = B.CreatePtrToInt(TrueV, I64);
  Value *FalseI = B.CreatePtrToInt(FalseV, I64);
  return B.CreateIntToPtr(muxInt(B, Cond, TrueI, FalseI), TrueV->getType());
}

static Value *muxValue(IRBuilder<> &B, Value *Cond, Value *TrueV,
                       Value *FalseV) {
  Type *Ty = TrueV->getType();
  if (Ty->isIntegerTy())
    return muxInt(B, Cond, TrueV, FalseV);
  if (Ty->isPointerTy())
    return muxPtr(B, Cond, TrueV, FalseV);
  return B.CreateSelect(Cond, TrueV, FalseV);
}

static bool muxIncomingPhis(IRBuilder<> &B, BasicBlock *Pred, BasicBlock *Succ,
                            Value *TakeEdge, ValueToValueMapTy &VMap,
                            const FunctionLayout &Layout,
                            GlobalVariable *Frame) {
  for (Instruction &I : *Succ) {
    auto *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      break;
    Value *Incoming = Phi->getIncomingValueForBlock(Pred);
    Value *Mapped = mapValueForUse(Incoming, B, VMap, Layout, Frame);
    if (!Mapped)
      return false;
    auto It = Layout.PhiOffsets.find(Phi);
    if (It == Layout.PhiOffsets.end())
      return false;
    Value *Old = loadSlot(B, Phi->getType(), Frame, It->second, "mfla.phi.old");
    Value *New = muxValue(B, TakeEdge, Mapped, Old);
    storeSlot(B, New, Frame, It->second);
  }
  return true;
}

static bool hasPhiNodes(BasicBlock *BB) {
  return isa<PHINode>(&BB->front());
}

static bool createThreadedTerminator(
    IRBuilder<> &B, Function &Mega, Instruction *OldTerm,
    ValueToValueMapTy &VMap, DenseMap<BasicBlock *, BasicBlock *> &BBMap,
    BasicBlock *Exit, GlobalVariable *Frame, GlobalVariable *RetCont,
    const FunctionLayout &Layout,
    DenseMap<Function *, SmallVector<IndirectBrInst *, 4>> &ReturnDispatches) {
  BasicBlock *Pred = OldTerm->getParent();

  if (auto *Ret = dyn_cast<ReturnInst>(OldTerm)) {
    if (Value *RV = Ret->getReturnValue()) {
      Value *Mapped = mapValueForUse(RV, B, VMap, Layout, Frame);
      if (!Mapped)
        return false;
      storeSlot(B, Mapped, Frame, Layout.RetOffset);
    }
    Type *PtrTy = PointerType::get(Mega.getContext(), 0);
    Value *ContSlot = retContPtr(B, RetCont, Layout.EntryID);
    Value *Target = B.CreateLoad(PtrTy, ContSlot, "mfla.ret.cont");
    Value *HasContinuation = B.CreateICmpNE(
        Target, ConstantPointerNull::get(cast<PointerType>(PtrTy)));
    BasicBlock *Cur = B.GetInsertBlock();
    BasicBlock *ReturnBB = BasicBlock::Create(
        Mega.getContext(), Cur->getName() + ".ret.cont", &Mega, Exit);
    B.CreateCondBr(HasContinuation, ReturnBB, Exit);

    IRBuilder<> ReturnB(ReturnBB);
    ReturnB.CreateStore(ConstantPointerNull::get(cast<PointerType>(PtrTy)),
                        retContPtr(ReturnB, RetCont, Layout.EntryID));
    auto *End = IndirectBrInst::Create(Target, 0, ReturnBB);
    ReturnDispatches[Layout.Owner].push_back(End);
    return true;
  }

  auto *Br = dyn_cast<BranchInst>(OldTerm);
  if (!Br) {
    auto *Sw = dyn_cast<SwitchInst>(OldTerm);
    if (!Sw)
      return false;
    Value *Cond = mapValueForUse(Sw->getCondition(), B, VMap, Layout, Frame);
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
      Target = muxPtr(B, TakeCase, BlockAddress::get(&Mega, NewSucc), Target);

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
        if (!muxIncomingPhis(B, Pred, Entry.first, Entry.second, VMap, Layout,
                             Frame))
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
    if (!NewSucc || !storeIncomingPhis(B, Pred, OldSucc, VMap, Layout, Frame))
      return false;
    createStaticIndirectBr(Mega, B.GetInsertBlock(), NewSucc);
    return true;
  }

  Value *Cond = mapValueForUse(Br->getCondition(), B, VMap, Layout, Frame);
  BasicBlock *OldTrue = Br->getSuccessor(0);
  BasicBlock *OldFalse = Br->getSuccessor(1);
  BasicBlock *TrueBB = BBMap.lookup(OldTrue);
  BasicBlock *FalseBB = BBMap.lookup(OldFalse);
  if (!Cond || !TrueBB || !FalseBB)
    return false;

  Value *TrueAddr = BlockAddress::get(&Mega, TrueBB);
  Value *FalseAddr = BlockAddress::get(&Mega, FalseBB);
  Value *Target = muxPtr(B, Cond, TrueAddr, FalseAddr);
  if (!muxIncomingPhis(B, Pred, OldTrue, Cond, VMap, Layout, Frame))
    return false;
  Value *NotCond = B.CreateNot(Cond);
  if (!muxIncomingPhis(B, Pred, OldFalse, NotCond, VMap, Layout, Frame))
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
        mapValueForUse(Arg, B, VMap, CallerLayout, LCtx.Artifacts.Frame);
    if (!MappedArg || ArgNo >= CalleeLayout.ArgOffsets.size())
      return false;
    storeSlot(B, MappedArg, LCtx.Artifacts.Frame,
              CalleeLayout.ArgOffsets[ArgNo++]);
  }

  BasicBlock *Cont = BasicBlock::Create(LCtx.Mega.getContext(),
                                        "mfla.call.cont", &LCtx.Mega,
                                        LCtx.Exit);
  LCtx.ContinuationsByCallee[Callee].push_back(Cont);
  B.CreateStore(BlockAddress::get(&LCtx.Mega, Cont),
                retContPtr(B, LCtx.Artifacts.RetCont, CalleeLayout.EntryID));
  auto *Jump = IndirectBrInst::Create(
      BlockAddress::get(&LCtx.Mega, LCtx.EntryBlockFor[Callee]), 1,
      B.GetInsertBlock());
  Jump->addDestination(LCtx.EntryBlockFor[Callee]);

  B.SetInsertPoint(Cont);
  TerminatorBlockFor[&OldBB] = Cont;
  if (!Call->getType()->isVoidTy()) {
    Value *RetVal = loadSlot(B, Call->getType(), LCtx.Artifacts.Frame,
                             CalleeLayout.RetOffset, "mfla.call.ret");
    auto It = CallerLayout.CallResultOffsets.find(Call);
    if (It == CallerLayout.CallResultOffsets.end())
      return false;
    storeSlot(B, RetVal, LCtx.Artifacts.Frame, It->second);
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
  BasicBlock *Trap = BasicBlock::Create(Ctx, "trap", &Mega);
  DenseMap<Function *, SmallVector<BasicBlock *, 4>> ContinuationsByCallee;
  DenseMap<Function *, SmallVector<IndirectBrInst *, 4>> ReturnDispatches;

  IRBuilder<> ExitB(Exit);
  ExitB.CreateRetVoid();

  yansollvm_create_trap_block(&Mega, Trap);

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
              loadSlot(B, Phi->getType(), A.Frame, It->second, Phi->getName());
          continue;
        }
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          if (Function *Callee = directCalledFunction(Call)) {
            if (CandidateIDs.count(Callee)) {
              if (!lowerInternalCall(Call, B, VMap, L, TerminatorBlockFor, BB,
                                     LCtx))
                return false;
              continue;
            }
          }
        }
        Instruction *Clone = cloneMapped(I, B, VMap, L, A.Frame);
        B.Insert(Clone);
        if (!I.getType()->isVoidTy()) {
          auto It = L.SpilledValueOffsets.find(&I);
          if (It == L.SpilledValueOffsets.end())
            return false;
          storeSlot(B, Clone, A.Frame, It->second);
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
                                    Exit, A.Frame, A.RetCont, L,
                                    ReturnDispatches))
        return false;
    }
  }

  for (auto &Entry : ReturnDispatches)
    for (IndirectBrInst *IB : Entry.second)
      for (BasicBlock *Cont : ContinuationsByCallee[Entry.first])
        IB->addDestination(Cont);

  return true;
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
    storeSlot(B, Args[I], A.Frame, L.ArgOffsets[I]);
  B.CreateStore(
      ConstantPointerNull::get(PointerType::get(Ctx, 0)),
      retContPtr(B, A.RetCont, L.EntryID));
  B.CreateCall(A.Mega, {ConstantInt::get(Type::getInt32Ty(Ctx), L.EntryID)});
  if (F.getReturnType()->isVoidTy())
    B.CreateRetVoid();
  else
    B.CreateRet(loadSlot(B, F.getReturnType(), A.Frame, L.RetOffset));
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

  DenseSet<Function *> Recursive = findRecursiveFunctions(InitialCandidates);
  std::vector<Function *> NonRecursiveCandidates;
  DenseMap<Function *, unsigned> NonRecursiveCandidateIDs;
  for (Function *F : InitialCandidates) {
    if (Recursive.count(F)) {
      YANSO_WARN_FUNCTION(PassName, *F, "recursive SCC not supported");
      continue;
    }
    NonRecursiveCandidateIDs[F] = NonRecursiveCandidates.size();
    NonRecursiveCandidates.push_back(F);
  }

  std::vector<Function *> Candidates;
  DenseMap<Function *, unsigned> CandidateIDs;
  for (Function *F : NonRecursiveCandidates) {
    if (!supportsCurrentLowering(*F, NonRecursiveCandidateIDs))
      continue;
    CandidateIDs[F] = Candidates.size();
    Candidates.push_back(F);
  }

  if (Candidates.size() < 2) {
    YANSO_WARN_MODULE(PassName, M, "fewer than two eligible functions");
    return PreservedAnalyses::all();
  }

  FramePlan Plan = makeFramePlan(Candidates, CandidateIDs, M.getDataLayout());
  MFLAArtifacts Artifacts = createArtifacts(M, Plan.FrameSize, Candidates.size());
  if (!buildStructuralMega(Artifacts, Candidates, Plan.Layouts, CandidateIDs)) {
    Artifacts.eraseFromParent();
    return PreservedAnalyses::none();
  }

  for (Function *F : Candidates)
    rewriteAsWrapper(*F, Artifacts, Plan.Layouts[F]);

  return PreservedAnalyses::none();
}
