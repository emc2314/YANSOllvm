#include "VMPass.h"

#include "CryptoUtils.h"
#include "VMVariant.h"
#include "YANSOllvmSeed.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <string>

using namespace llvm;

namespace {
static cl::opt<unsigned> VMMutationVariantPermille(
    "vm-mutation-variant-permille", cl::init(500), cl::Hidden,
    cl::desc("Permille probability for VM binary-op mutation wrappers"));
static cl::opt<unsigned> VMRelationAppVariantPermille(
    "vm-relation-app-variant-permille", cl::init(500), cl::Hidden,
    cl::desc(
        "Permille probability for relation-applied VM binary-op variants"));
static cl::opt<unsigned> VMMaxVariantsPerSuperOp(
    "vm-max-variants-per-superop", cl::init(8), cl::Hidden,
    cl::desc("Maximum body variants per super-op shape; 0 means unlimited"));
static cl::opt<unsigned>
    VMSuperOpMaxLen("vm-superop-max-len", cl::init(4), cl::Hidden,
                    cl::desc("Maximum IR instruction count per super-op DAG"));

class VirtualizeImpl {
  enum class HandlerKind {
    Binary,
    ICmp,
    Intrinsic,
    Cast,
    Select,
    GEP,
    Load,
    Store
  };

  struct VMOpNode {
    unsigned Opcode = 0;
    CmpInst::Predicate Predicate = CmpInst::ICMP_EQ;
    Intrinsic::ID IntrinsicID = Intrinsic::not_intrinsic;
    Type *Ty = nullptr;
    Type *SrcTy = nullptr;
    Type *DstTy = nullptr;
    Type *SourceElementTy = nullptr;
    Align Alignment = Align(1);
    bool InBounds = false;
    SmallVector<int, 3> Inputs;
    SmallVector<Value *, 3> Constants;
  };

  StringMap<Function *> Cache;
  StringMap<SmallVector<uint64_t, 8>> SuperOpVariantBuckets;
  uint64_t ModuleSeed = 0;

  static constexpr StringRef Prefix = "__yansollvm_vm_";

  static void attrs(Function *F) {
    F->addFnAttr(Attribute::NoInline);
    F->addFnAttr(Attribute::NoUnwind);
    F->addFnAttr(Attribute::OptimizeNone);
  }

  // Windows funclet EH: calls need a "funclet" bundle.
  static bool usesFuncletEH(Function &F) {
    for (BasicBlock &BB : F)
      if (BB.isEHPad() && !BB.isLandingPad())
        return true;
    return false;
  }

  static bool isSupportedInt(Type *Ty) { return isa<IntegerType>(Ty); }

  static bool isSupportedPointer(Type *Ty) { return isa<PointerType>(Ty); }

  static bool isSupportedScalar(Type *Ty) {
    return isSupportedInt(Ty) || isSupportedPointer(Ty);
  }

  static std::string sanitizeName(StringRef Name) {
    std::string Sanitized;
    Sanitized.reserve(Name.size());
    bool LastWasUnderscore = false;

    for (char C : Name) {
      if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
          (C >= '0' && C <= '9')) {
        Sanitized.push_back(C);
        LastWasUnderscore = false;
      } else if (!LastWasUnderscore) {
        Sanitized.push_back('_');
        LastWasUnderscore = true;
      }
    }

    while (!Sanitized.empty() && Sanitized.front() == '_')
      Sanitized.erase(Sanitized.begin());
    while (!Sanitized.empty() && Sanitized.back() == '_')
      Sanitized.pop_back();
    return Sanitized;
  }

  static std::string sanitizedTypeName(Type *Ty) {
    std::string Name;
    raw_string_ostream OS(Name);
    Ty->print(OS);
    OS.flush();
    return sanitizeName(Name);
  }

  static std::string instructionName(unsigned Opcode) {
    return sanitizeName(Instruction::getOpcodeName(Opcode));
  }

  static std::string predicateName(CmpInst::Predicate Pred) {
    return sanitizeName(CmpInst::getPredicateName(Pred));
  }

  static std::string intrinsicName(Intrinsic::ID ID) {
    StringRef Name = Intrinsic::getBaseName(ID);
    if (Name.consume_front("llvm."))
      return sanitizeName(Name);
    return sanitizeName(Name);
  }

  static uint64_t instructionSeed(Instruction *I, uint64_t Seed) {
    if (!I)
      return Seed;
    Function *F = I->getFunction();
    if (F)
      Seed = yanso_hash_string(F->getName(), Seed);
    unsigned BlockIndex = 0;
    if (F) {
      for (BasicBlock &BB : *F) {
        if (&BB == I->getParent())
          break;
        ++BlockIndex;
      }
    }
    unsigned InstIndex = 0;
    if (BasicBlock *BB = I->getParent()) {
      for (Instruction &Cur : *BB) {
        if (&Cur == I)
          break;
        ++InstIndex;
      }
    }
    Seed = yanso_mix64(BlockIndex + 1, Seed);
    Seed = yanso_mix64(InstIndex + 1, Seed);
    Seed = yanso_mix64(I->getOpcode() + 1, Seed);
    return Seed;
  }

  Function *createStoreHandler(Module &M, Type *StoredTy, Type *PtrTy,
                               Align Alignment) {
    std::string FullName =
        (Twine(Prefix) + "store_" + sanitizedTypeName(StoredTy) + "_" +
         sanitizedTypeName(PtrTy) + "_a" + Twine(Alignment.value()))
            .str();
    Function *&F = Cache[FullName];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(Type::getVoidTy(M.getContext()),
                                             {StoredTy, PtrTy}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    auto It = F->arg_begin();
    Value *Val = &*It++;
    Value *Ptr = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    StoreInst *S = B.CreateStore(Val, Ptr);
    S->setAlignment(Alignment);
    B.CreateRetVoid();

    // Single-op store handler; no memory attrs (conservative side effects).
    attrs(F);
    return F;
  }

  static std::string opNodeName(const VMOpNode &N) {
    if (N.IntrinsicID != Intrinsic::not_intrinsic)
      return intrinsicName(N.IntrinsicID);
    if (N.Opcode == Instruction::ICmp)
      return (Twine("icmp_") + predicateName(N.Predicate)).str();
    return instructionName(N.Opcode);
  }

  static std::string superOpPatternName(ArrayRef<VMOpNode> Nodes) {
    std::string Name;
    raw_string_ostream OS(Name);
    bool First = true;
    for (const VMOpNode &N : Nodes) {
      if (!First)
        OS << "_";
      First = false;
      OS << opNodeName(N);
    }
    OS.flush();
    return sanitizeName(Name);
  }

  std::string superOpName(Type *RetTy, ArrayRef<VMOpNode> Nodes) {
    return (Twine(Prefix) + superOpPatternName(Nodes) + "_" +
            sanitizedTypeName(RetTy))
        .str();
  }

  static constexpr int ConstantRefBase = -1000000;

  Function *createSuperOpHandler(Module &M, ArrayRef<VMOpNode> Nodes,
                                 ArrayRef<Type *> ParamTys, Type *RetTy,
                                 StringRef PatternKey, uint64_t VariantSeed) {
    std::string CacheKey = (Twine(PatternKey.size()) + ":" + PatternKey + "#" +
                            Twine::utohexstr(VariantSeed))
                               .str();
    std::string FullName = superOpName(RetTy, Nodes);
    Function *&F = Cache[CacheKey];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(RetTy, ParamTys, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);

    SmallVector<Value *, 8> Params;
    for (Argument &Arg : F->args())
      Params.push_back(&Arg);

    SmallVector<Value *, 8> Results;
    for (const VMOpNode &N : Nodes) {
      SmallVector<Value *, 4> Ops;
      for (int Ref : N.Inputs) {
        if (Ref >= 0)
          Ops.push_back(Params[Ref]);
        else if (Ref <= ConstantRefBase)
          Ops.push_back(N.Constants[ConstantRefBase - Ref]);
        else
          Ops.push_back(Results[-Ref - 1]);
      }

      Value *R = nullptr;
      uint64_t NodeSeed = yanso_mix64(
          N.Opcode + 1, yanso_mix64(Results.size() + 1, VariantSeed));
      switch (N.Opcode) {
      case Instruction::Trunc:
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::PtrToInt:
      case Instruction::IntToPtr: {
        VMVariantEmitter::ScalarVariant V = VMVariantEmitter::selectCastVariant(
            N.Opcode, N.SrcTy, N.DstTy, NodeSeed);
        R = VMVariantEmitter::emitCastValue(B, N.Opcode, N.SrcTy, N.DstTy,
                                            Ops[0], V, NodeSeed);
        break;
      }
      case Instruction::ICmp:
        R = VMVariantEmitter::emitICmpValue(
            B, N.Predicate, Ops[0]->getType(), Ops[0], Ops[1],
            VMVariantEmitter::selectPredicateVariant(
                Ops[0]->getType(), NodeSeed, VMMutationVariantPermille),
            NodeSeed);
        break;
      case Instruction::Select:
        R = VMVariantEmitter::emitSelectValue(
            B, N.Ty, Ops[0], Ops[1], Ops[2],
            VMVariantEmitter::selectSelectVariant(N.Ty, NodeSeed,
                                                  VMMutationVariantPermille),
            NodeSeed);
        break;
      case Instruction::GetElementPtr: {
        SmallVector<Value *, 4> Indices(Ops.begin() + 1, Ops.end());
        R = N.InBounds ? B.CreateInBoundsGEP(N.SourceElementTy, Ops[0], Indices)
                       : B.CreateGEP(N.SourceElementTy, Ops[0], Indices);
        break;
      }
      case Instruction::Load: {
        auto *L = B.CreateLoad(N.Ty, Ops[0]);
        L->setAlignment(N.Alignment);
        R = L;
        break;
      }
      case Instruction::Call: {
        auto *ITy = cast<IntegerType>(N.Ty);
        R = VMVariantEmitter::emitIntrinsicValue(
            B, N.IntrinsicID, ITy, Ops,
            VMVariantEmitter::selectIntrinsicVariant(N.IntrinsicID, ITy,
                                                     NodeSeed),
            NodeSeed);
        break;
      }
      default: {
        auto *ITy = cast<IntegerType>(N.Ty);
        R = VMVariantEmitter::emitBinaryValue(
            B, N.Opcode, ITy, Ops[0], Ops[1],
            VMVariantEmitter::selectBinaryVariant(N.Opcode, ITy, NodeSeed,
                                                  VMMutationVariantPermille,
                                                  VMRelationAppVariantPermille),
            NodeSeed);
        break;
      }
      }
      Results.push_back(R);
    }

    B.CreateRet(Results.back());
    attrs(F);
    return F;
  }

  static bool isSupportedIntrinsicCall(CallInst *CI) {
    if (CI->getNumOperandBundles() != 0 || CI->isMustTailCall())
      return false;
    auto *RetTy = dyn_cast<IntegerType>(CI->getType());
    if (!RetTy || intrinsicName(CI->getIntrinsicID()).empty())
      return false;

    switch (CI->getIntrinsicID()) {
    case Intrinsic::fshl:
    case Intrinsic::fshr:
      return CI->arg_size() == 3 && CI->getArgOperand(0)->getType() == RetTy &&
             CI->getArgOperand(1)->getType() == RetTy &&
             CI->getArgOperand(2)->getType() == RetTy;
    case Intrinsic::bswap:
    case Intrinsic::bitreverse:
    case Intrinsic::ctpop:
      return CI->arg_size() == 1 && CI->getArgOperand(0)->getType() == RetTy;
    case Intrinsic::ctlz:
    case Intrinsic::cttz:
    case Intrinsic::abs:
      return CI->arg_size() == 2 && CI->getArgOperand(0)->getType() == RetTy &&
             isa<ConstantInt>(CI->getArgOperand(1));
    case Intrinsic::smin:
    case Intrinsic::smax:
    case Intrinsic::umin:
    case Intrinsic::umax:
      return CI->arg_size() == 2 && CI->getArgOperand(0)->getType() == RetTy &&
             CI->getArgOperand(1)->getType() == RetTy;
    default:
      return false;
    }
  }

  static bool isSupportedCast(CastInst *CI) {
    switch (CI->getOpcode()) {
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
      return isSupportedInt(CI->getSrcTy()) && isSupportedInt(CI->getDestTy());
    case Instruction::PtrToInt:
      return isSupportedPointer(CI->getSrcTy()) &&
             isSupportedInt(CI->getDestTy());
    case Instruction::IntToPtr:
      return isSupportedInt(CI->getSrcTy()) &&
             isSupportedPointer(CI->getDestTy());
    default:
      return false;
    }
  }

  static bool isSupportedSelect(SelectInst *SI) {
    return SI->getCondition()->getType()->isIntegerTy(1) &&
           isSupportedScalar(SI->getType()) &&
           SI->getTrueValue()->getType() == SI->getType() &&
           SI->getFalseValue()->getType() == SI->getType();
  }

  struct HandlerKey {
    HandlerKind Kind = HandlerKind::Binary;
    Type *Ty = nullptr;
    Type *SrcTy = nullptr;
    Align Alignment = Align(1);
    SmallVector<VMOpNode, 8> OpNodes;
    SmallVector<Type *, 8> OpParamTys;
    std::string OpKey;
  };

  struct VMMatch {
    SmallVector<Instruction *, 4> Insts;
    SmallVector<TrackingVH<Value>, 8> Args;
    Instruction *ResultInst = nullptr;
  };

  struct VMRewritePlan {
    VMMatch Match;
    HandlerKey Key;
  };

  static bool isSupportedGEP(GetElementPtrInst *GEP) {
    if (!isSupportedPointer(GEP->getPointerOperandType()) ||
        !isSupportedPointer(GEP->getType()))
      return false;
    for (Value *Idx : GEP->indices())
      if (!Idx->getType()->isIntegerTy())
        return false;
    return true;
  }

  static bool isSupportedLoad(LoadInst *LI) {
    return !LI->isVolatile() && !LI->isAtomic() &&
           isSupportedScalar(LI->getType()) &&
           isSupportedPointer(LI->getPointerOperandType());
  }

  static bool isSupportedStore(StoreInst *SI) {
    return !SI->isVolatile() && !SI->isAtomic() &&
           isSupportedScalar(SI->getValueOperand()->getType()) &&
           isSupportedPointer(SI->getPointerOperandType());
  }

  static bool isSupportedVMOpInst(Instruction *I) {
    if (auto *BO = dyn_cast<BinaryOperator>(I))
      return isSupportedInt(BO->getType()) &&
             Instruction::isBinaryOp(BO->getOpcode());
    if (auto *ICI = dyn_cast<ICmpInst>(I))
      return !ICI->hasSameSign() &&
             isSupportedScalar(ICI->getOperand(0)->getType()) &&
             ICI->getOperand(0)->getType() == ICI->getOperand(1)->getType() &&
             CmpInst::isIntPredicate(ICI->getPredicate());
    if (auto *SI = dyn_cast<SelectInst>(I))
      return isSupportedSelect(SI);
    if (auto *GEP = dyn_cast<GetElementPtrInst>(I))
      return isSupportedGEP(GEP);
    if (auto *LI = dyn_cast<LoadInst>(I))
      return isSupportedLoad(LI);
    // Stores stay out of DAGs; only single-op handlers below.
    if (isa<StoreInst>(I))
      return false;
    if (auto *CI = dyn_cast<CastInst>(I))
      return isSupportedCast(CI);
    if (auto *Call = dyn_cast<CallInst>(I))
      return Call->getCalledFunction() &&
             Call->getIntrinsicID() != Intrinsic::not_intrinsic &&
             isSupportedIntrinsicCall(Call);
    return false;
  }

  static HandlerKind handlerKindForRoot(Instruction *I) {
    if (isa<BinaryOperator>(I))
      return HandlerKind::Binary;
    if (isa<ICmpInst>(I))
      return HandlerKind::ICmp;
    if (isa<CallInst>(I))
      return HandlerKind::Intrinsic;
    if (isa<CastInst>(I))
      return HandlerKind::Cast;
    if (isa<SelectInst>(I))
      return HandlerKind::Select;
    if (isa<GetElementPtrInst>(I))
      return HandlerKind::GEP;
    if (isa<LoadInst>(I))
      return HandlerKind::Load;
    llvm_unreachable("unsupported VM op root");
  }

  static bool
  hasMemoryOrderingBarrierBetweenLoadAndRoot(ArrayRef<Instruction *> Insts,
                                             Instruction *Root) {
    SmallPtrSet<Instruction *, 8> InSlice(Insts.begin(), Insts.end());
    for (Instruction *I : Insts) {
      if (!isa<LoadInst>(I) || I == Root)
        continue;

      // Reject moving a load across intervening memory/side-effect ops.
      for (Instruction *Cur = I->getNextNode(); Cur && Cur != Root;
           Cur = Cur->getNextNode()) {
        if (InSlice.contains(Cur))
          continue;
        if (Cur->mayReadOrWriteMemory() || Cur->mayHaveSideEffects())
          return true;
      }
    }
    return false;
  }

  struct SuperOpCandidate {
    SmallVector<Instruction *, 8> Nodes;
    int Score = 0;
  };

  static unsigned opcodeFamily(const Instruction &I) {
    if (isa<CastInst>(I))
      return 1;
    if (isa<ICmpInst>(I) || isa<SelectInst>(I))
      return 2;
    if (isa<GetElementPtrInst>(I) || isa<LoadInst>(I))
      return 3;
    if (isa<CallInst>(I))
      return 4;
    switch (I.getOpcode()) {
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
      return 5;
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
      return 6;
    default:
      return 7;
    }
  }

  static int scoreSuperOp(ArrayRef<Instruction *> Nodes) {
    SmallPtrSet<Instruction *, 8> InCandidate(Nodes.begin(), Nodes.end());
    DenseMap<Instruction *, bool> ConstantDerived;
    DenseMap<Instruction *, unsigned> DynamicDepth;
    SmallSet<unsigned, 8> Families;
    SmallPtrSet<Value *, 8> ExternalValues;
    unsigned MaxDynamicDepth = 0;
    unsigned DynamicNodes = 0;
    unsigned InternalEdges = 0;

    for (Instruction *I : Nodes) {
      bool IsConstantDerived = true;
      Families.insert(opcodeFamily(*I));
      for (Value *Op : I->operands()) {
        if (auto *Dep = dyn_cast<Instruction>(Op)) {
          if (InCandidate.contains(Dep)) {
            if (!ConstantDerived.lookup(Dep))
              IsConstantDerived = false;
            continue;
          }
        }
        if (!isa<Constant>(Op)) {
          IsConstantDerived = false;
          ExternalValues.insert(Op);
        }
      }
      ConstantDerived[I] = IsConstantDerived;
      if (IsConstantDerived)
        continue;

      ++DynamicNodes;
      unsigned D = 1;
      for (Value *Op : I->operands())
        if (auto *Dep = dyn_cast<Instruction>(Op))
          if (InCandidate.contains(Dep) && !ConstantDerived.lookup(Dep))
            D = std::max(D, DynamicDepth.lookup(Dep) + 1);
      DynamicDepth[I] = D;
      MaxDynamicDepth = std::max(MaxDynamicDepth, D);
    }

    for (Instruction *I : Nodes) {
      SmallPtrSet<Instruction *, 4> SeenDeps;
      for (Value *Op : I->operands()) {
        if (auto *Dep = dyn_cast<Instruction>(Op))
          if (InCandidate.contains(Dep) && SeenDeps.insert(Dep).second)
            ++InternalEdges;
      }
    }

    unsigned ContinuousConstantCost = 0;
    SmallPtrSet<Instruction *, 8> VisitedConstants;
    for (Instruction *Start : Nodes) {
      if (!ConstantDerived.lookup(Start) ||
          !VisitedConstants.insert(Start).second)
        continue;
      unsigned ComponentSize = 0;
      SmallVector<Instruction *, 8> Worklist = {Start};
      while (!Worklist.empty()) {
        Instruction *I = Worklist.pop_back_val();
        ++ComponentSize;
        for (Value *Op : I->operands())
          if (auto *Dep = dyn_cast<Instruction>(Op))
            if (InCandidate.contains(Dep) && ConstantDerived.lookup(Dep) &&
                VisitedConstants.insert(Dep).second)
              Worklist.push_back(Dep);
        for (User *U : I->users())
          if (auto *UI = dyn_cast<Instruction>(U))
            if (InCandidate.contains(UI) && ConstantDerived.lookup(UI) &&
                VisitedConstants.insert(UI).second)
              Worklist.push_back(UI);
      }
      ContinuousConstantCost += ComponentSize - 1;
    }

    // Score terms peak near 24 at the default 4-node limit.
    unsigned ExternalArity = std::min<unsigned>(ExternalValues.size(), 6);
    return static_cast<int>(DynamicNodes) * 6 +
           static_cast<int>(MaxDynamicDepth) * 8 +
           static_cast<int>(Families.size()) * 6 +
           static_cast<int>(InternalEdges) * 4 -
           static_cast<int>(ContinuousConstantCost) * 12 -
           static_cast<int>(ExternalArity) * 4;
  }

  static bool sameCandidate(ArrayRef<Instruction *> A,
                            ArrayRef<Instruction *> B) {
    return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin());
  }

  static void collectRootCone(Instruction *Root,
                              SmallPtrSetImpl<Instruction *> &RootCone) {
    SmallVector<Instruction *, 16> Worklist = {Root};
    RootCone.insert(Root);
    while (!Worklist.empty()) {
      Instruction *I = Worklist.pop_back_val();
      for (Value *Op : I->operands()) {
        auto *Dep = dyn_cast<Instruction>(Op);
        if (!Dep || Dep->getParent() != Root->getParent() ||
            !isSupportedVMOpInst(Dep) || !RootCone.insert(Dep).second)
          continue;
        Worklist.push_back(Dep);
      }
    }
  }

  static bool
  isInlineableInRootCone(Instruction *I, Instruction *Root,
                         const SmallPtrSetImpl<Instruction *> &RootCone) {
    if (I == Root)
      return true;
    for (User *U : I->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || UI->getParent() != Root->getParent() ||
          !isSupportedVMOpInst(UI) || !RootCone.contains(UI))
        return false;
    }
    return true;
  }

  static void
  collectExpandableDeps(Instruction *Root, ArrayRef<Instruction *> Nodes,
                        const SmallPtrSetImpl<Instruction *> &RootCone,
                        SmallVectorImpl<Instruction *> &Deps) {
    SmallPtrSet<Instruction *, 8> Seen(Nodes.begin(), Nodes.end());
    for (Instruction *Cur : Nodes) {
      for (Value *Op : Cur->operands()) {
        auto *Dep = dyn_cast<Instruction>(Op);
        if (!Dep || !RootCone.contains(Dep) || Seen.contains(Dep) ||
            !isInlineableInRootCone(Dep, Root, RootCone))
          continue;
        Deps.push_back(Dep);
      }
    }
    llvm::sort(
        Deps, [](Instruction *A, Instruction *B) { return A->comesBefore(B); });
    Deps.erase(std::unique(Deps.begin(), Deps.end()), Deps.end());
  }

  static bool
  expandWithUserClosure(Instruction *Dep, Instruction *Root, unsigned Budget,
                        const SmallPtrSetImpl<Instruction *> &RootCone,
                        SuperOpCandidate &Candidate) {
    SmallPtrSet<Instruction *, 8> InCandidate(Candidate.Nodes.begin(),
                                              Candidate.Nodes.end());
    SmallPtrSet<Instruction *, 8> Pending;
    SmallVector<Instruction *, 8> Worklist = {Dep};
    while (!Worklist.empty()) {
      Instruction *I = Worklist.pop_back_val();
      if (InCandidate.contains(I) || !Pending.insert(I).second)
        continue;
      if (!RootCone.contains(I) || !isInlineableInRootCone(I, Root, RootCone) ||
          InCandidate.size() + Pending.size() > Budget)
        return false;
      for (User *U : I->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI)
          return false;
        if (!InCandidate.contains(UI))
          Worklist.push_back(UI);
      }
    }
    Candidate.Nodes.append(Pending.begin(), Pending.end());
    llvm::sort(Candidate.Nodes, [](Instruction *A, Instruction *B) {
      return A->comesBefore(B);
    });
    return true;
  }

  static bool isClosedAndSafe(const SuperOpCandidate &Candidate,
                              Instruction *Root) {
    SmallPtrSet<Instruction *, 8> InSlice(Candidate.Nodes.begin(),
                                          Candidate.Nodes.end());
    if (hasMemoryOrderingBarrierBetweenLoadAndRoot(Candidate.Nodes, Root))
      return false;
    for (Instruction *I : Candidate.Nodes) {
      if (I == Root)
        continue;
      for (User *U : I->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || !InSlice.contains(UI))
          return false;
      }
    }
    return true;
  }

  static void enumerateSuperOps(Instruction *Root, unsigned Budget,
                                SmallVectorImpl<SuperOpCandidate> &Candidates) {
    if (!Root || !isSupportedVMOpInst(Root) || Budget == 0)
      return;

    SmallPtrSet<Instruction *, 16> RootCone;
    collectRootCone(Root, RootCone);

    SmallVector<SuperOpCandidate, 16> Beam;
    SuperOpCandidate Singleton{{Root}, scoreSuperOp({Root})};
    Candidates.push_back(Singleton);
    Beam.push_back(std::move(Singleton));
    while (!Beam.empty()) {
      SmallVector<SuperOpCandidate, 32> Next;
      for (const SuperOpCandidate &Candidate : Beam) {
        SmallVector<Instruction *, 8> Deps;
        collectExpandableDeps(Root, Candidate.Nodes, RootCone, Deps);
        for (Instruction *Dep : Deps) {
          SuperOpCandidate Expanded = Candidate;
          if (!expandWithUserClosure(Dep, Root, Budget, RootCone, Expanded))
            continue;
          bool Duplicate =
              llvm::any_of(Candidates,
                           [&](const auto &Existing) {
                             return sameCandidate(Existing.Nodes,
                                                  Expanded.Nodes);
                           }) ||
              llvm::any_of(Next, [&](const auto &Existing) {
                return sameCandidate(Existing.Nodes, Expanded.Nodes);
              });
          if (Duplicate)
            continue;
          Expanded.Score = scoreSuperOp(Expanded.Nodes);
          if (isClosedAndSafe(Expanded, Root))
            Next.push_back(std::move(Expanded));
        }
      }
      llvm::sort(
          Next, [](const auto &A, const auto &B) { return A.Score > B.Score; });
      if (Next.size() > 16)
        Next.resize(16);
      Candidates.append(Next.begin(), Next.end());
      Beam.assign(Next.begin(), Next.end());
    }
  }

  static void collectVMOpInsts(Instruction *Root, unsigned Budget,
                               uint64_t Seed,
                               SmallVectorImpl<Instruction *> &Nodes) {
    SmallVector<SuperOpCandidate, 32> Candidates;
    enumerateSuperOps(Root, Budget, Candidates);
    if (Candidates.empty())
      return;

    int BestScore = Candidates.front().Score;
    for (const SuperOpCandidate &Candidate : Candidates)
      BestScore = std::max(BestScore, Candidate.Score);

    // Softmax-style weights within 24 of the best score; worse candidates get 0.
    static constexpr unsigned ScoreWindow = 24;
    SmallVector<unsigned, 32> Weights;
    SmallVector<unsigned, 32> Eligible;
    for (unsigned I = 0; I != Candidates.size(); ++I) {
      const SuperOpCandidate &Candidate = Candidates[I];
      int Deficit = BestScore - Candidate.Score;
      unsigned Weight = Deficit <= static_cast<int>(ScoreWindow)
                            ? ScoreWindow - static_cast<unsigned>(Deficit) + 1
                            : 0;
      Weights.push_back(Weight);
      if (Weight)
        Eligible.push_back(I);
    }

    // Weight-proportional rejection sampling via YansoRNG::range.
    YansoRNG RNG(Seed);
    unsigned Selected;
    do {
      Selected = Eligible[RNG.range(Eligible.size())];
    } while (RNG.range(ScoreWindow + 1) >= Weights[Selected]);
    Nodes.append(Candidates[Selected].Nodes.begin(),
                 Candidates[Selected].Nodes.end());
  }

  static std::string printKeyValue(Value *V) {
    std::string S;
    raw_string_ostream OS(S);
    V->print(OS);
    OS.flush();
    return S;
  }

  static std::string printKeyType(Type *Ty) {
    std::string S;
    raw_string_ostream OS(S);
    Ty->print(OS);
    OS.flush();
    return S;
  }

  static void appendKeyField(raw_ostream &OS, StringRef Field) {
    OS << Field.size() << ':' << Field << ';';
  }

  static void appendTypeKey(raw_ostream &OS, Type *Ty) {
    appendKeyField(OS, Ty ? printKeyType(Ty) : StringRef());
  }

  static std::string superOpStructuralKey(ArrayRef<VMOpNode> Nodes,
                                          ArrayRef<Type *> ParamTys,
                                          Type *RetTy) {
    std::string Key;
    raw_string_ostream OS(Key);
    OS << "superop;ret=";
    appendTypeKey(OS, RetTy);
    OS << "params=" << ParamTys.size() << ';';
    for (Type *ParamTy : ParamTys)
      appendTypeKey(OS, ParamTy);
    OS << "nodes=" << Nodes.size() << ';';
    for (const VMOpNode &N : Nodes) {
      OS << "node;op=" << N.Opcode << ";pred=" << unsigned(N.Predicate)
         << ";intr=" << unsigned(N.IntrinsicID)
         << ";align=" << N.Alignment.value()
         << ";inbounds=" << unsigned(N.InBounds) << ";ty=";
      appendTypeKey(OS, N.Ty);
      OS << "src=";
      appendTypeKey(OS, N.SrcTy);
      OS << "dst=";
      appendTypeKey(OS, N.DstTy);
      OS << "source=";
      appendTypeKey(OS, N.SourceElementTy);
      OS << "inputs=" << N.Inputs.size() << ';';
      for (int Ref : N.Inputs)
        OS << Ref << ',';
      OS << ";constants=" << N.Constants.size() << ';';
      for (Value *C : N.Constants)
        appendKeyField(OS, printKeyValue(C));
    }
    OS.flush();
    return Key;
  }

  uint64_t limitVariantSeed(const HandlerKey &Key, uint64_t Seed) {
    if (Key.OpNodes.empty() || VMMaxVariantsPerSuperOp == 0)
      return Seed;

    SmallVector<uint64_t, 8> &Bucket = SuperOpVariantBuckets[Key.OpKey];
    if (Bucket.size() < VMMaxVariantsPerSuperOp) {
      Bucket.push_back(Seed);
      return Seed;
    }

    uint64_t Pick = yanso_mix64(Seed, 0x94d049bb133111ebULL) % Bucket.size();
    return Bucket[Pick];
  }

  bool addVMOpPlan(SmallVectorImpl<VMRewritePlan> &Plans, Instruction *Root,
                   SmallPtrSetImpl<Instruction *> &Consumed) {
    if (Consumed.contains(Root) || !isSupportedVMOpInst(Root))
      return false;

    SmallVector<Instruction *, 8> Insts;
    unsigned MaxLen = std::max(1U, static_cast<unsigned>(VMSuperOpMaxLen));
    collectVMOpInsts(Root, MaxLen, instructionSeed(Root, ModuleSeed), Insts);
    if (Insts.empty())
      return false;

    for (Instruction *I : Insts)
      if (Consumed.contains(I))
        return false;

    SmallPtrSet<Instruction *, 8> InSlice(Insts.begin(), Insts.end());
    if (hasMemoryOrderingBarrierBetweenLoadAndRoot(Insts, Root))
      return false;

    for (Instruction *I : Insts) {
      if (I == Root)
        continue;
      for (User *U : I->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || !InSlice.contains(UI))
          return false;
      }
    }

    DenseMap<Value *, int> ArgIndex;
    DenseMap<Instruction *, unsigned> NodeIndex;
    SmallVector<Value *, 8> Args;
    SmallVector<Type *, 8> ParamTys;
    SmallVector<VMOpNode, 8> Nodes;
    for (unsigned NI = 0; NI != Insts.size(); ++NI)
      NodeIndex[Insts[NI]] = NI;

    for (Instruction *I : Insts) {
      VMOpNode N;
      N.Opcode = isa<ICmpInst>(I) ? Instruction::ICmp : I->getOpcode();
      N.Ty = I->getType();
      if (auto *ICI = dyn_cast<ICmpInst>(I))
        N.Predicate = ICI->getPredicate();
      if (auto *Call = dyn_cast<CallInst>(I))
        N.IntrinsicID = Call->getIntrinsicID();
      if (auto *CI = dyn_cast<CastInst>(I)) {
        N.SrcTy = CI->getSrcTy();
        N.DstTy = CI->getDestTy();
      }
      if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
        N.SourceElementTy = GEP->getSourceElementType();
        N.InBounds = GEP->isInBounds();
      }
      if (auto *LI = dyn_cast<LoadInst>(I))
        N.Alignment = LI->getAlign();
      for (Use &U : I->operands()) {
        Value *Op = U.get();
        if (auto *Call = dyn_cast<CallInst>(I)) {
          if (Op == Call->getCalledOperand())
            continue;
        }
        if (auto *Dep = dyn_cast<Instruction>(Op)) {
          auto It = NodeIndex.find(Dep);
          if (It != NodeIndex.end()) {
            N.Inputs.push_back(-static_cast<int>(It->second) - 1);
            continue;
          }
        }
        if (isa<Constant>(Op)) {
          N.Inputs.push_back(ConstantRefBase -
                             static_cast<int>(N.Constants.size()));
          N.Constants.push_back(Op);
          continue;
        }
        auto It = ArgIndex.find(Op);
        if (It == ArgIndex.end()) {
          int Idx = Args.size();
          ArgIndex[Op] = Idx;
          Args.push_back(Op);
          ParamTys.push_back(Op->getType());
          N.Inputs.push_back(Idx);
        } else {
          N.Inputs.push_back(It->second);
        }
      }
      Nodes.push_back(std::move(N));
    }

    HandlerKey Key;
    Key.Kind = handlerKindForRoot(Root);
    Key.Ty = Root->getType();
    Key.OpNodes = Nodes;
    Key.OpParamTys = ParamTys;
    Key.OpKey = superOpStructuralKey(Key.OpNodes, Key.OpParamTys, Key.Ty);

    VMRewritePlan Plan;
    for (Instruction *I : Insts)
      Plan.Match.Insts.push_back(I);
    Plan.Match.ResultInst = Root;
    for (Value *Arg : Args)
      Plan.Match.Args.push_back(Arg);
    Plan.Key = std::move(Key);
    Plans.push_back(std::move(Plan));
    for (Instruction *I : Insts)
      Consumed.insert(I);
    return true;
  }

  void addStorePlan(SmallVectorImpl<VMRewritePlan> &Plans, StoreInst *SI) {
    if (!isSupportedStore(SI))
      return;

    VMRewritePlan Plan;
    Plan.Match.Insts.push_back(SI);
    Plan.Match.Args.push_back(SI->getValueOperand());
    Plan.Match.Args.push_back(SI->getPointerOperand());
    Plan.Match.ResultInst = SI;
    Plan.Key.Kind = HandlerKind::Store;
    Plan.Key.Ty = SI->getValueOperand()->getType();
    Plan.Key.SrcTy = SI->getPointerOperandType();
    Plan.Key.Alignment = SI->getAlign();
    Plans.push_back(std::move(Plan));
  }

  Function *createHandler(Module &M, const HandlerKey &Key,
                          uint64_t VariantSeed) {
    if (!Key.OpNodes.empty())
      return createSuperOpHandler(M, Key.OpNodes, Key.OpParamTys, Key.Ty,
                                  Key.OpKey, VariantSeed);

    if (Key.Kind != HandlerKind::Store)
      llvm_unreachable("non-store VM plan has no super-op nodes");
    return createStoreHandler(M, Key.Ty, Key.SrcTy, Key.Alignment);
  }

  bool rewritePlan(Module &M, VMRewritePlan &Plan) {
    if (!Plan.Match.ResultInst || Plan.Match.Insts.empty())
      return false;

    uint64_t VariantSeed = ModuleSeed;
    VariantSeed =
        yanso_mix64(static_cast<uint64_t>(Plan.Key.Kind) + 1, VariantSeed);
    if (!Plan.Key.OpNodes.empty()) {
      const VMOpNode &RootNode = Plan.Key.OpNodes.back();
      VariantSeed = yanso_mix64(RootNode.Opcode + 1, VariantSeed);
      VariantSeed = yanso_mix64(RootNode.Predicate + 1, VariantSeed);
      VariantSeed = yanso_mix64(RootNode.IntrinsicID + 1, VariantSeed);
    } else {
      VariantSeed = yanso_mix64(1, VariantSeed);
      VariantSeed = yanso_mix64(CmpInst::ICMP_EQ + 1, VariantSeed);
      VariantSeed = yanso_mix64(Intrinsic::not_intrinsic + 1, VariantSeed);
    }
    VariantSeed = instructionSeed(Plan.Match.ResultInst, VariantSeed);
    VariantSeed = limitVariantSeed(Plan.Key, VariantSeed);

    SmallVector<Value *, 8> Args;
    for (TrackingVH<Value> &Arg : Plan.Match.Args) {
      if (!Arg)
        return false;
      Args.push_back(Arg);
    }

    Function *Func = createHandler(M, Plan.Key, VariantSeed);
    if (!Func)
      return false;

    IRBuilder<> B(Plan.Match.ResultInst);
    CallInst *Call = B.CreateCall(Func, Args);
    if (!Plan.Match.ResultInst->getType()->isVoidTy())
      Plan.Match.ResultInst->replaceAllUsesWith(Call);

    for (Instruction *I : reverse(Plan.Match.Insts))
      if (I->getParent())
        I->eraseFromParent();
    return true;
  }

public:
  bool run(Module &M) {
    ModuleSeed = yanso_module_seed(M, "vm");
    SmallVector<VMRewritePlan, 64> Plans;
    SmallPtrSet<Instruction *, 32> Consumed;
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      // Skip funclet EH: VM calls would need matching "funclet" bundles.
      if (usesFuncletEH(F))
        continue;
      for (BasicBlock &BB : F) {
        SmallVector<Instruction *, 32> Roots;
        for (Instruction &I : BB)
          Roots.push_back(&I);
        for (Instruction *I : reverse(Roots)) {
          addVMOpPlan(Plans, I, Consumed);
          assert((!isSupportedVMOpInst(I) || Consumed.contains(I)) &&
                 "supported VM op was not assigned to a super-op plan");
        }
      }
    }

    for (Function &F : M) {
      if (usesFuncletEH(F))
        continue;
      for (BasicBlock &BB : F)
        for (Instruction &I : BB)
          if (auto *SI = dyn_cast<StoreInst>(&I); SI && isSupportedStore(SI))
            addStorePlan(Plans, SI);
    }

    bool Modified = false;
    for (VMRewritePlan &Plan : Plans)
      Modified |= rewritePlan(M, Plan);

    return Modified;
  }
};
} // namespace

PreservedAnalyses VMPass::run(Module &M, ModuleAnalysisManager &) {
  VirtualizeImpl Impl;
  return Impl.run(M) ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
