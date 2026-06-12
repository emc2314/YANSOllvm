#include "CryptoUtils.h"
#include "VMVariant.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
// Common helpers and capability gates
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::loConst(IntegerType *Ty, uint64_t V) {
  return ConstantInt::get(Ty, V);
}

Value *VMVariantEmitter::notV(IRBuilder<> &B, Value *V) { return B.CreateNot(V); }

bool VMVariantEmitter::supportsLoopMutation(unsigned Opcode, unsigned BitWidth) {
  return BitWidth >= 8 && BitWidth <= 64 && isSupportedBinaryOpcode(Opcode);
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
  return BitWidth >= 8 && BitWidth <= 64;
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

VMVariantEmitter::BinaryVariant VMVariantEmitter::selectBinaryVariant(
    unsigned Opcode, IntegerType *Ty, uint64_t Seed, unsigned MutationPermille,
    unsigned RelationAppPermille) {
  BinaryVariant V;
  unsigned BitWidth = Ty->getBitWidth();

  V.ExprVariant = Seed % binaryExprVariantCount(Opcode);

  if (supportsRelation(BitWidth) &&
      (RelationAppPermille >= 1000 ||
       ((Seed >> 56) % 1000) < RelationAppPermille)) {
    switch ((Seed >> 4) % 3) {
    case 0:
      V.Relation = RelationKind::PopcountCarry;
      break;
    case 1:
      V.Relation = RelationKind::MaskedPartition;
      break;
    default:
      V.Relation = RelationKind::AffineRoundTrip;
      break;
    }
    switch ((Seed >> 8) % 4) {
    case 0:
      V.RelationApp = RelationApplication::DiffFold;
      break;
    case 1:
      V.RelationApp = RelationApplication::PairedMulBranch;
      break;
    case 2:
      V.RelationApp = RelationApplication::PairedAffineBranch;
      break;
    default:
      V.RelationApp = RelationApplication::OpaqueFork;
      break;
    }
    switch ((Seed >> 10) % 3) {
    case 0:
      V.Projector = ProjectorKind::LowBit;
      break;
    case 1:
      V.Projector = ProjectorKind::Parity;
      break;
    default:
      V.Projector = ProjectorKind::KeyedBit;
      break;
    }
    V.RelationVariant = (Seed >> 12) & 7;
  }

  bool SupportsLoop = supportsLoopMutation(Opcode, BitWidth);
  bool SupportsMux = supportsDataMuxMutation(Opcode, BitWidth);
  if ((SupportsLoop || SupportsMux) &&
      (MutationPermille >= 1000 || (Seed % 1000) < MutationPermille)) {
    if (SupportsLoop && SupportsMux) {
      if (((Seed >> 24) & 1) == 0) {
        V.Mutation = MutationKind::BitRebuild;
        V.MutationVariant = (Seed >> 32) & 3;
      } else {
        V.Mutation = MutationKind::DataMux;
        V.MutationVariant = (Seed >> 48) & 3;
      }
    } else if (SupportsLoop) {
      V.Mutation = MutationKind::BitRebuild;
      V.MutationVariant = (Seed >> 32) & 3;
    } else {
      V.Mutation = MutationKind::DataMux;
      V.MutationVariant = (Seed >> 48) & 3;
    }
  }
  return V;
}

//===----------------------------------------------------------------------===//
// Handler-name suffix formatting
//===----------------------------------------------------------------------===//

std::string VMVariantEmitter::suffix(const BinaryVariant &Variant) {
  std::string S;
  raw_string_ostream OS(S);
  OS << "_e" << Variant.ExprVariant;
  switch (Variant.Mutation) {
  case MutationKind::None:
    break;
  case MutationKind::BitRebuild:
    OS << "_mloop" << Variant.MutationVariant;
    break;
  case MutationKind::DataMux:
    OS << "_mmux" << Variant.MutationVariant;
    break;
  }
  switch (Variant.Relation) {
  case RelationKind::None:
    break;
  case RelationKind::PopcountCarry:
    OS << "_rpopcarry" << Variant.RelationVariant;
    break;
  case RelationKind::MaskedPartition:
    OS << "_rmaskpart" << Variant.RelationVariant;
    break;
  case RelationKind::AffineRoundTrip:
    OS << "_raffrt" << Variant.RelationVariant;
    break;
  }
  if (Variant.Relation != RelationKind::None) {
    switch (Variant.Projector) {
    case ProjectorKind::LowBit:
      OS << "pl";
      break;
    case ProjectorKind::Parity:
      OS << "pp";
      break;
    case ProjectorKind::KeyedBit:
      OS << "pk";
      break;
    }
    switch (Variant.RelationApp) {
    case RelationApplication::DiffFold:
      OS << "d";
      break;
    case RelationApplication::PairedMulBranch:
      OS << "m";
      break;
    case RelationApplication::PairedAffineBranch:
      OS << "a";
      break;
    case RelationApplication::OpaqueFork:
      OS << "f";
      break;
    }
  }
  OS.flush();
  return S;
}

std::string VMVariantEmitter::suffix(const ScalarVariant &Variant) {
  std::string S;
  raw_string_ostream OS(S);
  OS << "_e" << Variant.ExprVariant;
  OS.flush();
  return S;
}

std::string VMVariantEmitter::suffix(const PredicateVariant &Variant) {
  std::string S;
  raw_string_ostream OS(S);
  OS << "_e" << Variant.ExprVariant;
  switch (Variant.Mutation) {
  case MutationKind::None:
  case MutationKind::BitRebuild:
    break;
  case MutationKind::DataMux:
    OS << "_pmux" << Variant.MutationVariant;
    break;
  }
  OS.flush();
  return S;
}

std::string VMVariantEmitter::suffix(const SelectVariant &Variant) {
  std::string S;
  raw_string_ostream OS(S);
  OS << "_e" << Variant.ExprVariant;
  switch (Variant.Mutation) {
  case MutationKind::None:
  case MutationKind::BitRebuild:
    break;
  case MutationKind::DataMux:
    OS << "_smux" << Variant.MutationVariant;
    break;
  }
  OS.flush();
  return S;
}

VMVariantEmitter::PredicateVariant VMVariantEmitter::selectPredicateVariant(
    Type *Ty, uint64_t Seed, unsigned MutationPermille) {
  PredicateVariant V;
  V.ExprVariant = Seed % 3;
  if (!supportsPredicateMutation(Ty))
    return V;
  if (MutationPermille >= 1000) {
    V.Mutation = MutationKind::DataMux;
    V.MutationVariant = (Seed >> 24) & 3;
  } else if (((Seed >> 8) % 1000) < MutationPermille) {
    V.Mutation = MutationKind::DataMux;
    V.MutationVariant = (Seed >> 24) & 3;
  }
  return V;
}

VMVariantEmitter::SelectVariant VMVariantEmitter::selectSelectVariant(
    Type *Ty, uint64_t Seed, unsigned MutationPermille) {
  SelectVariant V;
  V.ExprVariant = Seed % 3;
  if (!supportsSelectMutation(Ty))
    return V;
  if (MutationPermille >= 1000) {
    V.Mutation = MutationKind::DataMux;
    V.MutationVariant = (Seed >> 24) & 3;
  } else if (((Seed >> 8) % 1000) < MutationPermille) {
    V.Mutation = MutationKind::DataMux;
    V.MutationVariant = (Seed >> 24) & 3;
  }
  return V;
}

VMVariantEmitter::ScalarVariant VMVariantEmitter::selectIntrinsicVariant(
    Intrinsic::ID ID, IntegerType *Ty, uint64_t Seed) {
  (void)ID;
  (void)Ty;
  ScalarVariant V;
  V.ExprVariant = Seed % 4;
  return V;
}

VMVariantEmitter::ScalarVariant VMVariantEmitter::selectCastVariant(
    unsigned Opcode, Type *SrcTy, Type *DstTy, uint64_t Seed) {
  (void)Opcode;
  (void)SrcTy;
  (void)DstTy;
  ScalarVariant V;
  V.ExprVariant = Seed % 4;
  return V;
}

//===----------------------------------------------------------------------===//
// Binary expression templates
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitXorExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                                     Value *Y, unsigned Variant) {
  switch (Variant % 4) {
  case 0:
    return B.CreateXor(X, Y);
  case 1:
    return B.CreateSub(B.CreateOr(X, Y), B.CreateAnd(X, Y));
  case 2: {
    Value *A = B.CreateAdd(X, Y);
    Value *C = B.CreateShl(B.CreateAnd(X, Y), loConst(Ty, 1));
    return B.CreateSub(A, C);
  }
  default: {
    Value *A = B.CreateAnd(X, notV(B, Y));
    Value *C = B.CreateAnd(notV(B, X), Y);
    return B.CreateOr(A, C);
  }
  }
}

Value *VMVariantEmitter::emitAndExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                                     Value *Y, unsigned Variant) {
  (void)Ty;
  switch (Variant % 4) {
  case 0:
    return B.CreateAnd(X, Y);
  case 1:
    return notV(B, B.CreateOr(notV(B, X), notV(B, Y)));
  case 2:
    return B.CreateAnd(X, Y);
  default:
    return B.CreateAnd(X, Y);
  }
}

Value *VMVariantEmitter::emitOrExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                                    Value *Y, unsigned Variant) {
  (void)Ty;
  switch (Variant % 4) {
  case 0:
    return B.CreateOr(X, Y);
  case 1:
    return notV(B, B.CreateAnd(notV(B, X), notV(B, Y)));
  case 2:
    return B.CreateAdd(B.CreateXor(X, Y), B.CreateAnd(X, Y));
  default:
    return B.CreateSub(B.CreateAdd(X, Y), B.CreateAnd(X, Y));
  }
}

Value *VMVariantEmitter::emitAddExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                                     Value *Y, unsigned Variant) {
  switch (Variant % 4) {
  case 0:
    return B.CreateAdd(X, Y);
  case 1:
    return B.CreateAdd(B.CreateXor(X, Y),
                       B.CreateShl(B.CreateAnd(X, Y), loConst(Ty, 1)));
  case 2:
    return B.CreateSub(X, B.CreateSub(loConst(Ty, 0), Y));
  default: {
    Value *R = B.CreateAdd(X, Y);
    Value *Noise = B.CreateMul(B.CreateXor(X, Y), loConst(Ty, 3));
    return B.CreateSub(B.CreateAdd(R, Noise), Noise);
  }
  }
}

Value *VMVariantEmitter::emitSubExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                                     Value *Y, unsigned Variant) {
  switch (Variant % 4) {
  case 0:
    return B.CreateSub(X, Y);
  case 1: {
    Value *R = B.CreateAdd(X, notV(B, Y));
    return B.CreateAdd(R, loConst(Ty, 1));
  }
  case 2: {
    Value *R = B.CreateAdd(X, notV(B, Y));
    return B.CreateAdd(R, loConst(Ty, 1));
  }
  default: {
    Value *R = B.CreateAdd(X, notV(B, Y));
    R = B.CreateAdd(R, loConst(Ty, 1));
    Value *Noise = B.CreateMul(emitOrExpr(B, Ty, X, Y, 0), loConst(Ty, 3));
    return B.CreateSub(B.CreateAdd(R, Noise), Noise);
  }
  }
}

Value *VMVariantEmitter::emitShiftExpr(IRBuilder<> &B, unsigned Opcode,
                                       IntegerType *Ty, Value *X, Value *Y,
                                       unsigned Variant) {
  unsigned BW = Ty->getBitWidth();

  switch (Variant % 3) {
  case 0:
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X, Y);
  case 1: {
    Value *Noise = B.CreateMul(B.CreateXor(X, Y), loConst(Ty, 3));
    Value *UseAmt = B.CreateSub(B.CreateAdd(Y, Noise), Noise);
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X, UseAmt);
  }
  default: {
    Value *Salt = loConst(Ty, (BW * 13u) | 1u);
    Value *Noise = B.CreateOr(B.CreateAnd(X, Y), Salt);
    Value *UseAmt = B.CreateSub(B.CreateAdd(Y, Noise), Noise);
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X, UseAmt);
  }
  }
}

Value *VMVariantEmitter::decorateIntegerResult(IRBuilder<> &B, IntegerType *Ty,
                                               Value *V, unsigned Variant,
                                               uint64_t Seed,
                                               StringRef NamePrefix) {
  switch (Variant % 4) {
  case 0:
    return V;
  case 1: {
    Value *K = loConst(Ty, yanso_mix64(Seed, 0x6a09e667f3bcc909ULL));
    return B.CreateXor(B.CreateXor(V, K, Twine(NamePrefix) + ".xor0"), K,
                       Twine(NamePrefix) + ".xor1");
  }
  case 2: {
    Value *N = B.CreateOr(
        B.CreateAnd(V, loConst(Ty, yanso_mix64(Seed, 0xbb67ae8584caa73bULL))),
        loConst(Ty, 1), Twine(NamePrefix) + ".noise");
    return B.CreateSub(B.CreateAdd(V, N, Twine(NamePrefix) + ".add"), N,
                       Twine(NamePrefix) + ".sub");
  }
  default: {
    Value *K = loConst(Ty, yanso_mix64(Seed, 0x3c6ef372fe94f82bULL));
    Value *M = B.CreateOr(V, K, Twine(NamePrefix) + ".mix");
    return B.CreateXor(B.CreateXor(V, M, Twine(NamePrefix) + ".mixxor0"), M,
                       Twine(NamePrefix) + ".mixxor1");
  }
  }
}

Value *VMVariantEmitter::emitDivRemExpr(IRBuilder<> &B, unsigned Opcode,
                                        IntegerType *Ty, Value *X, Value *Y,
                                        unsigned Variant, uint64_t Seed) {
  Value *UseX = X;
  Value *UseY = Y;
  switch (Variant % 4) {
  case 0:
    break;
  case 1: {
    Value *K = loConst(Ty, yanso_mix64(Seed, 0x510e527fade682d1ULL));
    UseX = B.CreateXor(B.CreateXor(X, K, "vm.div.x.xor0"), K,
                       "vm.div.x.xor1");
    break;
  }
  case 2: {
    Value *N = B.CreateOr(
        B.CreateAnd(X, loConst(Ty, yanso_mix64(Seed, 0x9b05688c2b3e6c1fULL))),
        loConst(Ty, 1), "vm.div.y.noise");
    UseY = B.CreateSub(B.CreateAdd(Y, N, "vm.div.y.add"), N,
                       "vm.div.y.sub");
    break;
  }
  default: {
    Value *K = loConst(Ty, yanso_mix64(Seed, 0x1f83d9abfb41bd6bULL));
    Value *N = B.CreateOr(B.CreateXor(X, K, "vm.div.xy.mix"), loConst(Ty, 1));
    UseX = B.CreateXor(B.CreateXor(X, K, "vm.div.x.mixxor0"), K,
                       "vm.div.x.mixxor1");
    UseY = B.CreateSub(B.CreateAdd(Y, N, "vm.div.y.mixadd"), N,
                       "vm.div.y.mixsub");
    break;
  }
  }
  Value *R = B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), UseX,
                           UseY, "vm.divrem.core");
  return decorateIntegerResult(B, Ty, R, Variant + 1, Seed, "vm.divrem.out");
}

unsigned VMVariantEmitter::binaryExprVariantCount(unsigned Opcode) {
  switch (Opcode) {
  case BinaryOperator::Add:
  case BinaryOperator::Sub:
  case BinaryOperator::And:
  case BinaryOperator::Or:
  case BinaryOperator::Xor:
  case BinaryOperator::UDiv:
  case BinaryOperator::SDiv:
  case BinaryOperator::URem:
  case BinaryOperator::SRem:
    return 4;
  case BinaryOperator::Mul:
  case BinaryOperator::Shl:
  case BinaryOperator::LShr:
  case BinaryOperator::AShr:
    return 3;
  default:
    return 1;
  }
}

bool VMVariantEmitter::isSupportedBinaryOpcode(unsigned Opcode) {
  switch (Opcode) {
  case BinaryOperator::Add:
  case BinaryOperator::Sub:
  case BinaryOperator::Mul:
  case BinaryOperator::UDiv:
  case BinaryOperator::SDiv:
  case BinaryOperator::URem:
  case BinaryOperator::SRem:
  case BinaryOperator::Shl:
  case BinaryOperator::LShr:
  case BinaryOperator::AShr:
  case BinaryOperator::And:
  case BinaryOperator::Or:
  case BinaryOperator::Xor:
    return true;
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Predicate / ICmp expression templates and wrappers
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitICmpExpr(IRBuilder<> &B, CmpInst::Predicate Pred,
                                      Type *Ty, Value *X, Value *Y,
                                      unsigned ExprVariant, uint64_t Seed) {
  IntegerType *ITy = nullptr;
  Value *A = X;
  Value *C = Y;
  if (auto *PtrTy = dyn_cast<PointerType>(Ty)) {
    (void)PtrTy;
    ITy = B.getIntPtrTy(B.GetInsertBlock()->getModule()->getDataLayout());
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
  Value *Eq = B.CreateTrunc(EqWide, Type::getInt1Ty(B.getContext()),
                            "vm.pred.eq");
  Value *Ne = B.CreateTrunc(NZWide, Type::getInt1Ty(B.getContext()),
                            "vm.pred.ne");

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
      Value *SubSign = B.CreateLShr(Diff, loConst(ITy, BW - 1), "vm.pred.ssub.sign");
      LtWide = B.CreateOr(B.CreateAnd(SignDiff, SX),
                          B.CreateAnd(B.CreateXor(SignDiff, loConst(ITy, 1)),
                                      SubSign),
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
    Value *LtYX = emitICmpExpr(B, Swapped, ITy, C, A, 0, Seed);
    Base = (Pred == CmpInst::ICMP_ULE || Pred == CmpInst::ICMP_SLE)
               ? B.CreateNot(LtYX, "vm.pred.le")
               : LtYX;
    break;
  }
  default:
    Base = B.CreateICmp(Pred, X, Y);
    break;
  }

  switch (ExprVariant % 3) {
  case 0:
    return Base;
  case 1:
    return B.CreateNot(B.CreateNot(Base), "vm.pred.notnot");
  default: {
    Value *K = ConstantInt::get(Type::getInt1Ty(B.getContext()), (Seed >> 7) & 1);
    return B.CreateXor(B.CreateXor(Base, K), K, "vm.pred.xorxor");
  }
  }
}

//===----------------------------------------------------------------------===//
// Select expression templates and wrappers
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitSelectExpr(IRBuilder<> &B, Type *Ty, Value *Cond,
                                        Value *TrueV, Value *FalseV,
                                        unsigned ExprVariant, uint64_t Seed) {
  (void)Seed;
  if (auto *ITy = dyn_cast<IntegerType>(Ty)) {
    switch (ExprVariant % 3) {
    case 0:
      return B.CreateSelect(Cond, TrueV, FalseV);
    case 1: {
      Value *Mask = B.CreateSub(loConst(ITy, 0), B.CreateZExt(Cond, ITy));
      Value *TruePart = B.CreateAnd(TrueV, Mask);
      Value *FalsePart = B.CreateAnd(FalseV, B.CreateNot(Mask));
      return B.CreateOr(TruePart, FalsePart, "vm.select.mask");
    }
    default: {
      Value *InvCond = B.CreateNot(Cond);
      return B.CreateSelect(InvCond, FalseV, TrueV, "vm.select.invert");
    }
    }
  }
  switch (ExprVariant % 2) {
  case 0:
    return B.CreateSelect(Cond, TrueV, FalseV);
  default:
    return B.CreateSelect(B.CreateNot(Cond), FalseV, TrueV, "vm.select.invert");
  }
}

Value *VMVariantEmitter::emitBinaryExpr(IRBuilder<> &B, unsigned Opcode,
                                        IntegerType *Ty, Value *X, Value *Y,
                                        unsigned ExprVariant, uint64_t Seed) {
  switch (Opcode) {
  case BinaryOperator::Add:
    return emitAddExpr(B, Ty, X, Y, ExprVariant);
  case BinaryOperator::Sub:
    return emitSubExpr(B, Ty, X, Y, ExprVariant);
  case BinaryOperator::And:
    return emitAndExpr(B, Ty, X, Y, ExprVariant);
  case BinaryOperator::Or:
    return emitOrExpr(B, Ty, X, Y, ExprVariant);
  case BinaryOperator::Xor:
    return emitXorExpr(B, Ty, X, Y, ExprVariant);
  case BinaryOperator::Mul:
    if (ExprVariant == 0)
      return B.CreateMul(X, Y);
    else {
      Value *Base = B.CreateMul(X, Y);
      Value *Noise = B.CreateMul(emitXorExpr(B, Ty, X, Y, ExprVariant),
                                 loConst(Ty, (Seed % 7) | 1));
      return B.CreateSub(B.CreateAdd(Base, Noise), Noise);
    }
  case BinaryOperator::Shl:
  case BinaryOperator::LShr:
  case BinaryOperator::AShr:
    return emitShiftExpr(B, Opcode, Ty, X, Y, ExprVariant);
  case BinaryOperator::UDiv:
  case BinaryOperator::SDiv:
  case BinaryOperator::URem:
  case BinaryOperator::SRem:
    return emitDivRemExpr(B, Opcode, Ty, X, Y, ExprVariant, Seed);
  default:
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X, Y);
  }
}

//===----------------------------------------------------------------------===//
// Relation providers, projectors, and relation applications
//===----------------------------------------------------------------------===//

Value *VMVariantEmitter::emitRotateRight(IRBuilder<> &B, IntegerType *Ty,
                                         Value *X, unsigned Amount) {
  unsigned BW = Ty->getBitWidth();
  Amount %= BW;
  if (Amount == 0)
    return X;
  Value *Lo = B.CreateLShr(X, loConst(Ty, Amount));
  Value *Hi = B.CreateShl(X, loConst(Ty, BW - Amount));
  return B.CreateOr(Lo, Hi, "vm.id.rot");
}

VMVariantEmitter::Relation VMVariantEmitter::emitRelation(
    IRBuilder<> &B, IntegerType *Ty, Value *X, Value *Y, RelationKind Kind,
    unsigned Variant, uint64_t Seed) {
  switch (Kind) {
  case RelationKind::None:
    return {loConst(Ty, 0), loConst(Ty, 0)};
  case RelationKind::PopcountCarry: {
    Value *A = (Variant & 1) ? emitRotateRight(B, Ty, X, 1 + ((Seed >> 21) % (Ty->getBitWidth() - 1))) : X;
    Value *C = (Variant & 2) ? emitRotateRight(B, Ty, Y, 1 + ((Seed >> 29) % (Ty->getBitWidth() - 1))) : Y;
    Value *PA = B.CreateUnaryIntrinsic(Intrinsic::ctpop, A);
    Value *PC = B.CreateUnaryIntrinsic(Intrinsic::ctpop, C);
    Value *PXor = B.CreateUnaryIntrinsic(Intrinsic::ctpop,
                                         B.CreateXor(A, C, "vm.rel.popcarry.xor"));
    Value *PAnd = B.CreateUnaryIntrinsic(Intrinsic::ctpop,
                                         B.CreateAnd(A, C, "vm.rel.popcarry.and"));
    Value *L = B.CreateAdd(PA, PC, "vm.rel.lhs");
    Value *R = B.CreateAdd(PXor, B.CreateShl(PAnd, loConst(Ty, 1)),
                           "vm.rel.rhs");
    return {L, R};
  }
  case RelationKind::MaskedPartition: {
    Value *Mask = loConst(Ty, yanso_mix64(Seed, 0x8c3d37c819544da2ULL));
    Value *A = (Variant & 1) ? emitRotateRight(B, Ty, X, 1 + ((Seed >> 17) % (Ty->getBitWidth() - 1))) : X;
    Value *L = A;
    Value *R = B.CreateOr(B.CreateAnd(A, Mask, "vm.rel.maskpart.lo"),
                          B.CreateAnd(A, B.CreateNot(Mask), "vm.rel.maskpart.hi"),
                          "vm.rel.maskpart.rhs");
    return {L, R};
  }
  case RelationKind::AffineRoundTrip: {
    uint64_t AConst = yanso_mix64(Seed, 0xd1b54a32d192ed03ULL) | 1ULL;
    uint64_t CConst = yanso_mix64(Seed, 0x94d049bb133111ebULL);
    Value *A = loConst(Ty, AConst);
    Value *AInv = loConst(Ty, oddInverse64(AConst));
    Value *C = loConst(Ty, CConst);
    Value *Base = (Variant & 1) ? X : B.CreateXor(X, Y, "vm.rel.aff.base");
    Value *Enc = B.CreateAdd(B.CreateMul(Base, A, "vm.rel.aff.mul"), C,
                             "vm.rel.aff.enc");
    Value *Dec = B.CreateMul(B.CreateSub(Enc, C, "vm.rel.aff.sub"), AInv,
                             "vm.rel.aff.dec");
    return {Base, Dec};
  }
  }
  llvm_unreachable("unknown VM relation kind");
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
    unsigned BW = Ty->getBitWidth();
    unsigned Shift = (Seed >> 33) % BW;
    Value *Mixed = B.CreateXor(V, loConst(Ty, yanso_mix64(Seed, 0x517cc1b727220a95ULL)),
                               Twine(Name) + ".mix");
    return B.CreateAnd(B.CreateLShr(Mixed, loConst(Ty, Shift)), loConst(Ty, 1),
                       Name);
  }
  }
  llvm_unreachable("unknown VM relation projector");
}

Value *VMVariantEmitter::applyRelationDiffFold(IRBuilder<> &B, IntegerType *Ty,
                                               Value *R,
                                               const Relation &Rel) {
  Value *T = B.CreateAdd(R, Rel.L, "vm.id.app.diff.addx");
  return B.CreateSub(T, Rel.R, "vm.id.app.diff.suby");
}

uint64_t VMVariantEmitter::oddInverse64(uint64_t V) {
  uint64_t X = V;
  for (unsigned I = 0; I != 6; ++I)
    X *= 2 - V * X;
  return X;
}

Value *VMVariantEmitter::applyRelationPairedMulBranch(
    IRBuilder<> &B, IntegerType *Ty, Value *R, const Relation &Rel,
    ProjectorKind Projector, uint64_t Seed) {
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
  Value *AInv = loConst(Ty, oddInverse64(A));
  Value *BInv = loConst(Ty, oddInverse64(Bc));

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

  Value *PX = emitProjector(B, Ty, Rel.L, Projector, Seed,
                            "vm.id.app.aff.predx.v");
  Value *PY = emitProjector(B, Ty, Rel.R, Projector, Seed,
                            "vm.id.app.aff.predy.v");
  Value *Pred0 = B.CreateICmpNE(PX, loConst(Ty, 0), "vm.id.app.aff.predx");
  Value *Pred1 = B.CreateICmpNE(PY, loConst(Ty, 0), "vm.id.app.aff.predy");

  uint64_t A = yanso_mix64(Seed, 0x452821e638d01377ULL) | 1ULL;
  uint64_t Bc = yanso_mix64(Seed, 0xbe5466cf34e90c6cULL) | 1ULL;
  Value *A0 = loConst(Ty, A);
  Value *B0 = loConst(Ty, Bc);
  Value *AInv = loConst(Ty, oddInverse64(A));
  Value *BInv = loConst(Ty, oddInverse64(Bc));
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
  Value *T1 = T1B.CreateMul(T1B.CreateSub(Mid, C0, "vm.id.app.aff.sub.c"),
                            AInv, "vm.id.app.aff.mul.ainv");
  T1B.CreateBr(Join1);

  IRBuilder<> E1B(Else1);
  Value *E1 = E1B.CreateMul(E1B.CreateSub(Mid, D0, "vm.id.app.aff.sub.d"),
                            BInv, "vm.id.app.aff.mul.binv");
  E1B.CreateBr(Join1);

  IRBuilder<> J1B(Join1);
  PHINode *Out = J1B.CreatePHI(Ty, 2, "vm.id.app.aff.result");
  Out->addIncoming(T1, Then1);
  Out->addIncoming(E1, Else1);
  B.SetInsertPoint(Join1);
  return Out;
}

Value *VMVariantEmitter::applyRelationOpaqueFork(
    IRBuilder<> &B, IntegerType *Ty, Value *R, const Relation &Rel,
    ProjectorKind Projector, uint64_t Seed) {
  Function *F = B.GetInsertBlock()->getParent();
  LLVMContext &Ctx = F->getContext();
  BasicBlock *Good = BasicBlock::Create(Ctx, "vm.id.app.fork.good", F);
  BasicBlock *Bad = BasicBlock::Create(Ctx, "vm.id.app.fork.bad", F);
  BasicBlock *Join = BasicBlock::Create(Ctx, "vm.id.app.fork.join", F);

  Value *PX = emitProjector(B, Ty, Rel.L, Projector, Seed,
                            "vm.id.app.fork.predx.v");
  Value *PY = emitProjector(B, Ty, Rel.R, Projector, Seed,
                            "vm.id.app.fork.predy.v");
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

Value *VMVariantEmitter::applyRelation(IRBuilder<> &B, IntegerType *Ty, Value *R,
                                       Value *X, Value *Y,
                                       const BinaryVariant &Variant,
                                       uint64_t Seed) {
  if (Variant.Relation == RelationKind::None)
    return R;

  Relation Rel = emitRelation(B, Ty, X, Y, Variant.Relation,
                              Variant.RelationVariant, Seed);
  switch (Variant.RelationApp) {
  case RelationApplication::DiffFold:
    return applyRelationDiffFold(B, Ty, R, Rel);
  case RelationApplication::PairedMulBranch:
    return applyRelationPairedMulBranch(B, Ty, R, Rel, Variant.Projector,
                                        Seed);
  case RelationApplication::PairedAffineBranch:
    return applyRelationPairedAffineBranch(B, Ty, R, Rel, Variant.Projector,
                                           Seed);
  case RelationApplication::OpaqueFork:
    return applyRelationOpaqueFork(B, Ty, R, Rel, Variant.Projector, Seed);
  }
  llvm_unreachable("unknown VM relation application");
}

//===----------------------------------------------------------------------===//
// Binary mutation wrappers
//===----------------------------------------------------------------------===//

void VMVariantEmitter::emitControlFlowBitRebuild(IRBuilder<> &B, Function *F,
                                                 IntegerType *Ty, Value *Input) {
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
  DB.CreateRet(A);
}

void VMVariantEmitter::emitDataMuxBinary(IRBuilder<> &B, unsigned Opcode,
                                         IntegerType *Ty, Value *X, Value *Y,
                                         const BinaryVariant &Variant,
                                         uint64_t Seed) {
  Value *R0 = emitBinaryExpr(B, Opcode, Ty, X, Y, Variant.ExprVariant, Seed);
  R0 = applyRelation(B, Ty, R0, X, Y, Variant, Seed);
  Value *R1 = emitBinaryExpr(B, Opcode, Ty, X, Y, Variant.ExprVariant + 1,
                             yanso_mix64(Seed, 0x13198a2e03707344ULL));
  R1 = applyRelation(B, Ty, R1, X, Y, Variant,
                     yanso_mix64(Seed, 0x13198a2e03707344ULL));

  Value *K = loConst(Ty, yanso_mix64(Seed, 0xa4093822299f31d0ULL));
  Value *Pred = nullptr;
  switch (Variant.MutationVariant % 3) {
  case 0:
    Pred = B.CreateICmpEQ(B.CreateSub(B.CreateAdd(X, K), K), X,
                          "vm.mux.pred");
    break;
  case 1:
    Pred = B.CreateICmpEQ(B.CreateOr(B.CreateAnd(X, Y), X), X,
                          "vm.mux.pred");
    break;
  default:
    Pred = B.CreateICmpEQ(B.CreateXor(B.CreateXor(Y, K), K), Y,
                          "vm.mux.pred");
    break;
  }

  Value *Mask = B.CreateSub(loConst(Ty, 0), B.CreateZExt(Pred, Ty));
  Value *TruePart = B.CreateAnd(R0, Mask);
  Value *FalsePart = B.CreateAnd(R1, B.CreateNot(Mask));
  B.CreateRet(B.CreateOr(TruePart, FalsePart));
}

//===----------------------------------------------------------------------===//
// Intrinsic and cast wrappers
//===----------------------------------------------------------------------===//

void VMVariantEmitter::emitIntrinsic(IRBuilder<> &B, Intrinsic::ID ID,
                                     IntegerType *Ty, ArrayRef<Value *> Args,
                                     const ScalarVariant &Variant,
                                     uint64_t Seed) {
  FunctionCallee Intr = Intrinsic::getOrInsertDeclaration(
      B.GetInsertBlock()->getModule(), ID, {Ty});
  Value *R = B.CreateCall(Intr, Args, "vm.intr.core");
  B.CreateRet(decorateIntegerResult(B, Ty, R, Variant.ExprVariant, Seed,
                                    "vm.intr.out"));
}

void VMVariantEmitter::emitCast(IRBuilder<> &B, unsigned Opcode, Type *SrcTy,
                                Type *DstTy, Value *X,
                                const ScalarVariant &Variant, uint64_t Seed) {
  Value *UseX = X;
  if (auto *ITy = dyn_cast<IntegerType>(SrcTy)) {
    UseX = decorateIntegerResult(B, ITy, X, Variant.ExprVariant, Seed,
                                 "vm.cast.in");
  }

  Value *R = B.CreateCast(static_cast<Instruction::CastOps>(Opcode), UseX,
                          DstTy, "vm.cast.core");
  if (auto *ITy = dyn_cast<IntegerType>(DstTy))
    R = decorateIntegerResult(B, ITy, R, Variant.ExprVariant + 1, Seed,
                              "vm.cast.out");
  B.CreateRet(R);
}

//===----------------------------------------------------------------------===//
// Public emit entry points
//===----------------------------------------------------------------------===//

void VMVariantEmitter::emitICmp(IRBuilder<> &B, CmpInst::Predicate Pred,
                                Type *Ty, Value *X, Value *Y,
                                const PredicateVariant &Variant,
                                uint64_t Seed) {
  Value *P = emitICmpExpr(B, Pred, Ty, X, Y, Variant.ExprVariant, Seed);
  switch (Variant.Mutation) {
  case MutationKind::None:
  case MutationKind::BitRebuild:
    B.CreateRet(P);
    return;
  case MutationKind::DataMux: {
    IntegerType *I1 = Type::getInt1Ty(B.getContext());
    Value *P2 = emitICmpExpr(B, Pred, Ty, X, Y, Variant.ExprVariant + 1,
                             yanso_mix64(Seed, 0x7f4a7c159e3779b9ULL));
    Value *Key = ConstantInt::get(I1, (Seed >> 9) & 1);
    Value *Sel = B.CreateICmpEQ(B.CreateXor(B.CreateXor(P, Key), Key), P,
                                "vm.pred.mux.sel");
    B.CreateRet(B.CreateSelect(Sel, P, P2, "vm.pred.mux"));
    return;
  }
  }
  llvm_unreachable("unknown VM predicate mutation");
}

void VMVariantEmitter::emitSelect(IRBuilder<> &B, Type *Ty, Value *Cond,
                                  Value *TrueV, Value *FalseV,
                                  const SelectVariant &Variant,
                                  uint64_t Seed) {
  switch (Variant.Mutation) {
  case MutationKind::None:
  case MutationKind::BitRebuild:
    B.CreateRet(emitSelectExpr(B, Ty, Cond, TrueV, FalseV, Variant.ExprVariant,
                               Seed));
    return;
  case MutationKind::DataMux: {
    Value *R0 = emitSelectExpr(B, Ty, Cond, TrueV, FalseV, Variant.ExprVariant,
                               Seed);
    Value *R1 = emitSelectExpr(B, Ty, Cond, TrueV, FalseV,
                               Variant.ExprVariant + 1,
                               yanso_mix64(Seed, 0x13198a2e03707344ULL));
    Value *Sel = B.CreateICmpEQ(B.CreateXor(Cond, ConstantInt::getFalse(B.getContext())),
                                Cond, "vm.select.mux.sel");
    if (auto *ITy = dyn_cast<IntegerType>(Ty)) {
      Value *Mask = B.CreateSub(loConst(ITy, 0), B.CreateZExt(Sel, ITy));
      Value *TruePart = B.CreateAnd(R0, Mask);
      Value *FalsePart = B.CreateAnd(R1, B.CreateNot(Mask));
      B.CreateRet(B.CreateOr(TruePart, FalsePart, "vm.select.mux"));
    } else {
      B.CreateRet(B.CreateSelect(Sel, R0, R1, "vm.select.mux"));
    }
    return;
  }
  }
  llvm_unreachable("unknown VM select mutation");
}

void VMVariantEmitter::emitBinary(IRBuilder<> &B, unsigned Opcode,
                                  IntegerType *Ty, Value *X, Value *Y,
                                  const BinaryVariant &Variant, uint64_t Seed) {
  switch (Variant.Mutation) {
  case MutationKind::None: {
    Value *R = emitBinaryExpr(B, Opcode, Ty, X, Y, Variant.ExprVariant, Seed);
    B.CreateRet(applyRelation(B, Ty, R, X, Y, Variant, Seed));
    return;
  }
  case MutationKind::BitRebuild: {
    Value *R = emitBinaryExpr(B, Opcode, Ty, X, Y, Variant.ExprVariant, Seed);
    R = applyRelation(B, Ty, R, X, Y, Variant, Seed);
    return emitControlFlowBitRebuild(B, B.GetInsertBlock()->getParent(), Ty, R);
  }
  case MutationKind::DataMux:
    return emitDataMuxBinary(B, Opcode, Ty, X, Y, Variant, Seed);
  }
  llvm_unreachable("unknown VM binary mutation");
}
