#include "MFLAPass.h"
#include "CryptoUtils.h"
#include "MFLAInternal.h"
#include "Utils.h"
#include "YANSOllvmCommon.h"
#include "YANSOllvmSeed.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <cassert>
#include <unordered_set>
#include <vector>

using namespace llvm;
using namespace yansollvm::mfla;

namespace {
constexpr StringLiteral PassName = "mfla";

static cl::opt<unsigned> MFLAStaticFramePages(
    "mfla-static-frame-pages", cl::init(1), cl::Hidden,
    cl::desc("Preallocated frame pages for the MFLA static page backend"));
static cl::opt<unsigned> MFLAFramesPerPage(
    "mfla-frames-per-page", cl::init(16), cl::Hidden,
    cl::desc("Logical frame records per MFLA frame page"));
static cl::opt<unsigned> MFLAPageTableBlockEntries(
    "mfla-page-table-block-entries", cl::init(16), cl::Hidden,
    cl::desc("Page pointer entries per linked MFLA page-table block"));

struct FramePlan {
  DenseMap<Function *, FunctionLayout> Layouts;
  uint64_t FrameSize = 0;
  DenseSet<Function *> RecursiveMembers;
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

static uint64_t alignOffset(uint64_t Offset, Align Alignment) {
  return llvm::alignTo(Offset, Alignment.value());
}

// Visit every BlockAddress reachable through a constant tree. GlobalValues are
// opaque leaves (they are not descended into), matching how the rest of the
// pass treats cross-global references. This single walk backs every read-only
// blockaddress query below.
static void forEachBlockAddress(Constant *C,
                                function_ref<void(BlockAddress *)> Fn) {
  if (!C)
    return;
  if (auto *BA = dyn_cast<BlockAddress>(C)) {
    Fn(BA);
    return;
  }
  if (isa<GlobalValue>(C))
    return;
  for (Use &U : C->operands())
    if (auto *OpC = dyn_cast<Constant>(U.get()))
      if (OpC != C)
        forEachBlockAddress(OpC, Fn);
}

static bool constantContainsBlockAddress(Constant *C) {
  bool Found = false;
  forEachBlockAddress(C, [&](BlockAddress *) { Found = true; });
  return Found;
}

static bool constantUsesOnlyBlockAddressesInFunction(Constant *C, Function &F) {
  bool Ok = true;
  forEachBlockAddress(C, [&](BlockAddress *BA) {
    if (BA->getFunction() != &F)
      Ok = false;
  });
  return Ok;
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
    bool Foreign = false;
    forEachBlockAddress(C, [&](BlockAddress *BA) {
      HasBA = true;
      if (BA->getFunction() != &F)
        Foreign = true;
    });
    if (Foreign)
      return false;
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

static void collectBlockAddressOwners(Constant *C,
                                      SmallPtrSetImpl<Function *> &Owners) {
  forEachBlockAddress(C, [&](BlockAddress *BA) { Owners.insert(BA->getFunction()); });
}

static bool hasNonCandidateInstructionUser(
    GlobalVariable &GV, const DenseSet<Function *> &CandidateSet) {
  SmallVector<User *, 16> Work(GV.users());
  SmallPtrSet<User *, 16> Seen;

  while (!Work.empty()) {
    User *U = Work.pop_back_val();
    if (!Seen.insert(U).second)
      continue;

    if (auto *I = dyn_cast<Instruction>(U)) {
      Function *Owner = I->getFunction();
      if (!Owner || !CandidateSet.count(Owner))
        return true;
      continue;
    }

    if (auto *C = dyn_cast<Constant>(U)) {
      for (User *Next : C->users())
        Work.push_back(Next);
      continue;
    }

    for (User *Next : U->users())
      Work.push_back(Next);
  }

  return false;
}

static bool pruneCandidatesForBlockAddressGlobals(
    Module &M, DenseSet<Function *> &CandidateSet) {
  SmallPtrSet<Function *, 8> ToRemove;

  for (GlobalVariable &GV : M.globals()) {
    if (!GV.hasInitializer() || !GV.hasLocalLinkage())
      continue;

    SmallPtrSet<Function *, 8> Owners;
    collectBlockAddressOwners(GV.getInitializer(), Owners);
    if (Owners.empty())
      continue;

    bool HasCandidateOwner = false;
    bool HasNonCandidateOwner = false;
    for (Function *Owner : Owners) {
      if (CandidateSet.count(Owner))
        HasCandidateOwner = true;
      else
        HasNonCandidateOwner = true;
    }
    if (!HasCandidateOwner)
      continue;

    bool UnsafeUse = hasNonCandidateInstructionUser(GV, CandidateSet);
    if (!HasNonCandidateOwner && !UnsafeUse)
      continue;

    for (Function *Owner : Owners) {
      if (!CandidateSet.count(Owner))
        continue;
      ToRemove.insert(Owner);
      std::string Reason = HasNonCandidateOwner
                               ? (Twine("blockaddress global '") + GV.getName() +
                                  "' mixes candidate and non-candidate labels")
                                     .str()
                               : (Twine("blockaddress global '") + GV.getName() +
                                  "' is used by non-candidate code")
                                     .str();
      YANSO_WARN_SKIP_FUNCTION(PassName, *Owner, Reason);
    }
  }

  for (Function *F : ToRemove)
    CandidateSet.erase(F);
  return !ToRemove.empty();
}

static void collectBlockAddresses(Constant *C, Function &F,
                                  SmallVectorImpl<BasicBlock *> &Blocks) {
  SmallPtrSet<BasicBlock *, 8> Found;
  forEachBlockAddress(C, [&](BlockAddress *BA) {
    if (BA->getFunction() == &F)
      Found.insert(BA->getBasicBlock());
  });
  for (BasicBlock &BB : F)
    if (Found.contains(&BB))
      Blocks.push_back(&BB);
}

static SmallVector<BasicBlock *, 8> collectAddressTakenBlocks(Function &F) {
  SmallPtrSet<BasicBlock *, 8> Seen;
  SmallVector<BasicBlock *, 8> Blocks;
  auto Add = [&](BasicBlock *BB) {
    if (BB && Seen.insert(BB).second)
      Blocks.push_back(BB);
  };

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      for (Use &U : I.operands()) {
        auto *C = dyn_cast<Constant>(U.get());
        if (!C)
          continue;
        SmallVector<BasicBlock *, 8> Found;
        collectBlockAddresses(C, F, Found);
        for (BasicBlock *AddrBB : Found)
          Add(AddrBB);
      }
    }
    if (auto *IB = dyn_cast<IndirectBrInst>(BB.getTerminator()))
      for (unsigned I = 0, E = IB->getNumDestinations(); I != E; ++I)
        Add(IB->getDestination(I));
  }

  for (GlobalVariable &GV : F.getParent()->globals()) {
    if (!GV.hasInitializer() || !GV.hasLocalLinkage() ||
        !constantUsesOnlyBlockAddressesInFunction(GV.getInitializer(), F))
      continue;
    SmallVector<BasicBlock *, 8> Found;
    collectBlockAddresses(GV.getInitializer(), F, Found);
    for (BasicBlock *AddrBB : Found)
      Add(AddrBB);
  }

  return Blocks;
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
    YANSO_WARN_SKIP_FUNCTION(PassName, F, "vararg function");
    return false;
  }
  if (hasUnsupportedABIAttrs(F)) {
    YANSO_WARN_SKIP_FUNCTION(PassName, F,
                             "unsupported ABI parameter attribute");
    return false;
  }
  if (hasUnsupportedCallBr(F)) {
    YANSO_ERROR_SKIP_FUNCTION(PassName, F, "contains callbr");
    return false;
  }
  if (!hasSupportedBlockAddressDomain(F)) {
    YANSO_WARN_SKIP_FUNCTION(PassName, F, "unsupported blockaddress use");
    return false;
  }
  if (yansollvm_has_dynamic_stack_state(F)) {
    YANSO_WARN_SKIP_FUNCTION(PassName, F, "dynamic stack state");
    return false;
  }
  if (F.empty())
    return false;
  return true;
}


static bool supportsCurrentLowering(
    Function &F, const DenseMap<Function *, unsigned> &CandidateIDs) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  Type *RetTy = F.getReturnType();
  if (!RetTy->isVoidTy() && !isFrameScalar(RetTy)) {
    YANSO_WARN_SKIP_FUNCTION(
        PassName, F, "current lowering supports only scalar return functions");
    return false;
  }
  for (Argument &Arg : F.args()) {
    if (!isFrameScalar(Arg.getType())) {
      YANSO_WARN_SKIP_FUNCTION(
          PassName, F, "current lowering supports only scalar arguments");
      return false;
    }
  }
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<PHINode>(&I)) {
        auto *Phi = cast<PHINode>(&I);
        if (!isFrameScalar(Phi->getType())) {
          YANSO_WARN_SKIP_FUNCTION(
              PassName, F, "current lowering supports only scalar PHI nodes");
          return false;
        }
        continue;
      }
      if (auto *Call = dyn_cast<CallInst>(&I)) {
        Function *RawCallee = Call->getCalledFunction();
        if (RawCallee && CandidateIDs.count(RawCallee) &&
            RawCallee->getReturnType() != Call->getType()) {
          YANSO_WARN_SKIP_FUNCTION(PassName, F,
                                   "callee wrapper return type changed");
          return false;
        }
        if (Function *Callee = directCalledFunction(Call)) {
          if (CandidateIDs.count(Callee)) {
            if (!Call->getType()->isVoidTy() &&
                !isFrameScalar(Call->getType())) {
              YANSO_WARN_SKIP_FUNCTION(PassName, F,
                                       "current lowering supports only void or "
                                       "scalar internal call results");
              return false;
            }
            if (Call->mayThrow() && !Callee->hasFnAttribute(Attribute::NoUnwind) &&
                !F.hasFnAttribute(Attribute::NoUnwind)) {
              YANSO_WARN_SKIP_FUNCTION(
                  PassName, F,
                  "current lowering does not support throwing internal calls");
              return false;
            }
          } else if (!Call->getType()->isVoidTy() && !isFrameScalar(Call->getType())) {
            YANSO_WARN_SKIP_FUNCTION(
                PassName, F,
                "current lowering supports only void or scalar boundary call results");
            return false;
          }
        }
      }
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (DL.getTypeAllocSize(AI->getAllocatedType()).isScalable()) {
          YANSO_WARN_SKIP_FUNCTION(
              PassName, F, "current lowering does not support scalable allocas");
          return false;
        }
        continue;
      }
      if (!I.getType()->isVoidTy() && !isFrameScalar(I.getType())) {
        YANSO_WARN_SKIP_FUNCTION(
            PassName, F,
            "current lowering supports only scalar instruction results");
        return false;
      }
      if (isa<LandingPadInst>(&I) || isa<CatchPadInst>(&I) ||
          isa<CleanupPadInst>(&I)) {
        YANSO_WARN_SKIP_FUNCTION(
            PassName, F, "current lowering keeps EH for a later region pass");
        return false;
      }
    }
    Instruction *Term = BB.getTerminator();
    if (!isa<ReturnInst>(Term) && !isa<BranchInst>(Term) &&
        !isa<SwitchInst>(Term) && !isa<IndirectBrInst>(Term)) {
      YANSO_WARN_SKIP_FUNCTION(PassName, F,
                               "current lowering supports only branch, switch, "
                               "indirectbr, and return terminators");
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

static void emitTrapBlock(IRBuilder<> &B, Function &F, BasicBlock *InsertBefore,
                          StringRef OkName, StringRef TrapName,
                          Value *ShouldTrap,
                          function_ref<void(BasicBlock *)> AssignOk = nullptr) {
  LLVMContext &Ctx = F.getContext();
  BasicBlock *TrapBB = BasicBlock::Create(Ctx, TrapName, &F, InsertBefore);
  BasicBlock *OkBB = BasicBlock::Create(Ctx, OkName, &F, InsertBefore);
  if (AssignOk)
    AssignOk(OkBB);
  B.CreateCondBr(ShouldTrap, TrapBB, OkBB);

  IRBuilder<> TrapB(TrapBB);
  Function *Trap = Intrinsic::getOrInsertDeclaration(F.getParent(), Intrinsic::trap);
  TrapB.CreateCall(Trap);
  TrapB.CreateUnreachable();

  B.SetInsertPoint(OkBB);
}

static FunctionCallee getOrInsertNoUnwindFunc(Module &M, StringRef Name,
                                             FunctionType *FTy) {
  FunctionCallee Callee = M.getOrInsertFunction(Name, FTy);
  if (auto *F = dyn_cast<Function>(Callee.getCallee()))
    F->addFnAttr(Attribute::NoUnwind);
  return Callee;
}

static void buildFramePageResolver(Module &M, MFLAArtifacts &A) {
  if (A.FrameBackend != FramePageBackendKind::Malloc)
    return;

  LLVMContext &Ctx = M.getContext();
  Type *I8 = Type::getInt8Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *PtrTy = PointerType::get(Ctx, 0);
  FunctionType *FTy = FunctionType::get(PtrTy, {PtrTy, I64}, false);
  Function *F = Function::Create(FTy, GlobalValue::InternalLinkage,
                                 "__yansollvm_mfla_resolve_frame_page", M);
  F->addFnAttr(Attribute::NoInline);
  F->setName("__yansollvm_mfla_resolve_frame_page");
  A.FramePageResolver = F;

  FunctionType *MallocTy = FunctionType::get(PtrTy, {I64}, false);
  FunctionCallee Malloc = getOrInsertNoUnwindFunc(M, "malloc", MallocTy);
  auto emitMalloc = [&](IRBuilder<> &IB, uint64_t Size, Twine Name) -> Value * {
    return IB.CreateCall(Malloc, {constI64(Ctx, Size)}, Name);
  };

  Argument *CtxArg = F->getArg(0);
  Argument *PageIndexArg = F->getArg(1);
  CtxArg->setName("ctx");
  PageIndexArg->setName("page.index");

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *AllocHeadBB = BasicBlock::Create(Ctx, "alloc.head", F);
  BasicBlock *WalkBB = BasicBlock::Create(Ctx, "walk", F);
  BasicBlock *FoundBB = BasicBlock::Create(Ctx, "found.block", F);
  BasicBlock *AllocNextBB = BasicBlock::Create(Ctx, "alloc.next.block", F);
  BasicBlock *AdvanceBB = BasicBlock::Create(Ctx, "advance.block", F);
  BasicBlock *AllocPageBB = BasicBlock::Create(Ctx, "alloc.page", F);
  BasicBlock *ReturnBB = BasicBlock::Create(Ctx, "return", F);

  IRBuilder<> B(Entry);
  Value *HeadPtr = ctxBytePtr(B, CtxArg, A.FramePageTableHeadOffset);
  Value *Head = B.CreateLoad(PtrTy, HeadPtr, "mfla.pages.head");
  Value *NeedHead = B.CreateICmpEQ(
      Head, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "mfla.pages.need.head");
  B.CreateCondBr(NeedHead, AllocHeadBB, WalkBB);

  IRBuilder<> AllocB(AllocHeadBB);
  Value *HeadBlockPtr = emitMalloc(AllocB, A.PageTableBlockBytes,
                                  "mfla.page.table.block.ptr");
  Value *HeadNull = AllocB.CreateICmpEQ(
      HeadBlockPtr, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "mfla.page.table.block.null");
  emitTrapBlock(AllocB, *F, WalkBB, "mfla.page.table.block.ok",
                "mfla.page.table.block.oom", HeadNull);
  BasicBlock *HeadOkBB = AllocB.GetInsertBlock();
  AllocB.CreateMemSet(HeadBlockPtr, ConstantInt::get(I8, 0),
                      A.PageTableBlockBytes, Align(16));
  AllocB.CreateStore(HeadBlockPtr, HeadPtr);
  AllocB.CreateBr(WalkBB);

  IRBuilder<> WalkB(WalkBB);
  PHINode *Block = WalkB.CreatePHI(PtrTy, 3, "mfla.page.table.block");
  PHINode *BlockIndex = WalkB.CreatePHI(I64, 3, "mfla.page.table.index");
  Block->addIncoming(Head, Entry);
  Block->addIncoming(HeadBlockPtr, HeadOkBB);
  BlockIndex->addIncoming(constI64(Ctx, 0), Entry);
  BlockIndex->addIncoming(constI64(Ctx, 0), HeadOkBB);
  Value *TargetBlock = WalkB.CreateUDiv(
      PageIndexArg, constI64(Ctx, A.PageTableBlockEntries),
      "mfla.page.table.target");
  Value *AtBlock = WalkB.CreateICmpEQ(BlockIndex, TargetBlock,
                                      "mfla.page.table.at.block");
  WalkB.CreateCondBr(AtBlock, FoundBB, AllocNextBB);

  IRBuilder<> NextB(AllocNextBB);
  Value *NextPtr = NextB.CreateLoad(PtrTy, Block, "mfla.page.table.next");
  Value *NeedNext = NextB.CreateICmpEQ(
      NextPtr, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "mfla.page.table.need.next");
  BasicBlock *AllocNextRealBB = BasicBlock::Create(Ctx, "alloc.next.real", F);
  NextB.CreateCondBr(NeedNext, AllocNextRealBB, AdvanceBB);

  IRBuilder<> AllocNextB(AllocNextRealBB);
  Value *NextBlockPtr = emitMalloc(AllocNextB, A.PageTableBlockBytes,
                                   "mfla.page.table.next.block.ptr");
  Value *NextNull = AllocNextB.CreateICmpEQ(
      NextBlockPtr, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "mfla.page.table.next.block.null");
  emitTrapBlock(AllocNextB, *F, AdvanceBB, "mfla.page.table.next.block.ok",
                "mfla.page.table.next.block.oom", NextNull);
  BasicBlock *NextOkBB = AllocNextB.GetInsertBlock();
  AllocNextB.CreateMemSet(NextBlockPtr, ConstantInt::get(I8, 0),
                          A.PageTableBlockBytes, Align(16));
  AllocNextB.CreateStore(NextBlockPtr, Block);
  AllocNextB.CreateBr(AdvanceBB);

  IRBuilder<> AdvanceB(AdvanceBB);
  PHINode *NextBlock = AdvanceB.CreatePHI(PtrTy, 2,
                                          "mfla.page.table.next.block");
  NextBlock->addIncoming(NextPtr, AllocNextBB);
  NextBlock->addIncoming(NextBlockPtr, NextOkBB);
  Value *NextBlockIndex = AdvanceB.CreateAdd(BlockIndex, constI64(Ctx, 1),
                                             "mfla.page.table.index.next");
  AdvanceB.CreateBr(WalkBB);
  Block->addIncoming(NextBlock, AdvanceBB);
  BlockIndex->addIncoming(NextBlockIndex, AdvanceBB);

  IRBuilder<> SlotB(FoundBB);
  Value *Slot = SlotB.CreateURem(PageIndexArg,
                                 constI64(Ctx, A.PageTableBlockEntries),
                                 "mfla.page.table.slot");
  Value *SlotByteOff = SlotB.CreateAdd(
      constI64(Ctx, 8),
      SlotB.CreateMul(Slot, constI64(Ctx, M.getDataLayout().getPointerSize()),
                      "mfla.page.table.slot.off"));
  Value *SlotPtr = SlotB.CreateInBoundsGEP(I8, Block, SlotByteOff,
                                           "mfla.page.ptr.slot");
  Value *Page = SlotB.CreateLoad(PtrTy, SlotPtr, "mfla.page.ptr");
  Value *NeedPage = SlotB.CreateICmpEQ(
      Page, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "mfla.page.need");
  SlotB.CreateCondBr(NeedPage, AllocPageBB, ReturnBB);

  IRBuilder<> AllocPageB(AllocPageBB);
  Value *NewPage = emitMalloc(AllocPageB, A.FramePageSize,
                              "mfla.frame.page.new");
  Value *PageNull = AllocPageB.CreateICmpEQ(
      NewPage, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "mfla.frame.page.null");
  emitTrapBlock(AllocPageB, *F, ReturnBB, "mfla.frame.page.ok",
                "mfla.frame.page.oom", PageNull);
  BasicBlock *PageOkBB = AllocPageB.GetInsertBlock();
  AllocPageB.CreateStore(NewPage, SlotPtr);
  AllocPageB.CreateBr(ReturnBB);

  IRBuilder<> ReturnB(ReturnBB);
  PHINode *Result = ReturnB.CreatePHI(PtrTy, 2, "mfla.frame.page.result");
  Result->addIncoming(Page, FoundBB);
  Result->addIncoming(NewPage, PageOkBB);
  ReturnB.CreateRet(Result);
}

static bool isEffectivelyNoUnwind(CallBase &CB) {
  if (!CB.mayThrow())
    return true;
  if (Function *Callee = CB.getCalledFunction()) {
    if (Callee->hasFnAttribute(Attribute::NoUnwind))
      return true;
    StringRef Name = Callee->getName();
    if (Name == "malloc" || Name == "free")
      return true;
  }
  return false;
}

static MFLAArtifacts createArtifacts(Module &M, uint64_t FrameSize,
                                     unsigned NumFunctions) {
  LLVMContext &Ctx = M.getContext();
  const DataLayout &DL = M.getDataLayout();
  MFLAArtifacts A;

  auto *I64 = Type::getInt64Ty(Ctx);
  FrameLayoutBuilder CtxLayout(DL);
  A.CurrentFrameOffset =
      CtxLayout.reserve(StorageKind::FrameMetadata, I64).Offset;
  A.FrameTopOffset =
      CtxLayout.reserve(StorageKind::FrameMetadata, I64).Offset;
  A.FrameStride = std::max<uint64_t>(
      16, llvm::alignTo(FrameSize + FrameMetadataBytes, 16));
  A.FrameBackend = FramePageBackendKind::Malloc;
  A.StaticFramePages = std::max<unsigned>(1, MFLAStaticFramePages);
  A.FramesPerPage = std::max<unsigned>(1, MFLAFramesPerPage);
  A.ContinuationStride = 24;
  A.ContinuationSlots = std::max<uint64_t>(1, NumFunctions);
  uint64_t ContinuationBytes = A.ContinuationStride * A.ContinuationSlots;
  A.PageTableBlockEntries = std::max<unsigned>(1, MFLAPageTableBlockEntries);
  A.PageTableBlockBytes =
      8 + A.PageTableBlockEntries * DL.getPointerSize();
  A.FrameFreeHeadOffset = CtxLayout.reserve(StorageKind::FrameMetadata, I64).Offset;
  A.FramePageTableHeadOffset =
      CtxLayout.reserve(StorageKind::PageTableMetadata,
                        PointerType::get(Ctx, 0)).Offset;
  A.FrameArenaOffset = CtxLayout.frameSize();
  A.ContinuationOffset = alignOffset(A.FrameStride * A.FramesPerPage, Align(16));
  A.FramePageSize =
      llvm::alignTo(A.ContinuationOffset + ContinuationBytes * A.FramesPerPage,
                    16);
  if (A.FrameBackend == FramePageBackendKind::Static)
    CtxLayout.NextOffset =
        A.FrameArenaOffset + A.FramePageSize * A.StaticFramePages;
  else
    CtxLayout.NextOffset = A.FrameArenaOffset;
  A.ReturnedFrameOffset =
      CtxLayout.reserve(StorageKind::FrameMetadata, I64).Offset;
  A.StateOffset = CtxLayout.reserve(StorageKind::FrameMetadata, I64).Offset;
  A.CtxSize = CtxLayout.frameSize();

  YansoRNG SaltRNG(yanso_hash_string("state-key-salt",
                                      yanso_module_seed(M, PassName)));
  do {
    A.StateKeySalt = SaltRNG.next64();
  } while (!A.StateKeySalt);

  FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx),
                                        {PointerType::get(Ctx, 0), I64, I64},
                                        false);
  A.Mega = Function::Create(FTy, GlobalValue::InternalLinkage,
                            "__yansollvm_mfla_main", M);
  A.Mega->addFnAttr(Attribute::NoInline);
  buildFramePageResolver(M, A);
  return A;
}

// Rebuild a constant tree, replacing each BlockAddress via MapBlockAddress and
// each remapped global via GlobalRemaps/VMap. Returns null (fail closed) on any
// operand that cannot be mapped.
static Constant *remapConstant(
    Constant *C, ValueToValueMapTy &VMap,
    const DenseMap<GlobalVariable *, GlobalVariable *> *GlobalRemaps,
    function_ref<Constant *(BlockAddress *)> MapBlockAddress) {
  if (!C)
    return nullptr;
  if (auto *BA = dyn_cast<BlockAddress>(C))
    return MapBlockAddress(BA);
  if (auto *GV = dyn_cast<GlobalValue>(C)) {
    if (GlobalRemaps)
      if (auto *VarGV = dyn_cast<GlobalVariable>(GV)) {
        auto GlobalIt = GlobalRemaps->find(VarGV);
        if (GlobalIt != GlobalRemaps->end())
          return GlobalIt->second;
      }
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
      return nullptr;
    if (OpC == C)
      return C;
    Constant *Mapped = remapConstant(OpC, VMap, GlobalRemaps, MapBlockAddress);
    if (!Mapped)
      return nullptr;
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

static GlobalVariable *createRemappedGlobalShell(GlobalVariable &GV,
                                                Function &Mega,
                                                MFLAArtifacts &A) {
  auto *NewGV = new GlobalVariable(
      *Mega.getParent(), GV.getValueType(), GV.isConstant(),
      GlobalValue::InternalLinkage, UndefValue::get(GV.getValueType()),
      (GV.getName() + ".mfla").str(), nullptr, GV.getThreadLocalMode(),
      GV.getAddressSpace(), GV.isExternallyInitialized());
  NewGV->copyAttributesFrom(&GV);
  NewGV->setLinkage(GlobalValue::InternalLinkage);
  NewGV->setName((GV.getName() + ".mfla").str());
  A.GlobalRemaps[&GV] = NewGV;
  A.RemappedGlobals.push_back(NewGV);
  return NewGV;
}

static bool prepareModuleBlockAddressGlobals(
    Module &M, Function &Mega, ArrayRef<Function *> Candidates,
    DenseMap<Function *, DenseMap<BasicBlock *, BasicBlock *>> &FunctionBBMaps,
    DenseMap<Function *, DenseMap<BasicBlock *, BasicBlock *>> &AddrHelperForFunction,
    MFLAArtifacts &A) {
  DenseSet<Function *> CandidateSet;
  for (Function *F : Candidates)
    CandidateSet.insert(F);

  SmallVector<GlobalVariable *, 8> Globals;
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.hasInitializer() || !GV.hasLocalLinkage())
      continue;

    SmallPtrSet<Function *, 8> Owners;
    collectBlockAddressOwners(GV.getInitializer(), Owners);
    if (Owners.empty())
      continue;
    bool HasCandidateOwner = false;
    bool HasNonCandidateOwner = false;
    for (Function *Owner : Owners) {
      if (CandidateSet.count(Owner))
        HasCandidateOwner = true;
      else
        HasNonCandidateOwner = true;
    }
    if (!HasCandidateOwner)
      continue;
    if (HasNonCandidateOwner)
      return false;
    Globals.push_back(&GV);
  }

  for (GlobalVariable *GV : Globals)
    createRemappedGlobalShell(*GV, Mega, A);

  ValueToValueMapTy EmptyVMap;
  for (GlobalVariable *GV : Globals) {
    GlobalVariable *NewGV = A.GlobalRemaps.lookup(GV);
    if (!NewGV)
      return false;
    Constant *Init = remapConstant(
        GV->getInitializer(), EmptyVMap, &A.GlobalRemaps,
        [&](BlockAddress *BA) -> Constant * {
          auto FnIt = FunctionBBMaps.find(BA->getFunction());
          if (FnIt == FunctionBBMaps.end())
            return BA;
          auto HelperFnIt = AddrHelperForFunction.find(BA->getFunction());
          if (HelperFnIt != AddrHelperForFunction.end()) {
            auto HelperIt = HelperFnIt->second.find(BA->getBasicBlock());
            if (HelperIt != HelperFnIt->second.end())
              return BlockAddress::get(&Mega, HelperIt->second);
          }
          if (BasicBlock *NewBB = FnIt->second.lookup(BA->getBasicBlock()))
            return BlockAddress::get(&Mega, NewBB);
          return BA;
        });
    NewGV->setInitializer(Init);
  }
  return true;
}

static void commitModuleBlockAddressGlobals(MFLAArtifacts &A) {
  for (auto &Entry : A.GlobalRemaps)
    Entry.first->replaceAllUsesWith(Entry.second);
  SmallVector<GlobalVariable *, 8> OldGlobals;
  for (auto &Entry : A.GlobalRemaps)
    OldGlobals.push_back(Entry.first);
  for (GlobalVariable *GV : OldGlobals)
    if (GV->use_empty())
      GV->eraseFromParent();
  A.GlobalRemaps.clear();
}

static Value *mapValueForUse(Value *V, IRBuilder<> &B, ValueToValueMapTy &VMap,
                             DenseMap<Value *, Value *> *LocalMap,
                             const FunctionLayout &Layout, MFLAArtifacts &A,
                             Function *OldF = nullptr,
                             DenseMap<BasicBlock *, BasicBlock *> *BBMap = nullptr,
                             const DenseMap<BasicBlock *, BasicBlock *> *AddrHelperForOldBB = nullptr,
                             FrameRef *Frame = nullptr) {
  FrameRef CurFrame = Frame ? *Frame : currentFrame(B, A, Layout);
  if (LocalMap) {
    auto LocalIt = LocalMap->find(V);
    if (LocalIt != LocalMap->end())
      return LocalIt->second;
  }
  if (auto *C = dyn_cast<Constant>(V))
    if (OldF && BBMap)
      return remapConstant(
          C, VMap, &A.GlobalRemaps, [&](BlockAddress *BA) -> Constant * {
            if (BA->getFunction() != OldF)
              return BA;
            if (AddrHelperForOldBB) {
              auto HelperIt = AddrHelperForOldBB->find(BA->getBasicBlock());
              if (HelperIt != AddrHelperForOldBB->end())
                return BlockAddress::get(A.Mega, HelperIt->second);
            }
            if (BasicBlock *NewBB = BBMap->lookup(BA->getBasicBlock()))
              return BlockAddress::get(A.Mega, NewBB);
            return BA;
          });
  if (auto *Call = dyn_cast<CallInst>(V)) {
    auto It = Layout.CallResultOffsets.find(Call);
    if (It != Layout.CallResultOffsets.end())
      return loadFrameSlot(B, Call->getType(), A, CurFrame, It->second,
                           "mfla.call.use");
  }
  if (auto *Phi = dyn_cast<PHINode>(V)) {
    auto It = Layout.PhiOffsets.find(Phi);
    if (It != Layout.PhiOffsets.end())
      return loadFrameSlot(B, Phi->getType(), A, CurFrame, It->second,
                           Phi->getName());
  }
  if (auto *I = dyn_cast<Instruction>(V)) {
    auto It = Layout.SpilledValueOffsets.find(I);
    if (It != Layout.SpilledValueOffsets.end()) {
      if (isa<AllocaInst>(I))
        return frameSlotPtr(B, A, CurFrame, It->second);
      return loadFrameSlot(B, I->getType(), A, CurFrame, It->second,
                           I->getName());
    }
  }
  if (auto *Arg = dyn_cast<Argument>(V)) {
    auto It = Layout.ArgOffsetFor.find(Arg);
    if (It != Layout.ArgOffsetFor.end())
      return loadFrameSlot(B, Arg->getType(), A, CurFrame, It->second,
                           Arg->getName());
  }
  Value *Mapped = MapValue(V, VMap, RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
  if (Mapped == V && (isa<Instruction>(V) || isa<Argument>(V) || isa<BasicBlock>(V)))
    return nullptr;
  return Mapped;
}

static Value *keyForState(IRBuilder<> &B, MFLAArtifacts &A, Value *State) {
  return yanso_create_mix64_ir(State, constI64(A.Mega->getContext(), A.StateKeySalt),
                              B.GetInsertBlock(), *A.Mega->getParent());
}

static Constant *blockDeltaPlusKey(Function &Mega, BasicBlock *Anchor,
                                   BasicBlock *Target, Constant *Key) {
  Type *I64 = Type::getInt64Ty(Mega.getContext());
  auto *TargetI = ConstantExpr::getPtrToInt(BlockAddress::get(&Mega, Target), I64);
  auto *AnchorI = ConstantExpr::getPtrToInt(BlockAddress::get(&Mega, Anchor), I64);
  return ConstantExpr::getAdd(ConstantExpr::getSub(TargetI, AnchorI), Key);
}

static Constant *blockDeltaPlusKey(MFLAArtifacts &A, BasicBlock *Anchor,
                                   BasicBlock *Target, uint64_t State) {
  return blockDeltaPlusKey(*A.Mega, Anchor, Target, keyForState(A, State));
}

static Value *transitionState(IRBuilder<> &B, Value *CurState,
                              uint64_t FromState, uint64_t ToState) {
  LLVMContext &Ctx = B.getContext();
  Value *Next = B.CreateXor(CurState, constI64(Ctx, FromState));
  return B.CreateXor(Next, constI64(Ctx, ToState));
}

static Value *conditionalTransitionState(IRBuilder<> &B, Value *CurState,
                                         uint64_t FromState,
                                         uint64_t FalseState,
                                         uint64_t TrueState, Value *Cond) {
  LLVMContext &Ctx = B.getContext();
  Value *Next = transitionState(B, CurState, FromState, FalseState);
  Value *Mask = B.CreateSExt(Cond, Type::getInt64Ty(Ctx));
  Value *Diff = B.CreateAnd(Mask, constI64(Ctx, TrueState ^ FalseState));
  return B.CreateXor(Next, Diff);
}

static GlobalVariable *createEdgeConstant(MFLAArtifacts &A, BasicBlock *Anchor,
                                          BasicBlock *Target, uint64_t State) {
  Constant *Init = blockDeltaPlusKey(A, Anchor, Target, State);
  auto *GV = new GlobalVariable(*A.Mega->getParent(), Type::getInt64Ty(A.Mega->getContext()),
                                false, GlobalValue::PrivateLinkage, Init,
                                "__yansollvm_mfla_edge");
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(Align(8));
  A.EdgeConstants.push_back(GV);
  return GV;
}

static Value *encodedTarget(IRBuilder<> &B, Function &Mega, BasicBlock *Anchor,
                            Value *Encoded, Value *Key) {
  Type *I64 = Type::getInt64Ty(Mega.getContext());
  auto *AnchorI = ConstantExpr::getPtrToInt(BlockAddress::get(&Mega, Anchor), I64);
  Value *Delta = B.CreateSub(Encoded, Key, "mfla.delta");
  Value *Addr = B.CreateAdd(AnchorI, Delta, "mfla.addr");
  return B.CreateIntToPtr(Addr, PointerType::get(Mega.getContext(), 0),
                          "mfla.target");
}

static LoadInst *loadEdgeConstant(IRBuilder<> &B, MFLAArtifacts &A,
                                  BasicBlock *Anchor, BasicBlock *Target,
                                  uint64_t State, StringRef Name = "mfla.edge") {
  LoadInst *L = B.CreateLoad(Type::getInt64Ty(A.Mega->getContext()),
                             createEdgeConstant(A, Anchor, Target, State), Name);
  L->setVolatile(true);
  return L;
}

static Value *encodedTargetConst(IRBuilder<> &B, MFLAArtifacts &A,
                                 BasicBlock *Anchor, BasicBlock *Target,
                                 uint64_t State) {
  Value *Enc = loadEdgeConstant(B, A, Anchor, Target, State);
  return encodedTarget(B, *A.Mega, Anchor, Enc, keyForState(A, State));
}


struct MFLAStateAllocator {
  YansoRNG RNG;
  std::unordered_set<uint64_t> Used;

  explicit MFLAStateAllocator(Module &M) : RNG(yanso_module_seed(M, PassName)) {}

  uint64_t next() {
    uint64_t State = 0;
    do {
      State = RNG.next64();
    } while (!State || Used.count(State));
    Used.insert(State);
    return State;
  }

  uint64_t assign(BasicBlock *BB, DenseMap<BasicBlock *, uint64_t> &StateFor) {
    auto It = StateFor.find(BB);
    if (It != StateFor.end())
      return It->second;
    uint64_t State = next();
    StateFor[BB] = State;
    return State;
  }
};

constexpr unsigned MFLAAnchorGroupSize = 8;

struct AnchorPool {
  SmallVector<SmallVector<BasicBlock *, MFLAAnchorGroupSize>, 32> Groups;
  DenseMap<BasicBlock *, unsigned> GroupFor;
};

static void addAnchorConstraint(
    SmallVectorImpl<SmallVector<BasicBlock *, MFLAAnchorGroupSize>> &Constraints,
    ArrayRef<BasicBlock *> Blocks) {
  SmallVector<BasicBlock *, MFLAAnchorGroupSize> Unique;
  for (BasicBlock *BB : Blocks) {
    if (!BB || llvm::find(Unique, BB) != Unique.end())
      continue;
    Unique.push_back(BB);
  }
  if (Unique.size() > 1)
    Constraints.push_back(std::move(Unique));
}

static void buildAnchorPool(
    SmallVectorImpl<BasicBlock *> &Targets,
    ArrayRef<SmallVector<BasicBlock *, MFLAAnchorGroupSize>> Constraints,
    MFLAStateAllocator &StateAlloc, AnchorPool &AP) {
  StateAlloc.RNG.shuffle(Targets);

  EquivalenceClasses<BasicBlock *> Classes;
  for (BasicBlock *BB : Targets)
    Classes.insert(BB);
  for (const auto &Group : Constraints) {
    if (Group.empty())
      continue;
    for (BasicBlock *BB : Group)
      Classes.unionSets(Group.front(), BB);
  }

  DenseMap<BasicBlock *, SmallVector<BasicBlock *, MFLAAnchorGroupSize>> ByRoot;
  SmallVector<BasicBlock *, 32> Roots;
  for (BasicBlock *BB : Targets) {
    BasicBlock *Root = *Classes.findLeader(BB);
    auto It = ByRoot.find(Root);
    if (It == ByRoot.end()) {
      Roots.push_back(Root);
      It = ByRoot.try_emplace(Root).first;
    }
    It->second.push_back(BB);
  }

  for (BasicBlock *Root : Roots) {
    SmallVector<BasicBlock *, MFLAAnchorGroupSize> &Group = ByRoot[Root];
    unsigned Index = AP.Groups.size();
    AP.Groups.push_back(std::move(Group));
    for (BasicBlock *BB : AP.Groups.back())
      AP.GroupFor[BB] = Index;
  }
}

static BasicBlock *pickAnchorForTarget(BasicBlock *Target, const AnchorPool &AP,
                                       MFLAStateAllocator &StateAlloc) {
  auto It = AP.GroupFor.find(Target);
  if (It == AP.GroupFor.end())
    report_fatal_error("missing mfla anchor target");
  const auto &Group = AP.Groups[It->second];
  return Group[StateAlloc.RNG.range(static_cast<uint32_t>(Group.size()))];
}

static uint64_t stateFor(BasicBlock *BB,
                         DenseMap<BasicBlock *, uint64_t> &StateFor) {
  auto It = StateFor.find(BB);
  if (It == StateFor.end())
    report_fatal_error("missing mfla state for basic block");
  return It->second;
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

enum class CallDomain {
  InternalEnter,
  NativeOperation,
};

enum class FramePolicy {
  ReuseCurrentFrame,
  PushFrame,
};

struct CallsitePlan {
  CallDomain Domain = CallDomain::NativeOperation;
  bool TailReuse = false;
  FramePolicy Frame = FramePolicy::ReuseCurrentFrame;
  Function *Callee = nullptr;
};

static bool usesDynamicFrame(const CallsitePlan &Plan) {
  return Plan.Frame == FramePolicy::PushFrame;
}

static bool isTailReturnCall(CallInst *Call) {
  if (!Call || (!Call->isMustTailCall() && !Call->isTailCall()))
    return false;
  auto *Ret = dyn_cast<ReturnInst>(Call->getParent()->getTerminator());
  return Ret && Ret->getReturnValue() == Call;
}

static CallsitePlan planCallsite(CallInst *Call, Function *Caller,
                                 const DenseMap<Function *, unsigned> &CandidateIDs,
                                 const DenseSet<Function *> &RecursiveMembers) {
  Function *Callee = directCalledFunction(Call);
  if (!Callee || !CandidateIDs.count(Callee))
    return {};

  bool SelfTail = Caller && Callee == Caller && isTailReturnCall(Call);
  bool RecursiveCallee = RecursiveMembers.count(Callee);
  CallsitePlan Plan;
  Plan.Domain = CallDomain::InternalEnter;
  Plan.Callee = Callee;
  if (SelfTail)
    Plan.TailReuse = true;
  if ((RecursiveCallee) && !SelfTail)
    Plan.Frame = FramePolicy::PushFrame;
  return Plan;
}

static bool isInternalEnter(const CallsitePlan &Plan) {
  return Plan.Domain == CallDomain::InternalEnter && Plan.Callee;
}

static DenseMap<CallInst *, CallsitePlan>
makeCallsitePlans(ArrayRef<Function *> Candidates,
                  const DenseMap<Function *, unsigned> &CandidateIDs,
                  const DenseSet<Function *> &RecursiveMembers) {
  DenseMap<CallInst *, CallsitePlan> Plans;
  for (Function *F : Candidates)
    for (BasicBlock &BB : *F)
      for (Instruction &I : BB)
        if (auto *Call = dyn_cast<CallInst>(&I))
          Plans[Call] = planCallsite(Call, F, CandidateIDs, RecursiveMembers);
  return Plans;
}

static bool isInternalEnterCall(
    CallInst *Call, const DenseMap<CallInst *, CallsitePlan> &CallsitePlans) {
  auto It = CallsitePlans.find(Call);
  return It != CallsitePlans.end() && isInternalEnter(It->second);
}

static bool isPushContinuationInternalCall(
    CallInst *Call, const DenseMap<CallInst *, CallsitePlan> &CallsitePlans) {
  auto It = CallsitePlans.find(Call);
  return It != CallsitePlans.end() && isInternalEnter(It->second) &&
         !It->second.TailReuse;
}

static FramePlan
makeFramePlan(ArrayRef<Function *> Candidates,
              const DenseMap<Function *, unsigned> &CandidateIDs,
              const DenseMap<CallInst *, CallsitePlan> &CallsitePlans,
              const CallGraphInfo &CandidateGraph,
              const DenseSet<Function *> &RecursiveMembers,
              const DataLayout &DL) {
  FramePlan Plan;
  Plan.RecursiveMembers = RecursiveMembers;
  SmallVector<StaticLocalPlan, 8> Locals;

  for (unsigned I = 0, E = Candidates.size(); I != E; ++I) {
    Function *F = Candidates[I];
    FunctionLayout L;
    L.ContSlot = {I};
    L.Owner = F;
    FrameLayoutBuilder Builder(DL);
    for (Argument &Arg : F->args()) {
      StorageRef Ref = Builder.reserve(StorageKind::Argument, Arg.getType());
      L.ArgOffsets.push_back(Ref);
      L.ArgOffsetFor[&Arg] = Ref;
    }
    if (!F->getReturnType()->isVoidTy())
      L.RetOffset = Builder.reserve(StorageKind::ReturnValue, F->getReturnType());
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (auto *AI = dyn_cast<AllocaInst>(&I)) {
          TypeSize Size = DL.getTypeAllocSize(AI->getAllocatedType());
          if (Size.isScalable())
            continue;
          L.SpilledValueOffsets[AI] = Builder.reserveFixedObject(
              AI->getType(), AI->getAllocatedType(), AI->getAlign());
          continue;
        }
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          if (!Call->getType()->isVoidTy()) {
            Type *ResultTy = Call->getType();
            if (isFrameScalar(ResultTy))
              L.CallResultOffsets[Call] =
                  Builder.reserve(StorageKind::CallResult, ResultTy);
            continue;
          }
        }
        if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
          switch (II->getIntrinsicID()) {
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
            continue;
          default:
            break;
          }
        }
        auto *Phi = dyn_cast<PHINode>(&I);
        if (Phi) {
          L.PhiOffsets[Phi] = Builder.reserve(StorageKind::Phi, Phi->getType());
          continue;
        }
        if (!I.getType()->isVoidTy())
          L.SpilledValueOffsets[&I] = Builder.reserve(StorageKind::Spill, I.getType());
      }
    }

    StaticLocalPlan Local;
    Local.F = F;
    Local.Layout = std::move(L);
    Local.LocalSize = Builder.frameSize();
    Locals.push_back(std::move(Local));
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
    if (Plan.RecursiveMembers.count(Local.F)) {
      Local.Base = Plan.FrameSize;
      Plan.FrameSize += Local.LocalSize;
      continue;
    }
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
                                DenseMap<Value *, Value *> &LocalMap,
                                const FunctionLayout &Layout, MFLAArtifacts &A,
                                Function *OldF,
                                DenseMap<BasicBlock *, BasicBlock *> &BBMap,
                                const DenseMap<BasicBlock *, BasicBlock *> *AddrHelperForOldBB = nullptr,
                                FrameRef *Frame = nullptr) {
  Instruction *Clone = I.clone();
  for (Use &U : I.operands()) {
    Value *Mapped = mapValueForUse(U.get(), B, VMap, &LocalMap, Layout, A,
                                   OldF, &BBMap, AddrHelperForOldBB, Frame);
    if (!Mapped) {
      Clone->deleteValue();
      return nullptr;
    }
    Clone->setOperand(U.getOperandNo(), Mapped);
  }
  return Clone;
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

static bool writeIncomingPhis(
    IRBuilder<> &B, const DataLayout &DL, BasicBlock *Pred, BasicBlock *Succ,
    Value *TakeEdge, ValueToValueMapTy &VMap, const FunctionLayout &Layout,
    MFLAArtifacts &A, Function *OldF,
    DenseMap<BasicBlock *, BasicBlock *> &BBMap,
    const DenseMap<BasicBlock *, BasicBlock *> *AddrHelperForOldBB = nullptr) {
  // PHIs assign in parallel: each incoming value is read before the edge.
  // Since a PHI slot may read a sibling PHI's slot, compute all store values
  // before committing any, or a later PHI sees an earlier one's new value.
  SmallVector<std::pair<StorageRef, Value *>, 8> Pending;
  FrameRef CurFrame = currentFrame(B, A, Layout);
  for (Instruction &I : *Succ) {
    auto *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      break;
    Value *Incoming = Phi->getIncomingValueForBlock(Pred);
    Value *Mapped = mapValueForUse(Incoming, B, VMap, nullptr, Layout, A, OldF,
                                   &BBMap, AddrHelperForOldBB, nullptr);
    if (!Mapped)
      return false;
    auto It = Layout.PhiOffsets.find(Phi);
    if (It == Layout.PhiOffsets.end())
      return false;
    Value *ToStore = Mapped;
    if (TakeEdge) {
      Value *Old = loadFrameSlot(B, Phi->getType(), A, CurFrame, It->second,
                                 "mfla.phi.old");
      ToStore = muxValue(B, DL, TakeEdge, Mapped, Old);
    }
    Pending.emplace_back(It->second, ToStore);
  }
  for (auto &Entry : Pending)
    storeFrameSlot(B, Entry.second, A, CurFrame, Entry.first);
  return true;
}

static bool hasPhiNodes(BasicBlock *BB) {
  return BB && !BB->empty() && isa<PHINode>(&BB->front());
}

static void createAddressTakenHelper(Function &Mega, MFLAArtifacts &A,
                                     BasicBlock *Helper, BasicBlock *RealTarget,
                                     DenseMap<BasicBlock *, uint64_t> &StateFor,
                                     MFLAStateAllocator &StateAlloc,
                                     AnchorPool &Anchors) {
  IRBuilder<> B(Helper);
  uint64_t TargetState = stateFor(RealTarget, StateFor);
  BasicBlock *EdgeAnchor = pickAnchorForTarget(RealTarget, Anchors, StateAlloc);
  storeState(B, A, constI64(Mega.getContext(), TargetState));
  Value *Target = encodedTarget(
      B, Mega, EdgeAnchor, loadEdgeConstant(B, A, EdgeAnchor, RealTarget, TargetState),
      keyForState(A, TargetState));
  auto *End = IndirectBrInst::Create(Target, 1, Helper);
  End->addDestination(RealTarget);
}

struct LoweringContext {
  Function &Mega;
  MFLAArtifacts &Artifacts;
  DenseMap<Function *, FunctionLayout> &Layouts;
  const DenseMap<Function *, unsigned> &CandidateIDs;
  const DenseMap<CallInst *, CallsitePlan> &CallsitePlans;
  DenseMap<Function *, BasicBlock *> &EntryBlockFor;
  DenseMap<Function *, DenseMap<BasicBlock *, BasicBlock *>>
      &AddrHelperForFunction;
  DenseMap<BasicBlock *, uint64_t> &StateFor;
  MFLAStateAllocator &StateAlloc;
  BasicBlock *Anchor = nullptr;
  BasicBlock *Exit = nullptr;
  DenseMap<Function *, SmallVector<BasicBlock *, 4>> &ContinuationsByCallee;
  AnchorPool &Anchors;
};

struct FunctionTerminatorContext {
  Function *OldF = nullptr;
  ValueToValueMapTy &VMap;
  DenseMap<BasicBlock *, BasicBlock *> &BBMap;
  const FunctionLayout &Layout;
  FrameRef Frame;
  DenseMap<Function *, SmallVector<IndirectBrInst *, 4>> &ReturnDispatches;
  DenseMap<Value *, Value *> *LocalMap = nullptr;
  const DenseMap<BasicBlock *, BasicBlock *> *AddrHelpers = nullptr;
};

struct PushedContinuation {
  ContinuationRecord Record;
  FrameRef CallerFrame;
  FrameRef CalleeFrame;
  uint64_t ResumeState = 0;
  uint64_t CalleeEntryState = 0;
  BasicBlock *ResumeBlock = nullptr;
};

static bool lowerSelfTailEnter(CallInst *Call, IRBuilder<> &B,
                               ValueToValueMapTy &VMap,
                               DenseMap<Value *, Value *> &LocalMap,
                               const FunctionLayout &Layout,
                               LoweringContext &LCtx);

static void emitFrameOverflowGuard(IRBuilder<> &B, LoweringContext &LCtx) {
  MFLAArtifacts &A = LCtx.Artifacts;
  if (A.FrameBackend != FramePageBackendKind::Static)
    return;

  LLVMContext &Ctx = LCtx.Mega.getContext();
  Value *FreeHead = loadFrameFreeHead(B, A, "mfla.frame.guard.free");
  Value *HasFree = B.CreateICmpNE(FreeHead, constI64(Ctx, 0),
                                  "mfla.frame.guard.has.free");
  Value *Token = loadFrameTop(B, A, "mfla.frame.guard.top");
  Value *Capacity = constI64(Ctx, A.StaticFramePages * A.FramesPerPage);
  Value *AtCapacity =
      B.CreateICmpUGE(Token, Capacity, "mfla.frame.at.cap");
  Value *TooDeep = B.CreateAnd(B.CreateNot(HasFree), AtCapacity,
                               "mfla.frame.too.deep");
  emitTrapBlock(B, LCtx.Mega, LCtx.Exit, "mfla.frame.push.ok",
                "mfla.frame.overflow", TooDeep,
                [&](BasicBlock *Ok) { LCtx.StateAlloc.assign(Ok, LCtx.StateFor); });
}

static void releaseReturnedFrame(IRBuilder<> &B, MFLAArtifacts &A,
                                 Value *ReturnedToken, Value *CallerFrame) {
  Value *DidPushFrame =
      B.CreateICmpNE(ReturnedToken, CallerFrame, "mfla.frame.did.push");
  Value *OldFreeHead = loadFrameFreeHead(B, A, "mfla.frame.free.old");
  Value *FreeNext = B.CreateSelect(DidPushFrame, OldFreeHead,
                                   constI64(B.getContext(), 0),
                                   "mfla.frame.release.next");
  storeFrameFreeNext(B, A, ReturnedToken, FreeNext);
  storeFrameFreeHead(B, A,
                     B.CreateSelect(DidPushFrame, ReturnedToken, OldFreeHead,
                                    "mfla.frame.free.new"));
}

static PushedContinuation pushContinuation(
    IRBuilder<> &B, LoweringContext &LCtx, const FunctionLayout &CallerLayout,
    const FunctionLayout &CalleeLayout, Function *Callee, CallInst *Call,
    BasicBlock *CalleeEntry, uint64_t CalleeState, FrameRef CallerFrame,
    FrameRef CalleeFrame) {
  PushedContinuation Pushed;
  Pushed.CallerFrame = CallerFrame;
  Pushed.CalleeFrame = CalleeFrame;
  Pushed.ResumeBlock = BasicBlock::Create(
      LCtx.Mega.getContext(), "mfla.call.cont", &LCtx.Mega, LCtx.Exit);
  Pushed.ResumeState = LCtx.StateAlloc.assign(Pushed.ResumeBlock, LCtx.StateFor);
  // Return continuations and address-taken helpers are inserted after the
  // initial target ordering.  Keep them self-anchored for now; they cannot
  // participate in the fixed-size target groups without post-layout grouping.
  Pushed.CalleeEntryState = CalleeState;
  LCtx.ContinuationsByCallee[Callee].push_back(Pushed.ResumeBlock);
  Pushed.Record = continuationRecord(
      CalleeLayout, Pushed.ResumeBlock, CalleeFrame, CallerFrame,
      CallerLayout.CallResultOffsets.lookup(Call), !Call->getType()->isVoidTy());
  storeContinuation(
      B, LCtx.Artifacts, Pushed.Record, CalleeState, Pushed.ResumeState,
      loadEdgeConstant(B, LCtx.Artifacts, CalleeEntry, Pushed.Record.ResumeBlock,
                       Pushed.ResumeState));
  return Pushed;
}

static bool applyContinuation(IRBuilder<> &B, LoweringContext &LCtx,
                              FunctionTerminatorContext &FCtx,
                              uint64_t ReturnState,
                              uint64_t CalleeEntryState) {
  Function &Mega = LCtx.Mega;
  MFLAArtifacts &A = LCtx.Artifacts;
  FrameRef ReturnFrame = FCtx.Frame;
  ContinuationRecord RetCont = continuationRecord(FCtx.Layout, ReturnFrame);
  Value *ContXor = loadContinuationXor(B, A, RetCont, "mfla.ret.cont.xor");
  Value *HasContinuation = B.CreateICmpNE(ContXor, constI64(Mega.getContext(), 0));
  BasicBlock *Cur = B.GetInsertBlock();
  BasicBlock *ReturnBB = BasicBlock::Create(
      Mega.getContext(), Cur->getName() + ".ret.cont", &Mega, LCtx.Exit);
  B.CreateCondBr(HasContinuation, ReturnBB, LCtx.Exit);

  IRBuilder<> ReturnB(ReturnBB);
  Value *ContEdge = loadContinuationEdge(ReturnB, A, RetCont,
                                         "mfla.ret.cont.edge");
  Value *CallerFrame = loadContinuationCallerFrame(ReturnB, A, RetCont);
  Value *ReturnedToken =
      ReturnFrame.Token ? ReturnFrame.Token : constI64(Mega.getContext(), 0);
  storeReturnedFrameToken(ReturnB, A, ReturnedToken);
  releaseReturnedFrame(ReturnB, A, ReturnedToken, CallerFrame);
  clearContinuation(ReturnB, A, RetCont);
  restoreFrame(ReturnB, A, CallerFrame);
  Value *ReturnCurState = loadState(ReturnB, A, "mfla.ret.cur.state");
  Value *NextState = ReturnB.CreateXor(
      ReturnCurState, constI64(Mega.getContext(), CalleeEntryState ^ ReturnState));
  NextState = ReturnB.CreateXor(NextState, ContXor, "mfla.ret.next.state");
  storeState(ReturnB, A, NextState);
  BasicBlock *MappedEntry = FCtx.BBMap.lookup(&FCtx.OldF->getEntryBlock());
  if (!MappedEntry)
    return false;
  Value *Target = encodedTarget(ReturnB, Mega, MappedEntry, ContEdge,
                                keyForState(ReturnB, A, NextState));
  auto *End = IndirectBrInst::Create(Target, 0, ReturnBB);
  FCtx.ReturnDispatches[FCtx.Layout.Owner].push_back(End);
  return true;
}

static bool createThreadedTerminator(IRBuilder<> &B, Instruction *OldTerm,
                                     LoweringContext &LCtx,
                                     FunctionTerminatorContext &FCtx) {
  Function &Mega = LCtx.Mega;
  MFLAArtifacts &A = LCtx.Artifacts;
  const FunctionLayout &Layout = FCtx.Layout;
  ValueToValueMapTy &VMap = FCtx.VMap;
  DenseMap<BasicBlock *, BasicBlock *> &BBMap = FCtx.BBMap;
  Function *OldF = FCtx.OldF;
  const DenseMap<BasicBlock *, BasicBlock *> *AddrHelperForOldBB =
      FCtx.AddrHelpers;
  BasicBlock *Pred = OldTerm->getParent();

  if (auto *Ret = dyn_cast<ReturnInst>(OldTerm)) {
    if (Value *RV = Ret->getReturnValue()) {
      if (auto *Call = dyn_cast<CallInst>(RV)) {
        CallsitePlan Plan = {};
        auto It = LCtx.CallsitePlans.find(Call);
        if (It != LCtx.CallsitePlans.end())
          Plan = It->second;
        if (isInternalEnter(Plan) && Plan.TailReuse)
          return lowerSelfTailEnter(Call, B, VMap, *FCtx.LocalMap, Layout, LCtx);
      }
      Value *Mapped = mapValueForUse(RV, B, VMap, nullptr, Layout, A, OldF,
                                     &BBMap, AddrHelperForOldBB, &FCtx.Frame);
      if (!Mapped)
        return false;
      storeFrameSlot(B, Mapped, A, FCtx.Frame, Layout.RetOffset);
    }
    BasicBlock *Cur = B.GetInsertBlock();
    uint64_t ReturnState = stateFor(Cur, LCtx.StateFor);
    BasicBlock *MappedEntry = BBMap.lookup(&OldF->getEntryBlock());
    if (!MappedEntry)
      return false;
    uint64_t CalleeEntryState = stateFor(MappedEntry, LCtx.StateFor);
    return applyContinuation(B, LCtx, FCtx, ReturnState, CalleeEntryState);
  }

  auto *Br = dyn_cast<BranchInst>(OldTerm);
  if (!Br) {
    if (auto *IB = dyn_cast<IndirectBrInst>(OldTerm)) {
      Value *RawTarget = mapValueForUse(IB->getAddress(), B, VMap, nullptr,
                                        Layout, A, OldF, &BBMap,
                                        AddrHelperForOldBB);
      if (!RawTarget || !AddrHelperForOldBB)
        return false;

      Type *I64 = Type::getInt64Ty(Mega.getContext());
      Value *RawI = B.CreatePtrToInt(RawTarget, I64, "mfla.ibr.target.i");
      const DataLayout &DL = Mega.getParent()->getDataLayout();
      SmallVector<BasicBlock *, 8> Helpers;
      for (unsigned I = 0, E = IB->getNumDestinations(); I != E; ++I) {
        BasicBlock *OldDest = IB->getDestination(I);
        auto HelperIt = AddrHelperForOldBB->find(OldDest);
        if (HelperIt == AddrHelperForOldBB->end())
          return false;
        BasicBlock *Helper = HelperIt->second;
        if (llvm::find(Helpers, Helper) == Helpers.end())
          Helpers.push_back(Helper);
        Value *HelperI = ConstantExpr::getPtrToInt(BlockAddress::get(&Mega, Helper), I64);
        Value *TakeDest = B.CreateICmpEQ(RawI, HelperI, "mfla.ibr.take");
        if (!writeIncomingPhis(B, DL, Pred, OldDest, TakeDest, VMap, Layout, A,
                               OldF, BBMap, AddrHelperForOldBB))
          return false;
      }

      auto *End = IndirectBrInst::Create(RawTarget, Helpers.size(),
                                         B.GetInsertBlock());
      for (BasicBlock *Helper : Helpers)
        End->addDestination(Helper);
      return true;
    }

    auto *Sw = dyn_cast<SwitchInst>(OldTerm);
    if (!Sw)
      return false;
    Value *Cond = mapValueForUse(Sw->getCondition(), B, VMap, nullptr, Layout,
                                 A, OldF, &BBMap, AddrHelperForOldBB);
    BasicBlock *OldDefault = Sw->getDefaultDest();
    BasicBlock *NewDefault = BBMap.lookup(OldDefault);
    if (!Cond || !NewDefault)
      return false;

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

    BasicBlock *EdgeAnchor = pickAnchorForTarget(Destinations.front(), LCtx.Anchors,
                                                LCtx.StateAlloc);
    uint64_t FromState = stateFor(B.GetInsertBlock(), LCtx.StateFor);
    uint64_t DefaultState = stateFor(NewDefault, LCtx.StateFor);
    Value *CurState = loadState(B, A, "mfla.switch.cur.state");
    Value *NextState = transitionState(B, CurState, FromState, DefaultState);
    Value *Encoded = loadEdgeConstant(B, A, EdgeAnchor, NewDefault, DefaultState);
    SmallVector<std::pair<BasicBlock *, Value *>, 8> SuccConds;
    SuccConds.push_back({OldDefault, ConstantInt::getFalse(Mega.getContext())});

    for (auto Case : Sw->cases()) {
      BasicBlock *OldSucc = Case.getCaseSuccessor();
      BasicBlock *NewSucc = BBMap.lookup(OldSucc);
      if (!NewSucc)
        return false;
      Value *TakeCase = B.CreateICmpEQ(Cond, Case.getCaseValue());
      uint64_t CaseState = stateFor(NewSucc, LCtx.StateFor);
      Value *CaseNext = transitionState(B, CurState, FromState, CaseState);
      Value *CaseEnc = loadEdgeConstant(B, A, EdgeAnchor, NewSucc, CaseState);
      NextState = muxInt(B, TakeCase, CaseNext, NextState);
      Encoded = muxInt(B, TakeCase, CaseEnc, Encoded);

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

    for (auto &Entry : SuccConds) {
      if (hasPhiNodes(Entry.first)) {
        if (!writeIncomingPhis(B, Mega.getParent()->getDataLayout(), Pred,
                               Entry.first, Entry.second, VMap, Layout, A, OldF,
                               BBMap, AddrHelperForOldBB))
          return false;
      }
    }

    storeState(B, A, NextState);
    Value *Target = encodedTarget(B, Mega, EdgeAnchor, Encoded,
                                  keyForState(B, A, NextState));
    auto *End = IndirectBrInst::Create(Target, Destinations.size(), B.GetInsertBlock());
    for (BasicBlock *Dest : Destinations)
      End->addDestination(Dest);
    return true;
  }

  if (Br->isUnconditional()) {
    BasicBlock *OldSucc = Br->getSuccessor(0);
    BasicBlock *NewSucc = BBMap.lookup(OldSucc);
    if (!NewSucc || !writeIncomingPhis(B, Mega.getParent()->getDataLayout(),
                                       Pred, OldSucc, nullptr, VMap, Layout, A,
                                       OldF, BBMap, AddrHelperForOldBB))
      return false;
    BasicBlock *EdgeAnchor = pickAnchorForTarget(NewSucc, LCtx.Anchors,
                                                 LCtx.StateAlloc);
    uint64_t FromState = stateFor(B.GetInsertBlock(), LCtx.StateFor);
    uint64_t ToState = stateFor(NewSucc, LCtx.StateFor);
    Value *NextState = transitionState(B, loadState(B, A, "mfla.cur.state"),
                                       FromState, ToState);
    storeState(B, A, NextState);
    Value *Target = encodedTarget(B, Mega, EdgeAnchor,
                                  loadEdgeConstant(B, A, EdgeAnchor, NewSucc, ToState),
                                  keyForState(B, A, NextState));
    auto *End = IndirectBrInst::Create(Target, 1, B.GetInsertBlock());
    End->addDestination(NewSucc);
    return true;
  }

  Value *Cond = mapValueForUse(Br->getCondition(), B, VMap, nullptr, Layout, A,
                               OldF, &BBMap, AddrHelperForOldBB);
  BasicBlock *OldTrue = Br->getSuccessor(0);
  BasicBlock *OldFalse = Br->getSuccessor(1);
  BasicBlock *TrueBB = BBMap.lookup(OldTrue);
  BasicBlock *FalseBB = BBMap.lookup(OldFalse);
  if (!Cond || !TrueBB || !FalseBB)
    return false;

  BasicBlock *EdgeAnchor = pickAnchorForTarget(FalseBB, LCtx.Anchors,
                                               LCtx.StateAlloc);
  uint64_t FromState = stateFor(B.GetInsertBlock(), LCtx.StateFor);
  uint64_t TrueState = stateFor(TrueBB, LCtx.StateFor);
  uint64_t FalseState = stateFor(FalseBB, LCtx.StateFor);
  Value *CurState = loadState(B, A, "mfla.cond.cur.state");
  Value *NextState = conditionalTransitionState(B, CurState, FromState,
                                                FalseState, TrueState, Cond);
  Value *TrueEnc = loadEdgeConstant(B, A, EdgeAnchor, TrueBB, TrueState);
  Value *FalseEnc = loadEdgeConstant(B, A, EdgeAnchor, FalseBB, FalseState);
  Value *Encoded = muxInt(B, Cond, TrueEnc, FalseEnc);
  Value *Target = encodedTarget(B, Mega, EdgeAnchor, Encoded,
                                keyForState(B, A, NextState));
  storeState(B, A, NextState);
  if (!writeIncomingPhis(B, Mega.getParent()->getDataLayout(), Pred, OldTrue,
                         Cond, VMap, Layout, A, OldF, BBMap,
                         AddrHelperForOldBB))
    return false;
  Value *NotCond = B.CreateNot(Cond);
  if (!writeIncomingPhis(B, Mega.getParent()->getDataLayout(), Pred, OldFalse,
                         NotCond, VMap, Layout, A, OldF, BBMap,
                         AddrHelperForOldBB))
    return false;
  auto *End = IndirectBrInst::Create(Target, 2, B.GetInsertBlock());
  End->addDestination(TrueBB);
  End->addDestination(FalseBB);
  return true;
}

static bool lowerSelfTailEnter(CallInst *Call, IRBuilder<> &B,
                               ValueToValueMapTy &VMap,
                               DenseMap<Value *, Value *> &LocalMap,
                               const FunctionLayout &Layout,
                               LoweringContext &LCtx) {
  Function *Callee = directCalledFunction(Call);
  if (!Callee || Callee != Layout.Owner)
    return false;

  FrameRef CurFrame = currentFrame(B, LCtx.Artifacts, Layout);
  SmallVector<Value *, 8> Args;
  for (Value *Arg : Call->args()) {
    Value *MappedArg = mapValueForUse(Arg, B, VMap, &LocalMap, Layout,
                                      LCtx.Artifacts, nullptr, nullptr,
                                      nullptr, &CurFrame);
    if (!MappedArg)
      return false;
    Args.push_back(MappedArg);
  }
  if (Args.size() != Layout.ArgOffsets.size())
    return false;
  for (unsigned I = 0, E = Args.size(); I != E; ++I)
    storeFrameSlot(B, Args[I], LCtx.Artifacts, CurFrame, Layout.ArgOffsets[I]);

  BasicBlock *CalleeEntry = LCtx.EntryBlockFor[Callee];
  if (!CalleeEntry)
    return false;
  uint64_t FromState = stateFor(B.GetInsertBlock(), LCtx.StateFor);
  uint64_t CalleeState = stateFor(CalleeEntry, LCtx.StateFor);
  Value *CurState = loadState(B, LCtx.Artifacts, "mfla.tail.cur.state");
  Value *NextState = transitionState(B, CurState, FromState, CalleeState);
  storeState(B, LCtx.Artifacts, NextState);
  Value *Target = encodedTarget(
      B, LCtx.Mega, B.GetInsertBlock(),
      loadEdgeConstant(B, LCtx.Artifacts, B.GetInsertBlock(), CalleeEntry,
                       CalleeState),
      keyForState(B, LCtx.Artifacts, NextState));
  auto *Jump = IndirectBrInst::Create(Target, 1, B.GetInsertBlock());
  Jump->addDestination(CalleeEntry);
  return true;
}

static bool lowerInternalEnter(CallInst *Call, IRBuilder<> &B,
                               ValueToValueMapTy &VMap,
                               DenseMap<Value *, Value *> &LocalMap,
                               const FunctionLayout &CallerLayout,
                               DenseMap<BasicBlock *, BasicBlock *> &TerminatorBlockFor,
                               BasicBlock &OldBB, LoweringContext &LCtx,
                               FrameRef &CurrentFrame) {
  Function *Callee = directCalledFunction(Call);
  if (!Callee || !LCtx.CandidateIDs.count(Callee))
    return false;

  auto PlanIt = LCtx.CallsitePlans.find(Call);
  if (PlanIt == LCtx.CallsitePlans.end())
    return false;
  const CallsitePlan &Plan = PlanIt->second;
  const FunctionLayout &CalleeLayout = LCtx.Layouts[Callee];
  BasicBlock *CallOriginBlock = B.GetInsertBlock();
  uint64_t FromState = stateFor(CallOriginBlock, LCtx.StateFor);
  FrameRef CallerFrame = currentFrame(B, LCtx.Artifacts, CallerLayout);
  if (usesDynamicFrame(Plan))
    emitFrameOverflowGuard(B, LCtx);
  FrameRef CalleeFrame = usesDynamicFrame(Plan)
                             ? pushFrame(B, LCtx.Artifacts, CalleeLayout)
                             : frameWithToken(CalleeLayout, CallerFrame.Token);
  unsigned ArgNo = 0;
  for (Value *Arg : Call->args()) {
    Value *MappedArg =
        mapValueForUse(Arg, B, VMap, &LocalMap, CallerLayout, LCtx.Artifacts,
                       nullptr, nullptr, nullptr, &CallerFrame);
    if (!MappedArg || ArgNo >= CalleeLayout.ArgOffsets.size())
      return false;
    storeFrameSlot(B, MappedArg, LCtx.Artifacts, CalleeFrame,
                   CalleeLayout.ArgOffsets[ArgNo++]);
  }

  BasicBlock *CalleeEntry = LCtx.EntryBlockFor[Callee];
  uint64_t CalleeState = stateFor(CalleeEntry, LCtx.StateFor);
  Value *CurState = loadState(B, LCtx.Artifacts, "mfla.call.cur.state");
  Value *CallNextState = transitionState(B, CurState, FromState, CalleeState);
  PushedContinuation Pushed = pushContinuation(
      B, LCtx, CallerLayout, CalleeLayout, Callee, Call, CalleeEntry,
      CalleeState, CallerFrame, CalleeFrame);
  storeState(B, LCtx.Artifacts, CallNextState);
  Value *CallTarget = encodedTarget(
      B, LCtx.Mega, CallOriginBlock,
      loadEdgeConstant(B, LCtx.Artifacts, CallOriginBlock, CalleeEntry,
                       CalleeState),
      keyForState(B, LCtx.Artifacts, CallNextState));
  auto *Jump = IndirectBrInst::Create(CallTarget, 1, B.GetInsertBlock());
  Jump->addDestination(CalleeEntry);

  B.SetInsertPoint(Pushed.ResumeBlock);
  TerminatorBlockFor[&OldBB] = Pushed.ResumeBlock;
  LocalMap.clear();
  CurrentFrame = currentFrame(B, LCtx.Artifacts, CallerLayout);
  CalleeFrame = frameWithToken(CalleeLayout,
                               loadReturnedFrameToken(B, LCtx.Artifacts));
  if (Pushed.Record.HasResultSlot) {
    Value *RetVal = loadFrameSlot(B, Call->getType(), LCtx.Artifacts,
                                  CalleeFrame, CalleeLayout.RetOffset,
                                  "mfla.call.ret");
    LocalMap[Call] = RetVal;
    storeFrameSlot(B, RetVal, LCtx.Artifacts,
                   currentFrame(B, LCtx.Artifacts, CallerLayout),
                   Pushed.Record.ResultSlot);
  }
  return true;
}

static bool
buildStructuralMega(MFLAArtifacts &A, ArrayRef<Function *> Candidates,
                    DenseMap<Function *, FunctionLayout> &Layouts,
                    const DenseMap<Function *, unsigned> &CandidateIDs,
                    const DenseMap<CallInst *, CallsitePlan> &CallsitePlans) {
  Function &Mega = *A.Mega;
  Module &M = *Mega.getParent();
  LLVMContext &Ctx = Mega.getContext();
  Argument *EntryState = Mega.getArg(1);
  Argument *EntryEdge = Mega.getArg(2);
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", &Mega);
  BasicBlock *Exit = BasicBlock::Create(Ctx, "exit", &Mega);
  DenseMap<Function *, SmallVector<BasicBlock *, 4>> ContinuationsByCallee;
  DenseMap<Function *, SmallVector<IndirectBrInst *, 4>> ReturnDispatches;

  IRBuilder<> ExitB(Exit);
  ExitB.CreateRetVoid();

  SmallVector<BasicBlock *, 16> EntryRegions;
  DenseMap<Function *, BasicBlock *> EntryBlockFor;
  DenseMap<Function *, DenseMap<BasicBlock *, BasicBlock *>> FunctionBBMaps;
  DenseMap<Function *, DenseMap<BasicBlock *, BasicBlock *>> AddrHelperForFunction;

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

  DenseMap<BasicBlock *, uint64_t> StateFor;
  SmallVector<BasicBlock *, 32> AnchorCandidates;
  MFLAStateAllocator StateAlloc(M);
  for (Function *F : Candidates) {
    DenseMap<BasicBlock *, BasicBlock *> &BBMap = FunctionBBMaps[F];
    for (BasicBlock &BB : *F) {
      BasicBlock *NewBB = BBMap.lookup(&BB);
      if (!NewBB)
        return false;
      StateAlloc.assign(NewBB, StateFor);
      AnchorCandidates.push_back(NewBB);
    }
  }

  for (Function *F : Candidates) {
    DenseMap<BasicBlock *, BasicBlock *> &Helpers = AddrHelperForFunction[F];
    for (BasicBlock *OldBB : collectAddressTakenBlocks(*F)) {
      BasicBlock *RealTarget = FunctionBBMaps[F].lookup(OldBB);
      if (!RealTarget)
        return false;
      std::string Name = (RealTarget->getName() + ".addr.helper").str();
      BasicBlock *Helper = BasicBlock::Create(Ctx, Name, &Mega, Exit);
      Helpers[OldBB] = Helper;
      StateAlloc.assign(Helper, StateFor);
      AnchorCandidates.push_back(Helper);
    }
  }

  AnchorPool AnchorPool;
  SmallVector<SmallVector<BasicBlock *, MFLAAnchorGroupSize>, 32>
      AnchorConstraints;

  for (Function *F : Candidates) {
    DenseMap<BasicBlock *, BasicBlock *> &BBMap = FunctionBBMaps[F];
    DenseMap<BasicBlock *, BasicBlock *> &Helpers = AddrHelperForFunction[F];
    for (BasicBlock &BB : *F) {
      BasicBlock *Pred = BBMap.lookup(&BB);
      if (!Pred)
        return false;
      Instruction *Term = BB.getTerminator();
      if (auto *Br = dyn_cast<BranchInst>(Term)) {
        if (Br->isConditional())
          addAnchorConstraint(
              AnchorConstraints,
              {BBMap.lookup(Br->getSuccessor(0)), BBMap.lookup(Br->getSuccessor(1))});
      } else if (auto *Sw = dyn_cast<SwitchInst>(Term)) {
        SmallVector<BasicBlock *, 8> Targets;
        Targets.push_back(BBMap.lookup(Sw->getDefaultDest()));
        for (auto Case : Sw->cases())
          Targets.push_back(BBMap.lookup(Case.getCaseSuccessor()));
        addAnchorConstraint(AnchorConstraints, Targets);
      }

      for (Instruction &I : BB) {
        auto *Call = dyn_cast<CallInst>(&I);
        if (!Call)
          continue;
        auto PlanIt = CallsitePlans.find(Call);
        if (PlanIt == CallsitePlans.end() || !isInternalEnter(PlanIt->second))
          continue;
        Function *Callee = directCalledFunction(Call);
        if (BasicBlock *CalleeEntry = EntryBlockFor.lookup(Callee))
          addAnchorConstraint(AnchorConstraints, {Pred, CalleeEntry});
      }
    }

    SmallVector<BasicBlock *, 8> AddressTaken = collectAddressTakenBlocks(*F);
    for (BasicBlock *OldBB : AddressTaken) {
      auto HelperIt = Helpers.find(OldBB);
      if (HelperIt == Helpers.end())
        continue;
      addAnchorConstraint(AnchorConstraints,
                          {HelperIt->second, BBMap.lookup(OldBB)});
    }
  }

  if (!EntryRegions.empty())
    addAnchorConstraint(AnchorConstraints, EntryRegions);
  buildAnchorPool(AnchorCandidates, AnchorConstraints, StateAlloc, AnchorPool);

  if (!prepareModuleBlockAddressGlobals(M, Mega, Candidates, FunctionBBMaps,
                                        AddrHelperForFunction, A))
    return false;

  for (Function *F : Candidates) {
    DenseMap<BasicBlock *, BasicBlock *> &Helpers = AddrHelperForFunction[F];
    DenseMap<BasicBlock *, BasicBlock *> &BBMap = FunctionBBMaps[F];
    SmallVector<BasicBlock *, 8> AddressTaken = collectAddressTakenBlocks(*F);
    for (BasicBlock *OldBB : AddressTaken) {
      auto HelperIt = Helpers.find(OldBB);
      if (HelperIt == Helpers.end())
        continue;
      createAddressTakenHelper(Mega, A, HelperIt->second, BBMap[OldBB], StateFor,
                               StateAlloc, AnchorPool);
    }
  }

  BasicBlock *Anchor = pickAnchorForTarget(EntryRegions.front(), AnchorPool,
                                           StateAlloc);
  IRBuilder<> EntryB(Entry);
  storeState(EntryB, A, EntryState);
  Value *Target = encodedTarget(EntryB, Mega, Anchor, EntryEdge,
                                keyForState(EntryB, A, EntryState));
  auto *IB = IndirectBrInst::Create(Target, EntryRegions.size(), Entry);
  for (BasicBlock *R : EntryRegions)
    IB->addDestination(R);

  for (Function *F : Candidates) {
    BasicBlock *EntryBlock = EntryBlockFor[F];
    uint64_t EntryStateValue = stateFor(EntryBlock, StateFor);
    A.EntryStateForFunction[F] = EntryStateValue;
    A.EntryEdgeForFunction[F] =
        createEdgeConstant(A, Anchor, EntryBlock, EntryStateValue);
  }

  LoweringContext LCtx{Mega, A, Layouts, CandidateIDs, CallsitePlans,
                       EntryBlockFor, AddrHelperForFunction, StateFor,
                       StateAlloc, Anchor, Exit, ContinuationsByCallee,
                       AnchorPool};

  for (Function *F : Candidates) {
    ValueToValueMapTy VMap;
    DenseMap<BasicBlock *, BasicBlock *> &BBMap = FunctionBBMaps[F];
    DenseMap<BasicBlock *, BasicBlock *> &AddrHelpers = AddrHelperForFunction[F];
    DenseMap<BasicBlock *, BasicBlock *> TerminatorBlockFor;
    const FunctionLayout &L = Layouts[F];
    FrameRef CurFrame;

    for (BasicBlock &BB : *F) {
      DenseMap<Value *, Value *> LocalMap;
      BasicBlock *NewBB = BBMap[&BB];
      TerminatorBlockFor[&BB] = NewBB;
      IRBuilder<> B(NewBB);
      if (&BB != &F->getEntryBlock())
        B.SetInsertPoint(NewBB);
      CurFrame = currentFrame(B, A, L);
      for (Instruction &I : BB) {
        if (I.isTerminator())
          break;
        if (auto *Phi = dyn_cast<PHINode>(&I)) {
          auto It = L.PhiOffsets.find(Phi);
          if (It == L.PhiOffsets.end())
            return false;
          Value *PhiV = loadFrameSlot(B, Phi->getType(), A, CurFrame,
                                      It->second, Phi->getName());
          LocalMap[Phi] = PhiV;
          continue;
        }
        if (auto *AI = dyn_cast<AllocaInst>(&I)) {
          auto It = L.SpilledValueOffsets.find(AI);
          if (It == L.SpilledValueOffsets.end())
            return false;
          LocalMap[AI] = frameSlotPtr(B, A, CurFrame, It->second);
          continue;
        }
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          auto PlanIt = LCtx.CallsitePlans.find(Call);
          if (PlanIt != LCtx.CallsitePlans.end() &&
              isInternalEnter(PlanIt->second)) {
            if (PlanIt->second.TailReuse)
              continue;
            if (!lowerInternalEnter(Call, B, VMap, LocalMap, L,
                                    TerminatorBlockFor, BB, LCtx, CurFrame))
              return false;
            continue;
          }
        }
        Instruction *Clone = cloneMapped(I, B, VMap, LocalMap, L, A, F, BBMap,
                                         &AddrHelpers, &CurFrame);
        if (!Clone)
          return false;
        B.Insert(Clone);
        if (!I.getType()->isVoidTy()) {
          if (!isFrameScalar(I.getType()))
            continue;
          StorageRef ResultRef;
          if (auto *Call = dyn_cast<CallInst>(&I)) {
            auto CallIt = L.CallResultOffsets.find(Call);
            if (CallIt == L.CallResultOffsets.end())
              return false;
            ResultRef = CallIt->second;
          } else {
            auto It = L.SpilledValueOffsets.find(&I);
            if (It == L.SpilledValueOffsets.end())
              return false;
            ResultRef = It->second;
          }
          storeFrameSlot(B, Clone, A, CurFrame, ResultRef);
          LocalMap[&I] = Clone;
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
      FrameRef TermFrame = currentFrame(B, A, L);
      FunctionTerminatorContext TermCtx{
          F, VMap, BBMap, L, TermFrame, ReturnDispatches, nullptr,
          &AddrHelpers};
      if (!createThreadedTerminator(B, BB.getTerminator(), LCtx, TermCtx))
        return false;
    }
  }

  for (Function *F : Candidates)
    for (IndirectBrInst *IB : ReturnDispatches[F])
      for (BasicBlock *Cont : ContinuationsByCallee[F])
        IB->addDestination(Cont);

  // A candidate that is never called by another candidate has no continuation
  // resume points, leaving its return indirectbr with zero destinations. That
  // path is dynamically dead (its continuation is never set), so replace the
  // empty indirectbr with a direct branch to Exit rather than emit a degenerate
  // terminator.
  for (Function *F : Candidates)
    for (IndirectBrInst *IB : ReturnDispatches[F])
      if (IB->getNumDestinations() == 0) {
        BasicBlock *BB = IB->getParent();
        IB->eraseFromParent();
        BranchInst::Create(Exit, BB);
      }

  return true;
}

static void markNoUnwindIfNoThrowingCalls(Function &F) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (!isEffectivelyNoUnwind(*CB))
          return;
  F.addFnAttr(Attribute::NoUnwind);
}

static void sanitizeWrapperAttributes(Function &F) {
  // The rebuilt wrapper reads/writes a ctx and the cleanup path calls free, so
  // the old body's memory-effect attrs are now lies; the rest stay valid.
  F.removeFnAttr(Attribute::NoFree);
  F.removeFnAttr(Attribute::ReadNone);
  F.removeFnAttr(Attribute::ReadOnly);
  F.removeFnAttr(Attribute::Memory);
}

static void rewriteAsWrapper(Function &F, MFLAArtifacts &A,
                             const FunctionLayout &L, uint64_t EntryState,
                             GlobalVariable *EntryEdge) {
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
  uint64_t CtxSize = std::max<uint64_t>(1, A.CtxSize);
  auto *CtxTy = ArrayType::get(Type::getInt8Ty(Ctx), CtxSize);
  AllocaInst *CtxStorage = B.CreateAlloca(CtxTy, nullptr, "mfla.ctx");
  CtxStorage->setAlignment(Align(16));
  B.CreateMemSet(CtxStorage, ConstantInt::get(Type::getInt8Ty(Ctx), 0),
                 CtxSize, Align(16));
  Value *CtxPtr = B.CreateInBoundsGEP(
      CtxTy, CtxStorage, {constI64(Ctx, 0), constI64(Ctx, 0)}, "mfla.ctx.ptr");

  B.CreateStore(constI64(Ctx, 0), ctxBytePtr(B, CtxPtr, A.CurrentFrameOffset));
  B.CreateStore(constI64(Ctx, 1), ctxBytePtr(B, CtxPtr, A.FrameTopOffset));
  B.CreateStore(constI64(Ctx, 0), ctxBytePtr(B, CtxPtr, A.FrameFreeHeadOffset));
  B.CreateStore(ConstantPointerNull::get(PointerType::get(Ctx, 0)),
                ctxBytePtr(B, CtxPtr, A.FramePageTableHeadOffset));
  B.CreateStore(constI64(Ctx, 0), ctxBytePtr(B, CtxPtr, A.ReturnedFrameOffset));

  FrameRef CurFrame = rootFrame(L, Ctx);
  for (unsigned I = 0, E = Args.size(); I != E; ++I)
    storeFrameSlot(B, Args[I], A, CtxPtr, CurFrame, L.ArgOffsets[I]);
  initializeContinuation(B, A, CtxPtr, continuationRecord(L, CurFrame));
  Value *EntryEdgeValue =
      B.CreateLoad(Type::getInt64Ty(Ctx), EntryEdge, "mfla.entry.edge");
  B.CreateCall(A.Mega, {CtxPtr, constI64(Ctx, EntryState), EntryEdgeValue});
  if (F.getReturnType()->isVoidTy()) {
    cleanupFrameStorage(B, A, CtxPtr);
    B.CreateRetVoid();
  } else {
    Value *RetVal =
        loadFrameSlot(B, F.getReturnType(), A, CtxPtr, CurFrame, L.RetOffset, "");
    cleanupFrameStorage(B, A, CtxPtr);
    B.CreateRet(RetVal);
  }
}
} // namespace

PreservedAnalyses MFLAPass::run(Module &M, ModuleAnalysisManager &) {
  std::vector<Function *> InitialCandidates;
  for (Function &F : M) {
    if (!isCandidate(F))
      continue;
    InitialCandidates.push_back(&F);
  }

  DenseSet<Function *> CandidateSet;
  for (Function *F : InitialCandidates)
    CandidateSet.insert(F);

  std::vector<Function *> Candidates;
  DenseMap<Function *, unsigned> CandidateIDs;
  CallGraphInfo InitialGraph;
  CallGraphInfo CandidateGraph;
  DenseSet<Function *> RecursiveMembers;
  DenseMap<CallInst *, CallsitePlan> CallsitePlans;

  while (true) {
    std::vector<Function *> CandidateList;
    for (Function *F : InitialCandidates)
      if (CandidateSet.count(F))
        CandidateList.push_back(F);

    InitialGraph = buildCandidateCallGraph(CandidateList);
    SmallVector<SCCInfo, 8> InitialSCCs =
        findCandidateSCCs(CandidateList, InitialGraph);
    DenseSet<Function *> Recursive = recursiveMembers(InitialSCCs);
    RecursiveMembers = Recursive;
    std::vector<Function *> NonRecursiveCandidates;
    DenseMap<Function *, unsigned> NonRecursiveIDs;
    for (Function *F : CandidateList) {
      NonRecursiveIDs[F] = NonRecursiveCandidates.size();
      NonRecursiveCandidates.push_back(F);
    }

    bool Changed = false;
    for (Function *F : NonRecursiveCandidates) {
      if (supportsCurrentLowering(*F, NonRecursiveIDs))
        continue;
      CandidateSet.erase(F);
      Changed = true;
    }
    if (Changed)
      continue;

    if (pruneCandidatesForBlockAddressGlobals(M, CandidateSet))
      continue;

    Candidates.clear();
    CandidateIDs.clear();
    for (Function *F : InitialCandidates) {
      if (!CandidateSet.count(F))
        continue;
      CandidateIDs[F] = Candidates.size();
      Candidates.push_back(F);
    }

    if (Candidates.size() < 2) {
      YANSO_WARN_MODULE(PassName, M, "fewer than two eligible functions");
      return PreservedAnalyses::all();
    }

    CandidateGraph = buildCandidateCallGraph(Candidates);
    CallsitePlans = makeCallsitePlans(Candidates, CandidateIDs, RecursiveMembers);
    break;
  }

  FramePlan Plan = makeFramePlan(Candidates, CandidateIDs, CallsitePlans,
                                 CandidateGraph, RecursiveMembers,
                                 M.getDataLayout());

  MFLAArtifacts Artifacts = createArtifacts(M, Plan.FrameSize, Candidates.size());
  if (!buildStructuralMega(Artifacts, Candidates, Plan.Layouts, CandidateIDs,
                           CallsitePlans)) {
    Artifacts.eraseFromParent();
    return PreservedAnalyses::none();
  }
  markNoUnwindIfNoThrowingCalls(*Artifacts.FramePageResolver);
  markNoUnwindIfNoThrowingCalls(*Artifacts.Mega);

  commitModuleBlockAddressGlobals(Artifacts);

  for (Function *F : Candidates)
    rewriteAsWrapper(*F, Artifacts, Plan.Layouts[F],
                     Artifacts.EntryStateForFunction[F],
                     Artifacts.EntryEdgeForFunction.lookup(F));

  for (Function *F : Candidates)
    if (F->isDefTriviallyDead())
      F->eraseFromParent();

  return PreservedAnalyses::none();
}
