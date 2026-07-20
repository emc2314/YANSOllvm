#include "ObfConPass.h"

#include "CryptoUtils.h"
#include "YANSOllvmSeed.h"
#include "YConstants.h"
#include "YMBA.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/Utils/Local.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"

#include <memory>
#include <numeric>
#include <optional>
#include <utility>

using namespace llvm;

namespace {

static cl::opt<unsigned> ObfConKnownConstantPermille(
    "obfcon-known-constant-permille", cl::init(250), cl::Hidden,
    cl::desc("Permille probability of choosing an ObfCon split value from the "
             "well-known constant pool"));

enum class SplitKind : uint8_t { Add, Xor, And, Or };
enum class ArrayCombineKind : uint8_t { Add, Xor, Sub };

struct OperandSite {
  Instruction *Owner = nullptr;
  unsigned OperandNo = 0;
  uint64_t Seed = 0;
};

struct ArrayLoadUse {
  LoadInst *Load = nullptr;
  Value *Pointer = nullptr;
};

struct ArrayMemcpyUse {
  MemCpyInst *Memcpy = nullptr;
  Value *Source = nullptr;
};

struct ArrayUsePlan {
  GlobalVariable *Global = nullptr;
  ArrayType *Type = nullptr;
  IntegerType *ElementType = nullptr;
  SmallVector<ArrayLoadUse, 16> Loads;
  SmallVector<ArrayMemcpyUse, 4> Memcpys;
  SmallVector<WeakTrackingVH, 16> PointerInstructions;
};

struct EncodedArray {
  GlobalVariable *ShareA = nullptr;
  GlobalVariable *ShareB = nullptr;
  uint64_t LogicalBytes = 0;
  uint64_t PhysicalSlots = 0;
  uint64_t AffineA = 1;
  uint64_t AffineB = 0;
  ArrayCombineKind Combine = ArrayCombineKind::Xor;
  bool PackedI32 = false;
};

static bool isSupportedWidth(Type *Ty) {
  auto *ITy = dyn_cast<IntegerType>(Ty);
  if (!ITy)
    return false;
  unsigned BW = ITy->getBitWidth();
  return BW == 8 || BW == 16 || BW == 32 || BW == 64;
}

static ConstantInt *constant(IntegerType *Ty, const APInt &V) {
  return ConstantInt::get(Ty->getContext(), V.zextOrTrunc(Ty->getBitWidth()));
}

static BinaryOperator *insertBin(Instruction::BinaryOps Opcode, Value *L,
                                 Value *R, Instruction *Before,
                                 const Twine &Name = "") {
  return BinaryOperator::Create(Opcode, L, R, Name, Before->getIterator());
}

// Identity binop wrapper so ConstantInt operands are not folded away.
static Value *opaqueInt(Instruction::BinaryOps Op, IntegerType *Ty,
                        const APInt &V, Instruction *Before,
                        const Twine &Name) {
  return insertBin(Op, constant(Ty, V), ConstantInt::get(Ty, 0), Before, Name);
}

static bool isSemanticFlagged(const BinaryOperator &BO) {
  auto *OBO = dyn_cast<OverflowingBinaryOperator>(&BO);
  auto *PEO = dyn_cast<PossiblyExactOperator>(&BO);
  auto *PDI = dyn_cast<PossiblyDisjointInst>(&BO);
  return (OBO && (OBO->hasNoSignedWrap() || OBO->hasNoUnsignedWrap())) ||
         (PEO && PEO->isExact()) || (PDI && PDI->isDisjoint());
}

class ObfConImpl {
  Module &M;
  uint64_t ModuleSeed;
  bool Modified = false;

  static Instruction *insertionPoint(const OperandSite &Site) {
    if (auto *Phi = dyn_cast<PHINode>(Site.Owner)) {
      if (Site.OperandNo >= Phi->getNumIncomingValues())
        return nullptr;
      Instruction *Term =
          Phi->getIncomingBlock(Site.OperandNo)->getTerminator();
      return isa<CatchSwitchInst>(Term) ? nullptr : Term;
    }
    return Site.Owner;
  }

  static bool isReplaceableGEPOperand(GetElementPtrInst &GEP,
                                      unsigned OperandNo) {
    if (OperandNo == 0)
      return false;
    auto It = gep_type_begin(&GEP);
    for (unsigned I = 1; I < OperandNo; ++I)
      ++It;
    return !It.isStruct();
  }

  static std::optional<unsigned> callArgumentNumber(CallBase &Call,
                                                    unsigned OperandNo) {
    Use &U = Call.getOperandUse(OperandNo);
    if (!Call.isArgOperand(&U))
      return std::nullopt;
    return Call.getArgOperandNo(&U);
  }

  static bool isLegalSite(Instruction &I, unsigned OperandNo) {
    if (OperandNo >= I.getNumOperands() ||
        !isSupportedWidth(I.getOperand(OperandNo)->getType()) ||
        !isa<ConstantInt>(I.getOperand(OperandNo)))
      return false;

    if (I.isEHPad() || isa<AllocaInst>(I))
      return false;
    if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
      return isReplaceableGEPOperand(*GEP, OperandNo);
    if (auto *Switch = dyn_cast<SwitchInst>(&I))
      return OperandNo == 0 &&
             Switch->getCondition() == I.getOperand(OperandNo);
    if (auto *Call = dyn_cast<CallBase>(&I)) {
      if (Call->isInlineAsm())
        return false;
      std::optional<unsigned> ArgNo = callArgumentNumber(*Call, OperandNo);
      if (!ArgNo)
        return false;
      return !Call->paramHasAttr(*ArgNo, Attribute::ImmArg);
    }
    return true;
  }

  static bool functionUsesFuncletEH(const Function &F) {
    for (const BasicBlock &BB : F)
      if (BB.isEHPad() && !BB.isLandingPad())
        return true;
    return false;
  }

  static IntegerType *indexType(const GlobalVariable &GV) {
    return cast<IntegerType>(GV.getDataLayout().getIndexType(GV.getType()));
  }

  static uint64_t siteSeed(Function &F, unsigned BlockIndex, unsigned InstIndex,
                           unsigned OperandNo) {
    uint64_t H = yanso_function_seed(F, "obfcon");
    H = yanso_mix64(BlockIndex + 1, H);
    H = yanso_mix64(InstIndex + 1, H);
    return yanso_mix64(OperandNo + 1, H);
  }

  SmallVector<OperandSite, 64> collectSites(bool ZeroAndOneOnly) {
    SmallVector<OperandSite, 64> Sites;
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      unsigned BlockIndex = 0;
      for (BasicBlock &BB : F) {
        unsigned InstIndex = 0;
        for (Instruction &I : BB) {
          for (unsigned Op = 0; Op != I.getNumOperands(); ++Op) {
            auto *C = dyn_cast<ConstantInt>(I.getOperand(Op));
            if (!C || !isLegalSite(I, Op))
              continue;
            bool IsZeroOrOne = C->isZero() || C->isOne();
            if (IsZeroOrOne != ZeroAndOneOnly)
              continue;
            Sites.push_back({&I, Op, siteSeed(F, BlockIndex, InstIndex, Op)});
          }
          ++InstIndex;
        }
        ++BlockIndex;
      }
    }
    return Sites;
  }

  APInt randomAPInt(unsigned BW, YansoRNG &RNG) const {
    return APInt(BW, RNG.next64(), false, true);
  }

  APInt chooseSplitValue(IntegerType *Ty, YansoRNG &RNG,
                         bool AllowCrypto = true) const {
    unsigned BW = Ty->getBitWidth();
    ArrayRef<uint64_t> Pool =
        AllowCrypto ? yansoKnownConstants(BW) : ArrayRef<uint64_t>();
    APInt V(BW, 0);
    if (!Pool.empty() && ObfConKnownConstantPermille != 0 &&
        (ObfConKnownConstantPermille >= 1000 ||
         RNG.range(1000) < ObfConKnownConstantPermille)) {
      V = APInt(BW, Pool[RNG.range(Pool.size())], false, true);
    } else {
      V = randomAPInt(BW, RNG);
    }
    if (V.isZero() || V.isOne())
      V = APInt(BW, RNG.next64() | 3ULL, false, true);
    return V;
  }

  std::pair<APInt, APInt> splitConstant(const APInt &C, SplitKind Kind,
                                        YansoRNG &RNG) const {
    unsigned BW = C.getBitWidth();
    APInt R = chooseSplitValue(IntegerType::get(M.getContext(), BW), RNG);
    switch (Kind) {
    case SplitKind::Add:
      return {R, C - R};
    case SplitKind::Xor:
      return {R, C ^ R};
    case SplitKind::And:
      return {C | R, C | ~R};
    case SplitKind::Or:
      return {C & R, C & ~R};
    }
    llvm_unreachable("unknown ObfCon split kind");
  }

  Value *materializeConstant(ConstantInt &C, Instruction *Before,
                             uint64_t Seed) {
    auto *Ty = cast<IntegerType>(C.getType());
    YansoRNG RNG(Seed);
    APInt V = C.getValue();
    switch (RNG.range(3)) {
    case 0: {
      APInt R = chooseSplitValue(Ty, RNG);
      R.setBit(0);
      APInt Factor = yanso_mod_inverse(R) * V;
      auto Lo = RNG.range(2) ? Instruction::Add : Instruction::Xor;
      auto Ro = RNG.range(2) ? Instruction::Add : Instruction::Xor;
      Value *L = opaqueInt(Lo, Ty, R, Before, "obfcon.factor");
      Value *RHS = opaqueInt(Ro, Ty, Factor, Before, "obfcon.product");
      return insertBin(Instruction::Mul, L, RHS, Before, "obfcon.constant");
    }
    case 1: {
      auto [A, B] = splitConstant(V, SplitKind::Xor, RNG);
      Value *L = opaqueInt(Instruction::Xor, Ty, A, Before, "obfcon.xor.l");
      Value *R = opaqueInt(Instruction::Add, Ty, B, Before, "obfcon.xor.r");
      return insertBin(Instruction::Xor, L, R, Before, "obfcon.constant");
    }
    default: {
      auto [A, B] = splitConstant(V, SplitKind::Add, RNG);
      Value *L = opaqueInt(Instruction::Add, Ty, A, Before, "obfcon.add.l");
      Value *R = opaqueInt(Instruction::Xor, Ty, B, Before, "obfcon.add.r");
      return insertBin(Instruction::Add, L, R, Before, "obfcon.constant");
    }
    }
  }

  static void replaceOldUses(Value *Replacement, ArrayRef<Use *> Uses) {
    for (Use *U : Uses)
      U->set(Replacement);
  }

  bool rewriteContextually(const OperandSite &Site, ConstantInt &C) {
    auto *BO = dyn_cast<BinaryOperator>(Site.Owner);
    if (!BO || isSemanticFlagged(*BO) || Site.OperandNo > 1)
      return false;

    Value *Dynamic = BO->getOperand(Site.OperandNo ^ 1);
    if (isa<Constant>(Dynamic))
      return false;

    unsigned Opcode = BO->getOpcode();
    if (Opcode != Instruction::Add && Opcode != Instruction::Sub &&
        Opcode != Instruction::Mul && Opcode != Instruction::And &&
        Opcode != Instruction::Or && Opcode != Instruction::Xor)
      return false;

    SmallVector<Use *, 8> OldUses;
    for (Use &U : BO->uses())
      OldUses.push_back(&U);

    YansoRNG RNG(Site.Seed);
    auto *Ty = cast<IntegerType>(C.getType());
    APInt CV = C.getValue();
    Instruction *Before = BO->getNextNode();
    Value *Result = nullptr;

    if (Opcode == Instruction::Add || Opcode == Instruction::Xor) {
      SplitKind Kind =
          Opcode == Instruction::Add ? SplitKind::Add : SplitKind::Xor;
      auto [A, B] = splitConstant(CV, Kind, RNG);
      BO->setOperand(Site.OperandNo, constant(Ty, A));
      Result = insertBin(static_cast<Instruction::BinaryOps>(Opcode), BO,
                         constant(Ty, B), Before, "obfcon.reassoc");
    } else if (Opcode == Instruction::Sub) {
      auto [A, B] = splitConstant(CV, SplitKind::Add, RNG);
      BO->setOperand(Site.OperandNo, constant(Ty, A));
      Result = Site.OperandNo == 1
                   ? static_cast<Value *>(insertBin(Instruction::Sub, BO,
                                                    constant(Ty, B), Before,
                                                    "obfcon.reassoc"))
                   : static_cast<Value *>(insertBin(Instruction::Add, BO,
                                                    constant(Ty, B), Before,
                                                    "obfcon.reassoc"));
    } else if (Opcode == Instruction::Mul) {
      auto [A, B] = splitConstant(CV, SplitKind::Add, RNG);
      IRBuilder<> Builder(BO);
      Value *StableDynamic = Builder.CreateFreeze(Dynamic, "obfcon.freeze");
      BO->setOperand(Site.OperandNo ^ 1, StableDynamic);
      BO->setOperand(Site.OperandNo, constant(Ty, A));
      Value *Other = insertBin(Instruction::Mul, StableDynamic, constant(Ty, B),
                               Before, "obfcon.dist");
      Result = insertBin(Instruction::Add, BO, Other, Before, "obfcon.merge");
    } else if (Opcode == Instruction::And && RNG.range(2)) {
      auto [A, B] = splitConstant(CV, SplitKind::Xor, RNG);
      IRBuilder<> Builder(BO);
      Value *StableDynamic = Builder.CreateFreeze(Dynamic, "obfcon.freeze");
      BO->setOperand(Site.OperandNo ^ 1, StableDynamic);
      BO->setOperand(Site.OperandNo, constant(Ty, A));
      Value *Other = insertBin(Instruction::And, StableDynamic, constant(Ty, B),
                               Before, "obfcon.dist");
      Result = insertBin(Instruction::Xor, BO, Other, Before, "obfcon.merge");
    } else if (Opcode == Instruction::Or && RNG.range(2)) {
      auto [A, B] = splitConstant(CV, SplitKind::And, RNG);
      IRBuilder<> Builder(BO);
      Value *StableDynamic = Builder.CreateFreeze(Dynamic, "obfcon.freeze");
      BO->setOperand(Site.OperandNo ^ 1, StableDynamic);
      BO->setOperand(Site.OperandNo, constant(Ty, A));
      Value *Other = insertBin(Instruction::Or, StableDynamic, constant(Ty, B),
                               Before, "obfcon.dist");
      Result = insertBin(Instruction::And, BO, Other, Before, "obfcon.merge");
    } else {
      SplitKind Kind =
          Opcode == Instruction::And ? SplitKind::And : SplitKind::Or;
      auto [A, B] = splitConstant(CV, Kind, RNG);
      BO->setOperand(Site.OperandNo, constant(Ty, A));
      Result = insertBin(static_cast<Instruction::BinaryOps>(Opcode), BO,
                         constant(Ty, B), Before, "obfcon.reassoc");
    }

    if (!Result)
      return false;
    if (auto *RI = dyn_cast<Instruction>(Result)) {
      RI->setDebugLoc(BO->getDebugLoc());
    }
    replaceOldUses(Result, OldUses);
    return true;
  }

  void rewriteOrdinaryConstants() {
    SmallVector<OperandSite, 64> Sites = collectSites(false);
    SmallPtrSet<Instruction *, 32> StructurallyHandled;
    for (const OperandSite &Site : Sites) {
      Instruction *Owner = Site.Owner;
      if (!Owner->getParent() || Site.OperandNo >= Owner->getNumOperands() ||
          StructurallyHandled.contains(Owner))
        continue;
      auto *C = dyn_cast<ConstantInt>(Owner->getOperand(Site.OperandNo));
      if (!C || C->isZero() || C->isOne())
        continue;
      if (rewriteContextually(Site, *C)) {
        StructurallyHandled.insert(Owner);
        Modified = true;
        continue;
      }
      Instruction *Before = insertionPoint(Site);
      if (!Before)
        continue;
      Value *Replacement = materializeConstant(*C, Before, Site.Seed);
      Owner->setOperand(Site.OperandNo, Replacement);
      Modified = true;
    }
  }

  SmallVector<Value *, 32> liveIntegers(const OperandSite &Site,
                                        IntegerType *Ty, Instruction *Before,
                                        DominatorTree &DT) const {
    SmallVector<Value *, 32> Values;
    Function *F = Site.Owner->getFunction();
    for (Argument &Arg : F->args())
      if (Arg.getType() == Ty)
        Values.push_back(&Arg);

    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (&I == Before || I.getType() != Ty ||
            isa<PHINode>(&I) && I.getParent() == Before->getParent())
          continue;
        if (DT.dominates(&I, Before))
          Values.push_back(&I);
      }
    }
    return Values;
  }

  Value *materializeZeroOrOne(const OperandSite &Site, ConstantInt &C,
                              Instruction *Before, DominatorTree &DT) {
    auto *Ty = cast<IntegerType>(C.getType());
    SmallVector<Value *, 32> Values = liveIntegers(Site, Ty, Before, DT);
    YansoRNG RNG(Site.Seed);
    auto ConstantFallback = [&]() -> Value * {
      APInt A = chooseSplitValue(Ty, RNG, false);
      if (C.isZero())
        return insertBin(Instruction::Xor, constant(Ty, A), constant(Ty, A),
                         Before, "obfcon.zero");
      return insertBin(Instruction::Sub, constant(Ty, A), constant(Ty, A - 1),
                       Before, "obfcon.one");
    };
    // Funclet EH: keep 0/1 materialization arithmetic-only (no calls).
    if (Values.empty() || functionUsesFuncletEH(*Site.Owner->getFunction()))
      return ConstantFallback();

    IRBuilder<> B(Before);
    Value *X = Values[RNG.range(Values.size())];
    Value *FrozenX = B.CreateFreeze(X, "obfcon.rel.x");
    Value *FrozenY = nullptr;
    if (Values.size() > 1) {
      unsigned YIndex = RNG.range(Values.size());
      if (Values[YIndex] == X)
        YIndex = (YIndex + 1) % Values.size();
      FrozenY = B.CreateFreeze(Values[YIndex], "obfcon.rel.y");
    } else {
      FrozenY = constant(Ty, chooseSplitValue(Ty, RNG, false));
    }

    YMBA::Relation Rel =
        YMBA::emitSafeRelation(B, Ty, FrozenX, FrozenY, Site.Seed);
    if (!Rel.L || !Rel.R)
      return ConstantFallback();

    if (C.isZero())
      return RNG.range(2) ? B.CreateXor(Rel.L, Rel.R, "obfcon.zero")
                          : B.CreateSub(Rel.L, Rel.R, "obfcon.zero");

    if (RNG.range(2)) {
      Value *L = B.CreateAdd(B.CreateShl(Rel.L, ConstantInt::get(Ty, 1)),
                             ConstantInt::get(Ty, 1), "obfcon.one.l");
      Value *R = B.CreateAdd(B.CreateShl(Rel.R, ConstantInt::get(Ty, 1)),
                             ConstantInt::get(Ty, 1), "obfcon.one.r");
      return B.CreateUDiv(L, R, "obfcon.one");
    }

    Value *NegOne = B.CreateXor(Rel.L, B.CreateNot(Rel.R), "obfcon.negone");
    return B.CreateSub(ConstantInt::get(Ty, 0), NegOne, "obfcon.one");
  }

  void rewriteZeroAndOne() {
    SmallVector<OperandSite, 64> Sites = collectSites(true);
    DenseMap<Function *, std::unique_ptr<DominatorTree>> DTs;
    for (const OperandSite &Site : Sites) {
      Instruction *Owner = Site.Owner;
      if (!Owner->getParent() || Site.OperandNo >= Owner->getNumOperands())
        continue;
      auto *C = dyn_cast<ConstantInt>(Owner->getOperand(Site.OperandNo));
      if (!C || (!C->isZero() && !C->isOne()))
        continue;
      Instruction *Before = insertionPoint(Site);
      if (!Before)
        continue;
      Function *F = Owner->getFunction();
      auto &DT = DTs[F];
      if (!DT)
        DT = std::make_unique<DominatorTree>(*F);
      Value *Replacement = materializeZeroOrOne(Site, *C, Before, *DT);
      Owner->setOperand(Site.OperandNo, Replacement);
      Modified = true;
    }
  }

  static bool isPointerDerivation(const User &U) {
    auto *Op = dyn_cast<Operator>(&U);
    if (!Op || !U.getType()->isPointerTy())
      return false;
    return Op->getOpcode() == Instruction::GetElementPtr ||
           Op->getOpcode() == Instruction::BitCast ||
           Op->getOpcode() == Instruction::AddrSpaceCast;
  }

  static bool analyzeArrayUsers(Value &Pointer, ArrayUsePlan &Plan,
                                SmallPtrSetImpl<Value *> &Visited) {
    if (!Visited.insert(&Pointer).second)
      return true;
    for (Use &U : Pointer.uses()) {
      User *Usr = U.getUser();
      if (isPointerDerivation(*Usr)) {
        Value *Derived = cast<Value>(Usr);
        if (auto *I = dyn_cast<Instruction>(Derived))
          Plan.PointerInstructions.push_back(I);
        if (!analyzeArrayUsers(*Derived, Plan, Visited))
          return false;
        continue;
      }
      if (auto *LI = dyn_cast<LoadInst>(Usr)) {
        if (LI->getPointerOperand() != &Pointer || LI->isVolatile() ||
            LI->isAtomic() || LI->getType() != Plan.ElementType)
          return false;
        Plan.Loads.push_back({LI, &Pointer});
        continue;
      }
      if (auto *Memcpy = dyn_cast<MemCpyInst>(Usr)) {
        if (&U != &Memcpy->getRawSourceUse() ||
            Memcpy->getRawSource() != &Pointer ||
            Memcpy->getNumOperandBundles() != 0 || Memcpy->isVolatile() ||
            functionUsesFuncletEH(*Memcpy->getFunction()))
          return false;
        Plan.Memcpys.push_back({Memcpy, &Pointer});
        continue;
      }
      return false;
    }
    return true;
  }

  bool analyzeArray(GlobalVariable &GV, ArrayUsePlan &Plan) const {
    if (!GV.hasLocalLinkage() || !GV.hasInitializer() || GV.isThreadLocal() ||
        GV.isExternallyInitialized() || GV.hasSection() || GV.hasComdat())
      return false;

    auto *AT = dyn_cast<ArrayType>(GV.getValueType());
    auto *ElemTy = AT ? dyn_cast<IntegerType>(AT->getElementType()) : nullptr;
    if (!AT || !ElemTy || !isSupportedWidth(ElemTy))
      return false;
    uint64_t ByteWidth = ElemTy->getBitWidth() / 8;
    uint64_t LogicalBytes = 0;
    if (AT->getNumElements() == 0 ||
        AT->getNumElements() > UINT64_MAX / ByteWidth)
      return false;
    LogicalBytes = AT->getNumElements() * ByteWidth;
    uint64_t PhysicalBytes = LogicalBytes;
    if (ElemTy->getBitWidth() == 8) {
      if (LogicalBytes > UINT64_MAX - 3)
        return false;
      PhysicalBytes = (LogicalBytes + 3) & ~uint64_t(3);
    }
    unsigned IndexBits = indexType(GV)->getBitWidth();
    if (IndexBits <= 64) {
      uint64_t MaxSignedOffset = IndexBits == 64
                                     ? uint64_t(INT64_MAX)
                                     : (uint64_t(1) << (IndexBits - 1)) - 1;
      if (PhysicalBytes > MaxSignedOffset)
        return false;
    }

    GlobalStatus Status;
    if (GlobalStatus::analyzeGlobal(&GV, Status) || Status.IsCompared ||
        Status.StoredType != GlobalStatus::NotStored ||
        Status.Ordering != AtomicOrdering::NotAtomic)
      return false;

    Plan.Global = &GV;
    Plan.Type = AT;
    Plan.ElementType = ElemTy;
    SmallPtrSet<Value *, 32> Visited;
    if (!analyzeArrayUsers(GV, Plan, Visited))
      return false;
    return !Plan.Loads.empty() || !Plan.Memcpys.empty();
  }

  bool initializerBytes(const ArrayUsePlan &Plan,
                        SmallVectorImpl<uint8_t> &Bytes) const {
    const DataLayout &DL = M.getDataLayout();
    unsigned BW = Plan.ElementType->getBitWidth();
    unsigned ByteWidth = BW / 8;
    Constant *Init = Plan.Global->getInitializer();
    for (uint64_t I = 0; I != Plan.Type->getNumElements(); ++I) {
      Constant *Element = Init->getAggregateElement(I);
      if (!Element && isa<ConstantAggregateZero>(Init))
        Element = ConstantInt::get(Plan.ElementType, 0);
      auto *CI = dyn_cast_or_null<ConstantInt>(Element);
      if (!CI)
        return false;
      const APInt &V = CI->getValue();
      for (unsigned Byte = 0; Byte != ByteWidth; ++Byte) {
        unsigned Shift =
            (DL.isLittleEndian() ? Byte : ByteWidth - Byte - 1) * 8;
        Bytes.push_back(
            static_cast<uint8_t>(V.extractBitsAsZExtValue(8, Shift)));
      }
    }
    return true;
  }

  static uint8_t combineShare(uint8_t Original, uint8_t A,
                              ArrayCombineKind Kind) {
    switch (Kind) {
    case ArrayCombineKind::Add:
      return static_cast<uint8_t>(Original - A);
    case ArrayCombineKind::Xor:
      return Original ^ A;
    case ArrayCombineKind::Sub:
      return static_cast<uint8_t>(A - Original);
    }
    llvm_unreachable("unknown array share kind");
  }

  EncodedArray encodeArray(const ArrayUsePlan &Plan, ArrayRef<uint8_t> Original,
                           uint64_t Seed) {
    YansoRNG RNG(Seed);
    EncodedArray Enc;
    Enc.LogicalBytes = Original.size();
    Enc.PackedI32 = Plan.ElementType->getBitWidth() == 8;
    Enc.PhysicalSlots = Enc.PackedI32 ? alignTo(Enc.LogicalBytes, uint64_t(4))
                                      : Enc.LogicalBytes;
    Enc.Combine = static_cast<ArrayCombineKind>(RNG.range(3));
    if (Enc.PhysicalSlots > 1) {
      Enc.AffineB = RNG.next64() % Enc.PhysicalSlots;
      unsigned IndexBits = indexType(*Plan.Global)->getBitWidth();
      APInt MaxIndex = IndexBits >= 128 ? APInt::getAllOnes(128)
                                        : APInt::getLowBitsSet(128, IndexBits);
      bool Found = false;
      for (unsigned Attempt = 0; Attempt != 64; ++Attempt) {
        uint64_t Candidate = 1 + RNG.next64() % (Enc.PhysicalSlots - 1);
        APInt MaxMapped(128, Candidate);
        MaxMapped *= APInt(128, Enc.PhysicalSlots - 1);
        MaxMapped += APInt(128, Enc.AffineB);
        if (std::gcd(Candidate, Enc.PhysicalSlots) == 1 &&
            MaxMapped.ule(MaxIndex)) {
          Enc.AffineA = Candidate;
          Found = true;
          break;
        }
      }
      if (!Found) {
        Enc.AffineA = 1;
        Enc.AffineB = 0;
      }
    }

    SmallVector<uint8_t, 64> ABytes(Enc.PhysicalSlots);
    SmallVector<uint8_t, 64> BBytes(Enc.PhysicalSlots);
    for (uint64_t I = 0; I != Enc.PhysicalSlots; ++I) {
      ABytes[I] = static_cast<uint8_t>(RNG.next32());
      BBytes[I] = static_cast<uint8_t>(RNG.next32());
    }
    for (uint64_t I = 0; I != Enc.LogicalBytes; ++I) {
      APInt Mapped(128, Enc.AffineA);
      Mapped *= APInt(128, I);
      Mapped += APInt(128, Enc.AffineB);
      uint64_t P = Mapped.urem(APInt(128, Enc.PhysicalSlots)).getZExtValue();
      uint8_t A = static_cast<uint8_t>(RNG.next32());
      ABytes[P] = A;
      BBytes[P] = combineShare(Original[I], A, Enc.Combine);
    }

    Constant *AInit = nullptr;
    Constant *BInit = nullptr;
    LLVMContext &Ctx = M.getContext();
    if (Enc.PackedI32) {
      SmallVector<uint32_t, 32> AWords;
      SmallVector<uint32_t, 32> BWords;
      for (uint64_t Word = 0; Word != Enc.PhysicalSlots / 4; ++Word) {
        uint32_t AV = 0, BV = 0;
        for (unsigned Byte = 0; Byte != 4; ++Byte) {
          unsigned Shift =
              (M.getDataLayout().isLittleEndian() ? Byte : 3 - Byte) * 8;
          AV |= uint32_t(ABytes[Word * 4 + Byte]) << Shift;
          BV |= uint32_t(BBytes[Word * 4 + Byte]) << Shift;
        }
        AWords.push_back(AV);
        BWords.push_back(BV);
      }
      AInit = ConstantDataArray::get(Ctx, AWords);
      BInit = ConstantDataArray::get(Ctx, BWords);
    } else {
      AInit = ConstantDataArray::get(Ctx, ABytes);
      BInit = ConstantDataArray::get(Ctx, BBytes);
    }

    GlobalVariable &GV = *Plan.Global;
    Enc.ShareA = new GlobalVariable(
        M, AInit->getType(), true, GlobalValue::PrivateLinkage, AInit,
        GV.getName() + ".obf.a", nullptr, GlobalVariable::NotThreadLocal,
        GV.getAddressSpace());
    Enc.ShareB = new GlobalVariable(
        M, BInit->getType(), true, GlobalValue::PrivateLinkage, BInit,
        GV.getName() + ".obf.b", nullptr, GlobalVariable::NotThreadLocal,
        GV.getAddressSpace());
    Enc.ShareA->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    Enc.ShareB->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return Enc;
  }

  Value *emitPointerOffset(IRBuilder<> &B, Value *Pointer,
                           GlobalVariable &GV) const {
    IntegerType *IndexTy = indexType(GV);
    if (Pointer == &GV)
      return ConstantInt::get(IndexTy, 0);
    auto *Op = dyn_cast<Operator>(Pointer);
    if (!Op)
      return nullptr;
    if (Op->getOpcode() == Instruction::BitCast ||
        Op->getOpcode() == Instruction::AddrSpaceCast)
      return emitPointerOffset(B, Op->getOperand(0), GV);
    auto *GEP = dyn_cast<GEPOperator>(Op);
    if (!GEP)
      return nullptr;
    Value *Base = emitPointerOffset(B, GEP->getPointerOperand(), GV);
    if (!Base)
      return nullptr;
    Value *Local = emitGEPOffset(&B, M.getDataLayout(), cast<User>(GEP),
                                 /*NoAssumptions=*/true);
    Local = B.CreateSExtOrTrunc(Local, IndexTy, "obfcon.arr.gep.offset");
    return B.CreateAdd(Base, Local, "obfcon.arr.offset");
  }

  Value *emitDecodedByte(IRBuilder<> &B, const EncodedArray &Enc,
                         Value *LogicalIndex) const {
    auto *IndexTy = cast<IntegerType>(LogicalIndex->getType());
    Value *Mapped = B.CreateAdd(
        B.CreateMul(LogicalIndex, ConstantInt::get(IndexTy, Enc.AffineA)),
        ConstantInt::get(IndexTy, Enc.AffineB), "obfcon.arr.map");
    if (Enc.PhysicalSlots > 1)
      Mapped =
          B.CreateURem(Mapped, ConstantInt::get(IndexTy, Enc.PhysicalSlots),
                       "obfcon.arr.slot");

    auto LoadByte = [&](GlobalVariable *Share, StringRef Name) -> Value * {
      auto *AT = cast<ArrayType>(Share->getValueType());
      Value *Zero = ConstantInt::get(IndexTy, 0);
      if (!Enc.PackedI32) {
        Value *Ptr = B.CreateInBoundsGEP(AT, Share, {Zero, Mapped},
                                         Twine(Name) + ".ptr");
        LoadInst *L = B.CreateLoad(Type::getInt8Ty(M.getContext()), Ptr, Name);
        L->setAlignment(Align(1));
        return L;
      }
      Value *Word =
          B.CreateUDiv(Mapped, ConstantInt::get(IndexTy, 4), "obfcon.arr.word");
      Value *Lane =
          B.CreateURem(Mapped, ConstantInt::get(IndexTy, 4), "obfcon.arr.lane");
      Value *Ptr =
          B.CreateInBoundsGEP(AT, Share, {Zero, Word}, Twine(Name) + ".ptr");
      LoadInst *L = B.CreateLoad(Type::getInt32Ty(M.getContext()), Ptr, Name);
      Value *ShiftLane = M.getDataLayout().isLittleEndian()
                             ? Lane
                             : B.CreateSub(ConstantInt::get(IndexTy, 3), Lane);
      Value *Shift = B.CreateMul(ShiftLane, ConstantInt::get(IndexTy, 8));
      Shift = B.CreateZExtOrTrunc(Shift, Type::getInt32Ty(M.getContext()));
      return B.CreateTrunc(B.CreateLShr(L, Shift),
                           Type::getInt8Ty(M.getContext()),
                           Twine(Name) + ".byte");
    };

    Value *A = LoadByte(Enc.ShareA, "obfcon.arr.load.a");
    Value *C = LoadByte(Enc.ShareB, "obfcon.arr.load.b");
    switch (Enc.Combine) {
    case ArrayCombineKind::Add:
      return B.CreateAdd(A, C, "obfcon.arr.byte");
    case ArrayCombineKind::Xor:
      return B.CreateXor(A, C, "obfcon.arr.byte");
    case ArrayCombineKind::Sub:
      return B.CreateSub(A, C, "obfcon.arr.byte");
    }
    llvm_unreachable("unknown array combine kind");
  }

  Value *emitDecodedElement(IRBuilder<> &B, const ArrayUsePlan &Plan,
                            const EncodedArray &Enc, Value *ByteOffset) const {
    unsigned ByteWidth = Plan.ElementType->getBitWidth() / 8;
    Value *Result = ConstantInt::get(Plan.ElementType, 0);
    auto *OffsetTy = cast<IntegerType>(ByteOffset->getType());
    for (unsigned Byte = 0; Byte != ByteWidth; ++Byte) {
      Value *Logical =
          Byte ? B.CreateAdd(ByteOffset, ConstantInt::get(OffsetTy, Byte))
               : ByteOffset;
      Value *Part =
          B.CreateZExt(emitDecodedByte(B, Enc, Logical), Plan.ElementType);
      unsigned Shift =
          (M.getDataLayout().isLittleEndian() ? Byte : ByteWidth - Byte - 1) *
          8;
      if (Shift)
        Part = B.CreateShl(Part, ConstantInt::get(Plan.ElementType, Shift));
      Result = B.CreateOr(Result, Part, "obfcon.arr.element");
    }
    return Result;
  }

  Function *createDecodeCopyHelper(const ArrayUsePlan &Plan,
                                   const EncodedArray &Enc, MemCpyInst &Memcpy,
                                   unsigned Number) {
    auto *DestTy = cast<PointerType>(Memcpy.getRawDest()->getType());
    auto *LenTy = cast<IntegerType>(Memcpy.getLength()->getType());
    IntegerType *IndexTy = indexType(*Plan.Global);
    FunctionType *FT = FunctionType::get(Type::getVoidTy(M.getContext()),
                                         {DestTy, IndexTy, LenTy}, false);
    Function *F = Function::Create(
        FT, GlobalValue::InternalLinkage,
        Plan.Global->getName() + ".obf.copy." + Twine(Number), M);
    auto It = F->arg_begin();
    Value *Dest = &*It++;
    Value *Offset = &*It++;
    Value *Length = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    BasicBlock *Loop = BasicBlock::Create(M.getContext(), "loop", F);
    BasicBlock *Body = BasicBlock::Create(M.getContext(), "body", F);
    BasicBlock *Done = BasicBlock::Create(M.getContext(), "done", F);
    IRBuilder<> B(Entry);
    B.CreateBr(Loop);
    B.SetInsertPoint(Loop);
    PHINode *I = B.CreatePHI(LenTy, 2, "obfcon.arr.i");
    I->addIncoming(ConstantInt::get(LenTy, 0), Entry);
    B.CreateCondBr(B.CreateICmpULT(I, Length), Body, Done);
    B.SetInsertPoint(Body);
    Value *Index = B.CreateZExtOrTrunc(I, IndexTy);
    Value *Logical = B.CreateAdd(Offset, Index, "obfcon.arr.copy.index");
    Value *Byte = emitDecodedByte(B, Enc, Logical);
    Value *DestIndex = B.CreateGEP(Type::getInt8Ty(M.getContext()), Dest, I);
    StoreInst *Store = B.CreateStore(Byte, DestIndex);
    Store->setAlignment(Align(1));
    Value *Next = B.CreateAdd(I, ConstantInt::get(LenTy, 1));
    B.CreateBr(Loop);
    I->addIncoming(Next, Body);
    B.SetInsertPoint(Done);
    B.CreateRetVoid();
    F->addFnAttr(Attribute::NoUnwind);
    return F;
  }

  void cleanupArrayPointers(ArrayUsePlan &Plan) {
    bool Progress = true;
    while (Progress) {
      Progress = false;
      for (WeakTrackingVH &VH : reverse(Plan.PointerInstructions)) {
        auto *I = dyn_cast_or_null<Instruction>(VH);
        if (I && I->getParent() && I->use_empty()) {
          I->eraseFromParent();
          Progress = true;
        }
      }
    }
    Plan.Global->removeDeadConstantUsers();
  }

  bool rewriteEncodedArray(ArrayUsePlan &Plan, uint64_t Seed) {
    SmallVector<uint8_t, 64> Original;
    if (!initializerBytes(Plan, Original))
      return false;
    EncodedArray Enc = encodeArray(Plan, Original, Seed);

    for (ArrayLoadUse &Use : Plan.Loads) {
      IRBuilder<> B(Use.Load);
      Value *Offset = emitPointerOffset(B, Use.Pointer, *Plan.Global);
      if (!Offset)
        report_fatal_error("validated ObfCon array load lost its offset");
      Value *Decoded = emitDecodedElement(B, Plan, Enc, Offset);
      Use.Load->replaceAllUsesWith(Decoded);
      Use.Load->eraseFromParent();
    }

    unsigned HelperNumber = 0;
    for (ArrayMemcpyUse &Use : Plan.Memcpys) {
      MemCpyInst *Memcpy = Use.Memcpy;
      IRBuilder<> B(Memcpy);
      Value *Offset = emitPointerOffset(B, Use.Source, *Plan.Global);
      if (!Offset)
        report_fatal_error("validated ObfCon memcpy lost its source offset");
      Function *Helper =
          createDecodeCopyHelper(Plan, Enc, *Memcpy, ++HelperNumber);
      B.CreateCall(Helper, {Memcpy->getRawDest(), Offset, Memcpy->getLength()});
      Memcpy->eraseFromParent();
    }

    cleanupArrayPointers(Plan);
    if (!Plan.Global->use_empty())
      report_fatal_error("ObfCon array rewrite left uses of the original");
    Plan.Global->eraseFromParent();
    return true;
  }

  void encodeLocalArrays() {
    SmallVector<GlobalVariable *, 16> Globals;
    for (GlobalVariable &GV : M.globals())
      Globals.push_back(&GV);
    unsigned Index = 0;
    for (GlobalVariable *GV : Globals) {
      ArrayUsePlan Plan;
      if (!analyzeArray(*GV, Plan))
        continue;
      if (rewriteEncodedArray(Plan, yanso_mix64(++Index, ModuleSeed)))
        Modified = true;
    }
  }

public:
  explicit ObfConImpl(Module &M)
      : M(M), ModuleSeed(yanso_module_seed(M, "obfcon")) {}

  bool run() {
    encodeLocalArrays();
    rewriteOrdinaryConstants();
    rewriteZeroAndOne();
    return Modified;
  }
};

} // namespace

PreservedAnalyses ObfConPass::run(Module &M, ModuleAnalysisManager &) {
  ObfConImpl Impl(M);
  return Impl.run() ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
