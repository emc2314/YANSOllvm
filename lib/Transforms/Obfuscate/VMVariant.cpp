#include "VMVariant.h"
#include "CryptoUtils.h"
#include "YMBA.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace {

VMVariantEmitter::RelationVariant
selectRelationVariant(YansoChoiceStream &Choices) {
  VMVariantEmitter::RelationVariant V;
  V.Application =
      static_cast<VMVariantEmitter::RelationApplication>(Choices.range(4));
  V.Projector = static_cast<VMVariantEmitter::ProjectorKind>(Choices.range(3));
  return V;
}

VMVariantEmitter::BinaryMutation
selectDataMuxMutation(YansoChoiceStream &Choices) {
  switch (Choices.range(3)) {
  case 0:
    return VMVariantEmitter::BinaryMutation::DataMuxAddSub;
  case 1:
    return VMVariantEmitter::BinaryMutation::DataMuxOrAbsorb;
  default:
    return VMVariantEmitter::BinaryMutation::DataMuxXorRoundTrip;
  }
}

template <typename EnumT>
EnumT selectDifferent(YansoChoiceStream &Choices, EnumT Current,
                      unsigned Count) {
  assert(Count >= 2 && "selectDifferent needs at least two choices");
  unsigned Index = Choices.range(Count - 1);
  unsigned CurrentIndex = static_cast<unsigned>(Current);
  if (Index >= CurrentIndex)
    ++Index;
  return static_cast<EnumT>(Index);
}

} // namespace

//===----------------------------------------------------------------------===//
// Common helpers and capability gates
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::loConst(IntegerType *Ty, uint64_t V) {
  return ConstantInt::get(Ty, V);
}

static Value *modInvConst(IntegerType *Ty, uint64_t V) {
  return ConstantInt::get(
      Ty, yanso_mod_inverse(APInt(Ty->getBitWidth(), V, false, true)));
}

bool VMVariantEmitter::supportsLoopMutation(unsigned Opcode,
                                            unsigned BitWidth) {
  return BitWidth >= 8 && BitWidth <= 64 &&
         YMBA::isSupportedBinaryOpcode(Opcode);
}

bool VMVariantEmitter::supportsDataMuxMutation(unsigned Opcode,
                                               unsigned BitWidth) {
  switch (Opcode) {
  case BinaryOperator::Add:
  case BinaryOperator::Sub:
  case BinaryOperator::And:
  case BinaryOperator::Or:
  case BinaryOperator::Xor:
  case BinaryOperator::Mul:
  case BinaryOperator::UDiv:
  case BinaryOperator::SDiv:
  case BinaryOperator::URem:
  case BinaryOperator::SRem:
    return BitWidth <= 128;
  default:
    return false;
  }
}

bool VMVariantEmitter::supportsRelation(unsigned BitWidth) {
  return BitWidth == 8 || BitWidth == 16 || BitWidth == 32 || BitWidth == 64 ||
         BitWidth == 128;
}

bool VMVariantEmitter::supportsPredicateMutation(Type *Ty) {
  return Ty->isIntegerTy() || Ty->isPointerTy();
}

bool VMVariantEmitter::supportsSelectMutation(Type *Ty) {
  return Ty->isIntegerTy() || Ty->isPointerTy();
}

//===----------------------------------------------------------------------===//
// Variant selection policy
//===----------------------------------------------------------------------===//

VMVariantEmitter::BinaryVariant
VMVariantEmitter::selectBinaryVariant(unsigned Opcode, IntegerType *Ty,
                                      uint64_t Seed, unsigned MutationPermille,
                                      unsigned RelationAppPermille) {
  BinaryVariant V;
  YansoChoiceStream Choices(Seed, "vm.binary.variant");
  unsigned BitWidth = Ty->getBitWidth();

  if (supportsRelation(BitWidth) && Choices.chance(RelationAppPermille))
    V.Relation = selectRelationVariant(Choices);

  bool SupportsLoop = supportsLoopMutation(Opcode, BitWidth);
  bool SupportsMux = supportsDataMuxMutation(Opcode, BitWidth);
  if ((SupportsLoop || SupportsMux) && Choices.chance(MutationPermille)) {
    if (SupportsLoop && SupportsMux) {
      V.Mutation = Choices.range(2) == 0 ? BinaryMutation::BitRebuild
                                         : selectDataMuxMutation(Choices);
    } else
      V.Mutation = SupportsLoop ? BinaryMutation::BitRebuild
                                : selectDataMuxMutation(Choices);
  }
  return V;
}

VMVariantEmitter::PredicateVariant
VMVariantEmitter::selectPredicateVariant(Type *Ty, uint64_t Seed,
                                         unsigned MutationPermille) {
  PredicateVariant V;
  YansoChoiceStream Choices(Seed, "vm.predicate.variant");
  V.Primary = static_cast<PredicateExpr>(Choices.range(3));
  if (!supportsPredicateMutation(Ty))
    return V;
  if (Choices.chance(MutationPermille))
    V.Alternate = selectDifferent(Choices, V.Primary, 3);
  return V;
}

VMVariantEmitter::SelectVariant
VMVariantEmitter::selectSelectVariant(Type *Ty, uint64_t Seed,
                                      unsigned MutationPermille) {
  SelectVariant V;
  YansoChoiceStream Choices(Seed, "vm.select.variant");
  unsigned ExprCount = Ty->isIntegerTy() ? 3 : 2;
  V.Primary = static_cast<SelectExpr>(Choices.range(ExprCount));
  if (!supportsSelectMutation(Ty))
    return V;
  if (Choices.chance(MutationPermille))
    V.Alternate = selectDifferent(Choices, V.Primary, ExprCount);
  return V;
}

VMVariantEmitter::IntrinsicVariant
VMVariantEmitter::selectIntrinsicVariant(IntegerType *Ty, uint64_t Seed) {
  IntrinsicVariant V;
  YansoChoiceStream Choices(Seed, "vm.intrinsic.variant");
  if (supportsRelation(Ty->getBitWidth()) && Choices.range(2) != 0)
    V.Relation = selectRelationVariant(Choices);
  return V;
}

VMVariantEmitter::CastVariant
VMVariantEmitter::selectCastVariant(Type *SrcTy, Type *DstTy, uint64_t Seed) {
  CastVariant V;
  YansoChoiceStream Choices(Seed, "vm.cast.variant");
  if (SrcTy->isIntegerTy())
    V.InputDecoration = static_cast<IntegerDecoration>(Choices.range(4));
  if (auto *ITy = dyn_cast<IntegerType>(DstTy)) {
    V.OutputDecoration = static_cast<IntegerDecoration>(Choices.range(4));
    if (supportsRelation(ITy->getBitWidth()) && Choices.range(2) != 0)
      V.Relation = selectRelationVariant(Choices);
  }
  return V;
}

//===----------------------------------------------------------------------===//
// Predicate / ICmp expression templates and wrappers
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitICmpExpr(IRBuilder<> &B, CmpInst::Predicate Pred,
                                      Type *Ty, Value *X, Value *Y,
                                      PredicateExpr Expr, uint64_t Seed) {
  IntegerType *ITy = nullptr;
  Value *A = X;
  Value *C = Y;
  if (auto *PtrTy = dyn_cast<PointerType>(Ty)) {
    const DataLayout &DL = B.GetInsertBlock()->getModule()->getDataLayout();
    if (DL.isNonIntegralPointerType(PtrTy)) {
      Value *Base = B.CreateICmp(Pred, X, Y);
      switch (Expr) {
      case PredicateExpr::Direct:
        return Base;
      case PredicateExpr::DoubleNot:
        return B.CreateNot(B.CreateNot(Base), "vm.pred.notnot");
      case PredicateExpr::XorRoundTrip: {
        YansoChoiceStream Choices(Seed, "vm.predicate.emit");
        Value *K =
            ConstantInt::get(Type::getInt1Ty(B.getContext()), Choices.range(2));
        return B.CreateXor(B.CreateXor(Base, K), K, "vm.pred.xorxor");
      }
      }
    }
    ITy = IntegerType::get(B.getContext(),
                           DL.getPointerSizeInBits(PtrTy->getAddressSpace()));
    A = B.CreatePtrToInt(X, ITy, "vm.pred.ptr.x");
    C = B.CreatePtrToInt(Y, ITy, "vm.pred.ptr.y");
  } else {
    ITy = dyn_cast<IntegerType>(Ty);
  }
  if (!ITy)
    return B.CreateICmp(Pred, X, Y);

  unsigned BW = ITy->getBitWidth();
  Value *D = B.CreateXor(A, C, "vm.pred.diff");
  Value *DNeg = B.CreateSub(loConst(ITy, 0), D, "vm.pred.diff.neg");
  Value *NZWide = B.CreateLShr(B.CreateOr(D, DNeg), loConst(ITy, BW - 1),
                               "vm.pred.nz.wide");
  Value *EqWide = B.CreateXor(NZWide, loConst(ITy, 1), "vm.pred.eq.wide");
  Value *Eq =
      B.CreateTrunc(EqWide, Type::getInt1Ty(B.getContext()), "vm.pred.eq");
  Value *Ne =
      B.CreateTrunc(NZWide, Type::getInt1Ty(B.getContext()), "vm.pred.ne");

  Value *Base = nullptr;
  switch (Pred) {
  case CmpInst::ICMP_EQ:
    Base = Eq;
    break;
  case CmpInst::ICMP_NE:
    Base = Ne;
    break;
  case CmpInst::ICMP_ULT:
  case CmpInst::ICMP_UGE:
  case CmpInst::ICMP_SLT:
  case CmpInst::ICMP_SGE: {
    bool Signed = Pred == CmpInst::ICMP_SLT || Pred == CmpInst::ICMP_SGE;
    Value *LtWide = nullptr;
    if (Signed) {
      Value *SX = B.CreateLShr(A, loConst(ITy, BW - 1), "vm.pred.sx");
      Value *SY = B.CreateLShr(C, loConst(ITy, BW - 1), "vm.pred.sy");
      Value *SignDiff = B.CreateXor(SX, SY, "vm.pred.sdiff");
      Value *Diff = B.CreateSub(A, C, "vm.pred.ssub");
      Value *SubSign =
          B.CreateLShr(Diff, loConst(ITy, BW - 1), "vm.pred.ssub.sign");
      LtWide = B.CreateOr(
          B.CreateAnd(SignDiff, SX),
          B.CreateAnd(B.CreateXor(SignDiff, loConst(ITy, 1)), SubSign),
          "vm.pred.slt.wide");
    } else {
      Value *NX = B.CreateNot(A);
      Value *T0 = B.CreateAnd(NX, C);
      Value *T1 = B.CreateOr(NX, C);
      Value *T2 = B.CreateAnd(T1, B.CreateSub(A, C, "vm.pred.usub"));
      Value *Borrow = B.CreateOr(T0, T2, "vm.pred.borrow");
      LtWide = B.CreateLShr(Borrow, loConst(ITy, BW - 1), "vm.pred.ult.wide");
    }
    Value *Lt = B.CreateTrunc(LtWide, Type::getInt1Ty(B.getContext()),
                              Signed ? "vm.pred.slt" : "vm.pred.ult");
    Base = (Pred == CmpInst::ICMP_UGE || Pred == CmpInst::ICMP_SGE)
               ? B.CreateNot(Lt, Signed ? "vm.pred.sge" : "vm.pred.uge")
               : Lt;
    break;
  }
  case CmpInst::ICMP_ULE:
  case CmpInst::ICMP_UGT:
  case CmpInst::ICMP_SLE:
  case CmpInst::ICMP_SGT: {
    CmpInst::Predicate Swapped = CmpInst::ICMP_ULT;
    if (Pred == CmpInst::ICMP_SLE || Pred == CmpInst::ICMP_SGT)
      Swapped = CmpInst::ICMP_SLT;
    Value *LtYX =
        emitICmpExpr(B, Swapped, ITy, C, A, PredicateExpr::Direct, Seed);
    Base = (Pred == CmpInst::ICMP_ULE || Pred == CmpInst::ICMP_SLE)
               ? B.CreateNot(LtYX, "vm.pred.le")
               : LtYX;
    break;
  }
  default:
    Base = B.CreateICmp(Pred, X, Y);
    break;
  }

  switch (Expr) {
  case PredicateExpr::Direct:
    return Base;
  case PredicateExpr::DoubleNot:
    return B.CreateNot(B.CreateNot(Base), "vm.pred.notnot");
  case PredicateExpr::XorRoundTrip: {
    YansoChoiceStream Choices(Seed, "vm.predicate.emit");
    Value *K =
        ConstantInt::get(Type::getInt1Ty(B.getContext()), Choices.range(2));
    return B.CreateXor(B.CreateXor(Base, K), K, "vm.pred.xorxor");
  }
  }
  llvm_unreachable("unknown VM predicate expression");
}

//===----------------------------------------------------------------------===//
// Select expression templates and wrappers
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitSelectExpr(IRBuilder<> &B, Type *Ty, Value *Cond,
                                        Value *TrueV, Value *FalseV,
                                        SelectExpr Expr, uint64_t Seed) {
  if (auto *ITy = dyn_cast<IntegerType>(Ty)) {
    switch (Expr) {
    case SelectExpr::Direct:
      return B.CreateSelect(Cond, TrueV, FalseV);
    case SelectExpr::Inverted: {
      Value *InvCond = B.CreateNot(Cond);
      return B.CreateSelect(InvCond, FalseV, TrueV, "vm.select.invert");
    }
    case SelectExpr::XorRoundTrip: {
      Value *Selected = B.CreateSelect(Cond, TrueV, FalseV, "vm.select.value");
      Value *K = loConst(ITy, yanso_mix64(Seed, 0x6a09e667f3bcc909ULL));
      return B.CreateXor(B.CreateXor(Selected, K), K, "vm.select.xorxor");
    }
    }
  }
  switch (Expr) {
  case SelectExpr::Direct:
    return B.CreateSelect(Cond, TrueV, FalseV);
  case SelectExpr::Inverted:
    return B.CreateSelect(B.CreateNot(Cond), FalseV, TrueV, "vm.select.invert");
  case SelectExpr::XorRoundTrip:
    llvm_unreachable("xor select expression requires an integer type");
  }
  llvm_unreachable("unknown VM select expression");
}

//===----------------------------------------------------------------------===//
// Relation providers, projectors, and relation applications
//===----------------------------------------------------------------------===//

VMVariantEmitter::Relation VMVariantEmitter::emitRelation(IRBuilder<> &B,
                                                          IntegerType *Ty,
                                                          Value *X, Value *Y,
                                                          uint64_t Seed) {
  Value *FrozenX = B.CreateFreeze(X, "vm.rel.x");
  Value *FrozenY = X == Y ? FrozenX : B.CreateFreeze(Y, "vm.rel.y");
  YMBA::Relation Rel = YMBA::emitSafeRelation(B, Ty, FrozenX, FrozenY, Seed);
  if (Rel.L && Rel.R)
    return {Rel.L, Rel.R};
  return {loConst(Ty, 0), loConst(Ty, 0)};
}

Value *VMVariantEmitter::emitProjector(IRBuilder<> &B, IntegerType *Ty,
                                       Value *V, ProjectorKind Kind,
                                       uint64_t Seed, StringRef Name) {
  switch (Kind) {
  case ProjectorKind::LowBit:
    return B.CreateAnd(V, loConst(Ty, 1), Name);
  case ProjectorKind::Parity:
    return B.CreateAnd(B.CreateUnaryIntrinsic(Intrinsic::ctpop, V),
                       loConst(Ty, 1), Name);
  case ProjectorKind::KeyedBit: {
    YansoChoiceStream Choices(Seed, "vm.relation.projector");
    unsigned BW = Ty->getBitWidth();
    unsigned Shift = Choices.range(BW);
    Value *Mixed =
        B.CreateXor(V, loConst(Ty, Choices.next64()), Twine(Name) + ".mix");
    return B.CreateAnd(B.CreateLShr(Mixed, loConst(Ty, Shift)), loConst(Ty, 1),
                       Name);
  }
  }
  llvm_unreachable("unknown VM relation projector");
}

Value *VMVariantEmitter::applyRelationDiffFold(IRBuilder<> &B, IntegerType *Ty,
                                               Value *R, const Relation &Rel) {
  Value *T = B.CreateAdd(R, Rel.L, "vm.id.app.diff.addx");
  return B.CreateSub(T, Rel.R, "vm.id.app.diff.suby");
}

Value *VMVariantEmitter::applyRelationPairedMulBranch(IRBuilder<> &B,
                                                      IntegerType *Ty, Value *R,
                                                      const Relation &Rel,
                                                      ProjectorKind Projector,
                                                      uint64_t Seed) {
  Function *F = B.GetInsertBlock()->getParent();
  LLVMContext &Ctx = F->getContext();
  BasicBlock *Then0 = BasicBlock::Create(Ctx, "vm.id.app.mul0.t", F);
  BasicBlock *Else0 = BasicBlock::Create(Ctx, "vm.id.app.mul0.f", F);
  BasicBlock *Join0 = BasicBlock::Create(Ctx, "vm.id.app.mul0.join", F);
  BasicBlock *Then1 = BasicBlock::Create(Ctx, "vm.id.app.mul1.t", F);
  BasicBlock *Else1 = BasicBlock::Create(Ctx, "vm.id.app.mul1.f", F);
  BasicBlock *Join1 = BasicBlock::Create(Ctx, "vm.id.app.mul1.join", F);

  Value *PX = emitProjector(B, Ty, Rel.L, Projector, Seed, "vm.id.app.predx.v");
  Value *PY = emitProjector(B, Ty, Rel.R, Projector, Seed, "vm.id.app.predy.v");
  Value *Pred0 = B.CreateICmpNE(PX, loConst(Ty, 0), "vm.id.app.predx");
  Value *Pred1 = B.CreateICmpNE(PY, loConst(Ty, 0), "vm.id.app.predy");
  uint64_t A = yanso_mix64(Seed, 0x9e3779b97f4a7c15ULL) | 1ULL;
  uint64_t Bc = yanso_mix64(Seed, 0x243f6a8885a308d3ULL) | 1ULL;
  Value *A0 = loConst(Ty, A);
  Value *B0 = loConst(Ty, Bc);
  Value *AInv = modInvConst(Ty, A);
  Value *BInv = modInvConst(Ty, Bc);

  B.CreateCondBr(Pred0, Then0, Else0);

  IRBuilder<> T0B(Then0);
  Value *T0 = T0B.CreateMul(R, A0, "vm.id.app.mul.a");
  T0B.CreateBr(Join0);

  IRBuilder<> E0B(Else0);
  Value *E0 = E0B.CreateMul(R, B0, "vm.id.app.mul.b");
  E0B.CreateBr(Join0);

  IRBuilder<> J0B(Join0);
  PHINode *Mid = J0B.CreatePHI(Ty, 2, "vm.id.app.mul.mid");
  Mid->addIncoming(T0, Then0);
  Mid->addIncoming(E0, Else0);
  J0B.CreateCondBr(Pred1, Then1, Else1);

  IRBuilder<> T1B(Then1);
  Value *T1 = T1B.CreateMul(Mid, AInv, "vm.id.app.mul.ainv");
  T1B.CreateBr(Join1);

  IRBuilder<> E1B(Else1);
  Value *E1 = E1B.CreateMul(Mid, BInv, "vm.id.app.mul.binv");
  E1B.CreateBr(Join1);

  IRBuilder<> J1B(Join1);
  PHINode *Out = J1B.CreatePHI(Ty, 2, "vm.id.app.mul.result");
  Out->addIncoming(T1, Then1);
  Out->addIncoming(E1, Else1);
  B.SetInsertPoint(Join1);
  return Out;
}

Value *VMVariantEmitter::applyRelationPairedAffineBranch(
    IRBuilder<> &B, IntegerType *Ty, Value *R, const Relation &Rel,
    ProjectorKind Projector, uint64_t Seed) {
  Function *F = B.GetInsertBlock()->getParent();
  LLVMContext &Ctx = F->getContext();
  BasicBlock *Then0 = BasicBlock::Create(Ctx, "vm.id.app.aff0.t", F);
  BasicBlock *Else0 = BasicBlock::Create(Ctx, "vm.id.app.aff0.f", F);
  BasicBlock *Join0 = BasicBlock::Create(Ctx, "vm.id.app.aff0.join", F);
  BasicBlock *Then1 = BasicBlock::Create(Ctx, "vm.id.app.aff1.t", F);
  BasicBlock *Else1 = BasicBlock::Create(Ctx, "vm.id.app.aff1.f", F);
  BasicBlock *Join1 = BasicBlock::Create(Ctx, "vm.id.app.aff1.join", F);

  Value *PX =
      emitProjector(B, Ty, Rel.L, Projector, Seed, "vm.id.app.aff.predx.v");
  Value *PY =
      emitProjector(B, Ty, Rel.R, Projector, Seed, "vm.id.app.aff.predy.v");
  Value *Pred0 = B.CreateICmpNE(PX, loConst(Ty, 0), "vm.id.app.aff.predx");
  Value *Pred1 = B.CreateICmpNE(PY, loConst(Ty, 0), "vm.id.app.aff.predy");

  uint64_t A = yanso_mix64(Seed, 0x452821e638d01377ULL) | 1ULL;
  uint64_t Bc = yanso_mix64(Seed, 0xbe5466cf34e90c6cULL) | 1ULL;
  Value *A0 = loConst(Ty, A);
  Value *B0 = loConst(Ty, Bc);
  Value *AInv = modInvConst(Ty, A);
  Value *BInv = modInvConst(Ty, Bc);
  Value *C0 = loConst(Ty, yanso_mix64(Seed, 0xc0ac29b7c97c50ddULL));
  Value *D0 = loConst(Ty, yanso_mix64(Seed, 0x3f84d5b5b5470917ULL));

  B.CreateCondBr(Pred0, Then0, Else0);

  IRBuilder<> T0B(Then0);
  Value *T0 = T0B.CreateAdd(T0B.CreateMul(R, A0, "vm.id.app.aff.mul.a"), C0,
                            "vm.id.app.aff.add.c");
  T0B.CreateBr(Join0);

  IRBuilder<> E0B(Else0);
  Value *E0 = E0B.CreateAdd(E0B.CreateMul(R, B0, "vm.id.app.aff.mul.b"), D0,
                            "vm.id.app.aff.add.d");
  E0B.CreateBr(Join0);

  IRBuilder<> J0B(Join0);
  PHINode *Mid = J0B.CreatePHI(Ty, 2, "vm.id.app.aff.mid");
  Mid->addIncoming(T0, Then0);
  Mid->addIncoming(E0, Else0);
  J0B.CreateCondBr(Pred1, Then1, Else1);

  IRBuilder<> T1B(Then1);
  Value *T1 = T1B.CreateMul(T1B.CreateSub(Mid, C0, "vm.id.app.aff.sub.c"), AInv,
                            "vm.id.app.aff.mul.ainv");
  T1B.CreateBr(Join1);

  IRBuilder<> E1B(Else1);
  Value *E1 = E1B.CreateMul(E1B.CreateSub(Mid, D0, "vm.id.app.aff.sub.d"), BInv,
                            "vm.id.app.aff.mul.binv");
  E1B.CreateBr(Join1);

  IRBuilder<> J1B(Join1);
  PHINode *Out = J1B.CreatePHI(Ty, 2, "vm.id.app.aff.result");
  Out->addIncoming(T1, Then1);
  Out->addIncoming(E1, Else1);
  B.SetInsertPoint(Join1);
  return Out;
}

Value *VMVariantEmitter::applyRelationOpaqueFork(IRBuilder<> &B,
                                                 IntegerType *Ty, Value *R,
                                                 const Relation &Rel,
                                                 ProjectorKind Projector,
                                                 uint64_t Seed) {
  Function *F = B.GetInsertBlock()->getParent();
  LLVMContext &Ctx = F->getContext();
  BasicBlock *Good = BasicBlock::Create(Ctx, "vm.id.app.fork.good", F);
  BasicBlock *Bad = BasicBlock::Create(Ctx, "vm.id.app.fork.bad", F);
  BasicBlock *Join = BasicBlock::Create(Ctx, "vm.id.app.fork.join", F);

  Value *PX =
      emitProjector(B, Ty, Rel.L, Projector, Seed, "vm.id.app.fork.predx.v");
  Value *PY =
      emitProjector(B, Ty, Rel.R, Projector, Seed, "vm.id.app.fork.predy.v");
  Value *Guard = B.CreateICmpEQ(PX, PY, "vm.id.app.fork.guard");
  B.CreateCondBr(Guard, Good, Bad);

  IRBuilder<> GB(Good);
  Value *Noise = loConst(Ty, yanso_mix64(Seed, 0x6a09e667f3bcc909ULL));
  Value *GoodV = GB.CreateSub(GB.CreateAdd(R, Noise, "vm.id.app.fork.good.add"),
                              Noise, "vm.id.app.fork.good.sub");
  GB.CreateBr(Join);

  IRBuilder<> BB(Bad);
  Value *BadNoise = BB.CreateOr(
      loConst(Ty, yanso_mix64(Seed, 0xbb67ae8584caa73bULL)), loConst(Ty, 1));
  Value *BadV = BB.CreateAdd(R, BadNoise, "vm.id.app.fork.bad");
  BB.CreateBr(Join);

  IRBuilder<> JB(Join);
  PHINode *Out = JB.CreatePHI(Ty, 2, "vm.id.app.fork.result");
  Out->addIncoming(GoodV, Good);
  Out->addIncoming(BadV, Bad);
  B.SetInsertPoint(Join);
  return Out;
}

Value *VMVariantEmitter::applyRelation(
    IRBuilder<> &B, IntegerType *Ty, Value *R, Value *X, Value *Y,
    const std::optional<RelationVariant> &Variant, uint64_t Seed) {
  if (!Variant)
    return R;

  Relation Rel = emitRelation(B, Ty, X, Y, Seed);
  switch (Variant->Application) {
  case RelationApplication::DiffFold:
    return applyRelationDiffFold(B, Ty, R, Rel);
  case RelationApplication::PairedMulBranch:
    return applyRelationPairedMulBranch(B, Ty, R, Rel, Variant->Projector,
                                        Seed);
  case RelationApplication::PairedAffineBranch:
    return applyRelationPairedAffineBranch(B, Ty, R, Rel, Variant->Projector,
                                           Seed);
  case RelationApplication::OpaqueFork:
    return applyRelationOpaqueFork(B, Ty, R, Rel, Variant->Projector, Seed);
  }
  llvm_unreachable("unknown VM relation application");
}

//===----------------------------------------------------------------------===//
// Binary mutation wrappers
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitControlFlowBitRebuild(IRBuilder<> &B, Function *F,
                                                   IntegerType *Ty,
                                                   Value *Input) {
  LLVMContext &Ctx = F->getContext();
  BasicBlock *Entry = B.GetInsertBlock();
  BasicBlock *Loop = BasicBlock::Create(Ctx, "vm.cf.loop", F);
  BasicBlock *Body = BasicBlock::Create(Ctx, "vm.cf.body", F);
  BasicBlock *Done = BasicBlock::Create(Ctx, "vm.cf.done", F);
  unsigned BW = Ty->getBitWidth();

  B.CreateBr(Loop);

  IRBuilder<> LB(Loop);
  PHINode *I = LB.CreatePHI(Ty, 2, "vm.i");
  PHINode *A = LB.CreatePHI(Ty, 2, "vm.a");
  I->addIncoming(loConst(Ty, 0), Entry);
  A->addIncoming(loConst(Ty, 0), Entry);
  Value *Cond = LB.CreateICmpULT(I, loConst(Ty, BW));
  LB.CreateCondBr(Cond, Body, Done);

  IRBuilder<> BB(Body);
  Value *SrcBit = BB.CreateSub(loConst(Ty, BW - 1), I);
  Value *Bit = BB.CreateAnd(BB.CreateLShr(Input, SrcBit), loConst(Ty, 1));
  Value *NextA = BB.CreateOr(BB.CreateShl(A, loConst(Ty, 1)), Bit);
  Value *NextI = BB.CreateAdd(I, loConst(Ty, 1));
  BB.CreateBr(Loop);
  I->addIncoming(NextI, Body);
  A->addIncoming(NextA, Body);

  IRBuilder<> DB(Done);
  B.SetInsertPoint(Done);
  return A;
}

Value *VMVariantEmitter::emitDataMuxBinaryValue(IRBuilder<> &B, unsigned Opcode,
                                                IntegerType *Ty, Value *X,
                                                Value *Y,
                                                const BinaryVariant &Variant,
                                                uint64_t Seed) {
  YansoChoiceStream Choices(Seed, "vm.binary.mux");
  uint64_t AlternateSeed = Choices.next64();
  Value *R0 = YMBA::emitBinary(B, Opcode, Ty, X, Y, Seed);
  R0 = applyRelation(B, Ty, R0, X, Y, Variant.Relation, Seed);
  Value *R1 = YMBA::emitBinary(B, Opcode, Ty, X, Y, AlternateSeed);
  R1 = applyRelation(B, Ty, R1, X, Y, Variant.Relation, AlternateSeed);

  Value *K = loConst(Ty, Choices.next64());
  Value *Pred = nullptr;
  switch (Variant.Mutation) {
  case BinaryMutation::DataMuxAddSub:
    Pred = B.CreateICmpEQ(B.CreateSub(B.CreateAdd(X, K), K), X, "vm.mux.pred");
    break;
  case BinaryMutation::DataMuxOrAbsorb:
    Pred = B.CreateICmpEQ(B.CreateOr(B.CreateAnd(X, Y), X), X, "vm.mux.pred");
    break;
  case BinaryMutation::DataMuxXorRoundTrip:
    Pred = B.CreateICmpEQ(B.CreateXor(B.CreateXor(Y, K), K), Y, "vm.mux.pred");
    break;
  case BinaryMutation::None:
  case BinaryMutation::BitRebuild:
    llvm_unreachable("data mux emitter requires a data mux mutation");
  }

  Value *Mask = B.CreateSub(loConst(Ty, 0), B.CreateZExt(Pred, Ty));
  Value *TruePart = B.CreateAnd(R0, Mask);
  Value *FalsePart = B.CreateAnd(R1, B.CreateNot(Mask));
  return B.CreateOr(TruePart, FalsePart);
}

//===----------------------------------------------------------------------===//
// Intrinsic and cast wrappers
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitIntrinsicValue(IRBuilder<> &B, Intrinsic::ID ID,
                                            IntegerType *Ty,
                                            ArrayRef<Value *> Args,
                                            const IntrinsicVariant &Variant,
                                            uint64_t Seed) {
  Value *R = YMBA::emitIntrinsic(B, ID, Ty, Args, Seed);
  Value *RelX = Args.empty() ? R : Args[0];
  Value *RelY =
      (Args.size() >= 2 && Args[1]->getType() == Ty) ? Args[1] : loConst(Ty, 0);
  if ((ID == Intrinsic::fshl || ID == Intrinsic::fshr) && Args.size() >= 3)
    RelY = Args[2];
  return applyRelation(B, Ty, R, RelX, RelY, Variant.Relation, Seed);
}

Value *VMVariantEmitter::emitCastValue(IRBuilder<> &B, unsigned Opcode,
                                       Type *SrcTy, Type *DstTy, Value *X,
                                       const CastVariant &Variant,
                                       uint64_t Seed) {
  YansoChoiceStream DecorationSeeds(Seed, "vm.cast.emit");
  uint64_t InputDecorationSeed = DecorationSeeds.next64();
  uint64_t OutputDecorationSeed = DecorationSeeds.next64();
  Value *UseX = X;
  if (auto *ITy = dyn_cast<IntegerType>(SrcTy)) {
    UseX = YMBA::decorateInteger(B, ITy, X, Variant.InputDecoration,
                                 InputDecorationSeed, "vm.cast.in");
  }

  Value *R = B.CreateCast(static_cast<Instruction::CastOps>(Opcode), UseX,
                          DstTy, "vm.cast.core");
  if (auto *ITy = dyn_cast<IntegerType>(DstTy)) {
    R = YMBA::decorateInteger(B, ITy, R, Variant.OutputDecoration,
                              OutputDecorationSeed, "vm.cast.out");
    Value *RelY = loConst(ITy, yanso_mix64(Seed, 0x7f4a7c159e3779b9ULL));
    return applyRelation(B, ITy, R, R, RelY, Variant.Relation, Seed);
  }
  return R;
}

//===----------------------------------------------------------------------===//
// Public emit entry points
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitICmpValue(IRBuilder<> &B, CmpInst::Predicate Pred,
                                       Type *Ty, Value *X, Value *Y,
                                       const PredicateVariant &Variant,
                                       uint64_t Seed) {
  Value *P = emitICmpExpr(B, Pred, Ty, X, Y, Variant.Primary, Seed);
  if (!Variant.Alternate)
    return P;
  YansoChoiceStream Choices(Seed, "vm.predicate.mux");
  IntegerType *I1 = Type::getInt1Ty(B.getContext());
  Value *P2 =
      emitICmpExpr(B, Pred, Ty, X, Y, *Variant.Alternate, Choices.next64());
  Value *Key = ConstantInt::get(I1, Choices.range(2));
  Value *Sel = B.CreateICmpEQ(B.CreateXor(B.CreateXor(P, Key), Key), P,
                              "vm.pred.mux.sel");
  return B.CreateSelect(Sel, P, P2, "vm.pred.mux");
}

Value *VMVariantEmitter::emitSelectValue(IRBuilder<> &B, Type *Ty, Value *Cond,
                                         Value *TrueV, Value *FalseV,
                                         const SelectVariant &Variant,
                                         uint64_t Seed) {
  Value *R0 = emitSelectExpr(B, Ty, Cond, TrueV, FalseV, Variant.Primary, Seed);
  if (!Variant.Alternate)
    return R0;
  YansoChoiceStream Choices(Seed, "vm.select.mux");
  Value *R1 = emitSelectExpr(B, Ty, Cond, TrueV, FalseV, *Variant.Alternate,
                             Choices.next64());
  Value *Sel =
      B.CreateICmpEQ(B.CreateXor(Cond, ConstantInt::getFalse(B.getContext())),
                     Cond, "vm.select.mux.sel");
  if (auto *ITy = dyn_cast<IntegerType>(Ty)) {
    Value *Mask = B.CreateSub(loConst(ITy, 0), B.CreateZExt(Sel, ITy));
    Value *TruePart = B.CreateAnd(R0, Mask);
    Value *FalsePart = B.CreateAnd(R1, B.CreateNot(Mask));
    return B.CreateOr(TruePart, FalsePart, "vm.select.mux");
  }
  return B.CreateSelect(Sel, R0, R1, "vm.select.mux");
}

Value *VMVariantEmitter::emitBinaryValue(IRBuilder<> &B, unsigned Opcode,
                                         IntegerType *Ty, Value *X, Value *Y,
                                         const BinaryVariant &Variant,
                                         uint64_t Seed) {
  switch (Variant.Mutation) {
  case BinaryMutation::None: {
    Value *R = YMBA::emitBinary(B, Opcode, Ty, X, Y, Seed);
    return applyRelation(B, Ty, R, X, Y, Variant.Relation, Seed);
  }
  case BinaryMutation::BitRebuild: {
    Value *R = YMBA::emitBinary(B, Opcode, Ty, X, Y, Seed);
    R = applyRelation(B, Ty, R, X, Y, Variant.Relation, Seed);
    return emitControlFlowBitRebuild(B, B.GetInsertBlock()->getParent(), Ty, R);
  }
  case BinaryMutation::DataMuxAddSub:
  case BinaryMutation::DataMuxOrAbsorb:
  case BinaryMutation::DataMuxXorRoundTrip:
    return emitDataMuxBinaryValue(B, Opcode, Ty, X, Y, Variant, Seed);
  }
  llvm_unreachable("unknown VM binary mutation");
}
