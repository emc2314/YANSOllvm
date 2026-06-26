#include "YMBA.h"
#include "CryptoUtils.h"

#include "GeneratedYMBACatalog.inc"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

using namespace llvm;

namespace {
// Fixed for now: not a runtime pass option, so the self-test can link YMBA
// without pulling command-line option RTTI into the standalone binary.
static constexpr double YMBACostTemperature = 16.0;

// Postfix bytecode opcodes. Same-width integer stack only.
enum : uint8_t {
  BC_VAR_X = 0x01,
  BC_VAR_Y = 0x02,
  BC_CONST_U = 0x08,

  BC_CTPOP = 0x12,
  BC_BSWAP = 0x13,
  BC_BITREVERSE = 0x14,

  BC_ADD = 0x20,
  BC_SUB = 0x21,
  BC_MUL = 0x22,
  BC_AND = 0x23,
  BC_OR = 0x24,
  BC_XOR = 0x25,
  BC_SHL = 0x26,
  BC_LSHR = 0x27,
  BC_ASHR = 0x28,
  BC_ROTL = 0x29,
  BC_ROTR = 0x2a,

  BC_SHL_IMM = 0x30,
  BC_LSHR_IMM = 0x31,
  BC_ASHR_IMM = 0x32,
  BC_ROTL_IMM = 0x33,
  BC_ROTR_IMM = 0x34,

  BC_UDIV_CONST = 0x40,
  BC_UREM_CONST = 0x41,
};

struct ExprRecord {
  uint32_t BytecodeOff = 0;
  uint32_t BytecodeLen = 0;
  uint16_t WidthMask = 0;
  uint16_t VarMask = 0;
  uint16_t Cost = 0;
  uint16_t MaxStack = 0;
};

struct RewriteRecord {
  uint16_t Opcode = 0;
  uint16_t WidthMask = 0;
  uint32_t Expr = 0;
};

struct RelationRecord {
  uint32_t LExpr = 0;
  uint32_t RExpr = 0;
  uint16_t WidthMask = 0;
};

struct Header {
  uint32_t NumExprs = 0;
  uint32_t NumRewrites = 0;
  uint32_t NumRelations = 0;
  uint32_t BytecodeSize = 0;
};

constexpr uint32_t HeaderSize = 16;
constexpr uint32_t ExprRecordSize = 16;
constexpr uint32_t RewriteRecordSize = 8;
constexpr uint32_t RelationRecordSize = 10;

static uint16_t read16(ArrayRef<uint8_t> Data, uint32_t Off) {
  return uint16_t(Data[Off]) | (uint16_t(Data[Off + 1]) << 8);
}

static uint32_t read32(ArrayRef<uint8_t> Data, uint32_t Off) {
  return uint32_t(Data[Off]) | (uint32_t(Data[Off + 1]) << 8) |
         (uint32_t(Data[Off + 2]) << 16) | (uint32_t(Data[Off + 3]) << 24);
}

static bool rangeInBounds(uint32_t Off, uint32_t Size, uint32_t Total) {
  return Off <= Total && Size <= Total - Off;
}

static std::optional<APInt> readULEB(ArrayRef<uint8_t> Bytes, uint32_t &Pos) {
  APInt Value(128, 0);
  unsigned Shift = 0;
  while (Pos < Bytes.size()) {
    uint8_t B = Bytes[Pos++];
    uint8_t Payload = B & 0x7f;
    if (Payload) {
      if (Shift >= 128)
        return std::nullopt;
      if (Shift > 121 && (Payload >> (128 - Shift)) != 0)
        return std::nullopt;
      Value |= APInt(128, Payload).shl(Shift);
    }
    if ((B & 0x80) == 0)
      return Value;
    Shift += 7;
  }
  return std::nullopt;
}

static uint16_t widthMaskFor(unsigned BitWidth) {
  switch (BitWidth) {
  case 8:
    return YMBA::YMBA_W8;
  case 16:
    return YMBA::YMBA_W16;
  case 32:
    return YMBA::YMBA_W32;
  case 64:
    return YMBA::YMBA_W64;
  case 128:
    return YMBA::YMBA_W128;
  default:
    return 0;
  }
}

static std::optional<uint16_t> toYMBAOpcode(unsigned LLVMOpcode) {
  switch (LLVMOpcode) {
  case BinaryOperator::Add:
    return YMBA::YMBA_OP_ADD;
  case BinaryOperator::Sub:
    return YMBA::YMBA_OP_SUB;
  case BinaryOperator::Mul:
    return YMBA::YMBA_OP_MUL;
  case BinaryOperator::And:
    return YMBA::YMBA_OP_AND;
  case BinaryOperator::Or:
    return YMBA::YMBA_OP_OR;
  case BinaryOperator::Xor:
    return YMBA::YMBA_OP_XOR;
  case BinaryOperator::Shl:
    return YMBA::YMBA_OP_SHL;
  case BinaryOperator::LShr:
    return YMBA::YMBA_OP_LSHR;
  case BinaryOperator::AShr:
    return YMBA::YMBA_OP_ASHR;
  default:
    return std::nullopt;
  }
}

static std::optional<uint16_t> intrinsicToYMBAOpcode(Intrinsic::ID ID) {
  switch (ID) {
  case Intrinsic::fshl:
    return YMBA::YMBA_OP_ROTL;
  case Intrinsic::fshr:
    return YMBA::YMBA_OP_ROTR;
  case Intrinsic::ctpop:
    return YMBA::YMBA_OP_CTPOP;
  case Intrinsic::bswap:
    return YMBA::YMBA_OP_BSWAP;
  case Intrinsic::bitreverse:
    return YMBA::YMBA_OP_BITREVERSE;
  default:
    return std::nullopt;
  }
}

static ConstantInt *loConst(IntegerType *Ty, uint64_t V) {
  uint64_t Mask = Ty->getBitWidth() >= 64
                      ? ~uint64_t(0)
                      : ((uint64_t(1) << Ty->getBitWidth()) - 1);
  return ConstantInt::get(Ty, V & Mask);
}

static Value *notV(IRBuilder<> &B, Value *V) { return B.CreateNot(V); }

static Value *emitXorExpr(IRBuilder<> &B, IntegerType *Ty, Value *X, Value *Y,
                          unsigned Variant) {
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

static Value *emitAndExpr(IRBuilder<> &B, IntegerType *, Value *X, Value *Y,
                          unsigned Variant) {
  switch (Variant % 2) {
  case 0:
    return B.CreateAnd(X, Y);
  default:
    return notV(B, B.CreateOr(notV(B, X), notV(B, Y)));
  }
}

static Value *emitOrExpr(IRBuilder<> &B, IntegerType *, Value *X, Value *Y,
                         unsigned Variant) {
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

static Value *emitAddExpr(IRBuilder<> &B, IntegerType *Ty, Value *X, Value *Y,
                          unsigned Variant) {
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

static Value *emitSubExpr(IRBuilder<> &B, IntegerType *Ty, Value *X, Value *Y,
                          unsigned Variant) {
  switch (Variant % 3) {
  case 0:
    return B.CreateSub(X, Y);
  case 1: {
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

static Value *decorateIntegerResult(IRBuilder<> &B, IntegerType *Ty, Value *V,
                                    unsigned Variant, uint64_t Seed,
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

static Value *emitShiftExpr(IRBuilder<> &B, unsigned Opcode, IntegerType *Ty,
                            Value *X, Value *Y, unsigned Variant) {
  unsigned BW = Ty->getBitWidth();
  switch (Variant % 3) {
  case 0:
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X, Y);
  case 1: {
    Value *Noise = B.CreateMul(B.CreateXor(X, Y), loConst(Ty, 3));
    Value *UseAmt = B.CreateSub(B.CreateAdd(Y, Noise), Noise);
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X,
                         UseAmt);
  }
  default: {
    Value *Salt = loConst(Ty, (BW * 13u) | 1u);
    Value *Noise = B.CreateOr(B.CreateAnd(X, Y), Salt);
    Value *UseAmt = B.CreateSub(B.CreateAdd(Y, Noise), Noise);
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X,
                         UseAmt);
  }
  }
}

static Value *emitDivRemExpr(IRBuilder<> &B, unsigned Opcode, IntegerType *Ty,
                             Value *X, Value *Y, unsigned Variant,
                             uint64_t Seed) {
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

static void collectBuiltinBinaryCosts(unsigned Opcode,
                                      SmallVectorImpl<unsigned> &Costs) {
  switch (Opcode) {
  case BinaryOperator::Add:
    Costs.append({1, 4, 2, 5});
    break;
  case BinaryOperator::Sub:
    Costs.append({1, 3, 6});
    break;
  case BinaryOperator::And:
    Costs.append({1, 4});
    break;
  case BinaryOperator::Or:
    Costs.append({1, 4, 3, 3});
    break;
  case BinaryOperator::Xor:
    Costs.append({1, 3, 4, 4});
    break;
  case BinaryOperator::Mul:
    Costs.append({1, 5, 5});
    break;
  case BinaryOperator::Shl:
  case BinaryOperator::LShr:
  case BinaryOperator::AShr:
    Costs.append({1, 4, 4});
    break;
  case BinaryOperator::UDiv:
  case BinaryOperator::SDiv:
  case BinaryOperator::URem:
  case BinaryOperator::SRem:
    Costs.append({1, 3, 5, 7});
    break;
  default:
    Costs.push_back(1);
    break;
  }
}

static Value *emitBuiltinBinary(IRBuilder<> &B, unsigned Opcode, IntegerType *Ty,
                                Value *X, Value *Y, unsigned Variant,
                                uint64_t Seed) {
  // For i1 the value set is {0,1}, so boolean and mod-2 arithmetic coincide and
  // the multi-bit MBA forms (which rely on a carry-propagating `(x&y) << 1`)
  // would emit a poison `shl i1, 1`.  Disguise each op as an equivalent
  // sibling instead: and<->mul, xor<->add<->sub are identities at one bit, and
  // or is rebuilt from them.
  if (Ty->getBitWidth() == 1) {
    switch (Opcode) {
    case BinaryOperator::And:
      return B.CreateMul(X, Y);
    case BinaryOperator::Mul:
      return B.CreateAnd(X, Y);
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
      return B.CreateXor(X, Y);
    case BinaryOperator::Xor:
      return B.CreateAdd(X, Y);
    case BinaryOperator::Or:
      // a | b == (a + b) - a*b  (== a + b - (a & b)) for one-bit values.
      return B.CreateSub(B.CreateAdd(X, Y), B.CreateMul(X, Y));
    default:
      break;
    }
  }
  switch (Opcode) {
  case BinaryOperator::Add:
    return emitAddExpr(B, Ty, X, Y, Variant);
  case BinaryOperator::Sub:
    return emitSubExpr(B, Ty, X, Y, Variant);
  case BinaryOperator::And:
    return emitAndExpr(B, Ty, X, Y, Variant);
  case BinaryOperator::Or:
    return emitOrExpr(B, Ty, X, Y, Variant);
  case BinaryOperator::Xor:
    return emitXorExpr(B, Ty, X, Y, Variant);
  case BinaryOperator::Mul:
    if (Variant == 0)
      return B.CreateMul(X, Y);
    else {
      Value *Base = B.CreateMul(X, Y);
      Value *Noise = B.CreateMul(emitXorExpr(B, Ty, X, Y, Variant),
                                 loConst(Ty, (Seed % 7) | 1));
      return B.CreateSub(B.CreateAdd(Base, Noise), Noise);
    }
  case BinaryOperator::Shl:
  case BinaryOperator::LShr:
  case BinaryOperator::AShr:
    return emitShiftExpr(B, Opcode, Ty, X, Y, Variant);
  case BinaryOperator::UDiv:
  case BinaryOperator::SDiv:
  case BinaryOperator::URem:
  case BinaryOperator::SRem:
    return emitDivRemExpr(B, Opcode, Ty, X, Y, Variant, Seed);
  default:
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X, Y);
  }
}

static FunctionCallee getIntrinsic(IRBuilder<> &B, Intrinsic::ID ID,
                                   IntegerType *Ty) {
  return Intrinsic::getOrInsertDeclaration(B.GetInsertBlock()->getModule(), ID,
                                           Ty);
}

static Value *emitFunnelIntrinsic(IRBuilder<> &B, Intrinsic::ID ID,
                                  IntegerType *Ty, Value *L, Value *R,
                                  Value *Amt) {
  return B.CreateCall(getIntrinsic(B, ID, Ty), {L, R, Amt});
}

static Value *emitRotateIntrinsic(IRBuilder<> &B, Intrinsic::ID ID,
                                  IntegerType *Ty, Value *V, Value *Amt) {
  return emitFunnelIntrinsic(B, ID, Ty, V, V, Amt);
}

static Value *emitUnaryIntrinsic(IRBuilder<> &B, Intrinsic::ID ID,
                                 IntegerType *Ty, Value *A) {
  return B.CreateCall(getIntrinsic(B, ID, Ty), {A});
}

template <typename VecT>
static bool popValue(VecT &Stack, typename VecT::value_type &Out) {
  if (Stack.empty())
    return false;
  Out = Stack.back();
  Stack.pop_back();
  return true;
}

static Value *emitRotateRight(IRBuilder<> &B, IntegerType *Ty, Value *X,
                              unsigned Amount) {
  unsigned BW = Ty->getBitWidth();
  Amount %= BW;
  if (Amount == 0)
    return X;
  Value *Lo = B.CreateLShr(X, loConst(Ty, Amount));
  Value *Hi = B.CreateShl(X, loConst(Ty, BW - Amount));
  return B.CreateOr(Lo, Hi, "vm.rel.rot");
}

static YMBA::Relation emitBuiltinRelation(IRBuilder<> &B, IntegerType *Ty,
                                          Value *X, Value *Y, unsigned Variant,
                                          uint64_t Seed) {
  switch (Variant % 3) {
  case 0: {
    Value *A = (Variant & 8)
                   ? emitRotateRight(B, Ty, X,
                                     1 + ((Seed >> 21) % (Ty->getBitWidth() - 1)))
                   : X;
    Value *C = (Variant & 16)
                   ? emitRotateRight(B, Ty, Y,
                                     1 + ((Seed >> 29) % (Ty->getBitWidth() - 1)))
                   : Y;
    Value *PA = B.CreateUnaryIntrinsic(Intrinsic::ctpop, A);
    Value *PC = B.CreateUnaryIntrinsic(Intrinsic::ctpop, C);
    Value *PXor = B.CreateUnaryIntrinsic(
        Intrinsic::ctpop, B.CreateXor(A, C, "vm.rel.popcarry.xor"));
    Value *PAnd = B.CreateUnaryIntrinsic(
        Intrinsic::ctpop, B.CreateAnd(A, C, "vm.rel.popcarry.and"));
    Value *L = B.CreateAdd(PA, PC, "vm.rel.lhs");
    Value *R = B.CreateAdd(PXor, B.CreateShl(PAnd, loConst(Ty, 1)),
                           "vm.rel.rhs");
    return {L, R};
  }
  case 1: {
    Value *Mask = loConst(Ty, yanso_mix64(Seed, 0x8c3d37c819544da2ULL));
    Value *A = (Variant & 8)
                   ? emitRotateRight(B, Ty, X,
                                     1 + ((Seed >> 17) % (Ty->getBitWidth() - 1)))
                   : X;
    Value *L = A;
    Value *R = B.CreateOr(B.CreateAnd(A, Mask, "vm.rel.maskpart.lo"),
                          B.CreateAnd(A, B.CreateNot(Mask),
                                      "vm.rel.maskpart.hi"),
                          "vm.rel.maskpart.rhs");
    return {L, R};
  }
  default: {
    uint64_t AConst = yanso_mix64(Seed, 0xd1b54a32d192ed03ULL) | 1ULL;
    uint64_t CConst = yanso_mix64(Seed, 0x94d049bb133111ebULL);
    Value *A = loConst(Ty, AConst);
    Value *AInv = ConstantInt::get(
        Ty, yanso_mod_inverse(APInt(Ty->getBitWidth(), AConst, false, true)));
    Value *C = loConst(Ty, CConst);
    Value *Base = (Variant & 8) ? X : B.CreateXor(X, Y, "vm.rel.aff.base");
    Value *Enc = B.CreateAdd(B.CreateMul(Base, A, "vm.rel.aff.mul"), C,
                             "vm.rel.aff.enc");
    Value *Dec = B.CreateMul(B.CreateSub(Enc, C, "vm.rel.aff.sub"), AInv,
                             "vm.rel.aff.dec");
    return {Base, Dec};
  }
  }
}

static unsigned builtinRelationCost(unsigned Variant) {
  switch (Variant % 3) {
  case 0:
    return 6;
  case 1:
    return (Variant & 8) ? 5 : 3;
  default:
    return 5;
  }
}

class Catalog {
  ArrayRef<uint8_t> Data;
  Header H;
  uint32_t ExprOff = 0;
  uint32_t RewriteOff = 0;
  uint32_t RelationOff = 0;
  uint32_t BytecodeOff = 0;
  SmallVector<ExprRecord, 32> Exprs;
  SmallVector<RewriteRecord, 32> Rewrites;
  SmallVector<RelationRecord, 32> Relations;
  bool Valid = false;

public:
  explicit Catalog(ArrayRef<uint8_t> Bytes) : Data(Bytes) { parse(); }

  bool isValid() const { return Valid; }

  unsigned rewriteCount(uint16_t Opcode, unsigned BitWidth) const {
    if (!Valid)
      return 0;
    uint16_t WM = widthMaskFor(BitWidth);
    if (!WM)
      return 0;
    return std::count_if(Rewrites.begin(), Rewrites.end(), [&](const auto &R) {
      return R.Opcode == Opcode && (R.WidthMask & WM) &&
             R.Expr < Exprs.size() && (Exprs[R.Expr].WidthMask & WM);
    });
  }

  const RewriteRecord *selectRewrite(uint16_t Opcode, unsigned BitWidth,
                                     unsigned Index) const {
    if (!Valid)
      return nullptr;
    uint16_t WM = widthMaskFor(BitWidth);
    if (!WM)
      return nullptr;
    SmallVector<const RewriteRecord *, 16> Bucket;
    for (const RewriteRecord &R : Rewrites) {
      if (R.Opcode == Opcode && (R.WidthMask & WM) && R.Expr < Exprs.size() &&
          (Exprs[R.Expr].WidthMask & WM))
        Bucket.push_back(&R);
    }
    if (Bucket.empty())
      return nullptr;
    return Bucket[Index % Bucket.size()];
  }

  void collectRewrites(uint16_t Opcode, unsigned BitWidth,
                       SmallVectorImpl<const RewriteRecord *> &Bucket) const {
    if (!Valid)
      return;
    uint16_t WM = widthMaskFor(BitWidth);
    if (!WM)
      return;
    for (const RewriteRecord &R : Rewrites)
      if (R.Opcode == Opcode && (R.WidthMask & WM) && R.Expr < Exprs.size() &&
          (Exprs[R.Expr].WidthMask & WM))
        Bucket.push_back(&R);
  }

  unsigned exprCost(uint32_t ExprIndex) const {
    if (!Valid || ExprIndex >= Exprs.size())
      return 0;
    return Exprs[ExprIndex].Cost;
  }

  void collectRelations(unsigned BitWidth,
                        SmallVectorImpl<const RelationRecord *> &Bucket) const {
    if (!Valid)
      return;
    uint16_t WM = widthMaskFor(BitWidth);
    if (!WM)
      return;
    for (const RelationRecord &R : Relations)
      if ((R.WidthMask & WM) && R.LExpr < Exprs.size() &&
          R.RExpr < Exprs.size() && (Exprs[R.LExpr].WidthMask & WM) &&
          (Exprs[R.RExpr].WidthMask & WM))
        Bucket.push_back(&R);
  }

  Value *emitExpr(IRBuilder<> &B, IntegerType *Ty, Value *X, Value *Y,
                  uint32_t ExprIndex) const {
    if (!Valid || ExprIndex >= Exprs.size())
      return nullptr;
    const ExprRecord &E = Exprs[ExprIndex];
    if (!(E.WidthMask & widthMaskFor(Ty->getBitWidth())))
      return nullptr;
    if (!rangeInBounds(E.BytecodeOff, E.BytecodeLen, H.BytecodeSize))
      return nullptr;

    ArrayRef<uint8_t> BC(Data.data() + BytecodeOff + E.BytecodeOff,
                         E.BytecodeLen);
    SmallVector<Value *, 16> Stack;
    uint32_t Pos = 0;
    auto Pop = [&]() -> Value * {
      Value *V = nullptr;
      return popValue(Stack, V) ? V : nullptr;
    };
    auto Pop2 = [&]() -> std::optional<std::pair<Value *, Value *>> {
      Value *R = nullptr;
      Value *L = nullptr;
      if (!popValue(Stack, R) || !popValue(Stack, L))
        return std::nullopt;
      return std::make_pair(L, R);
    };

    while (Pos < BC.size()) {
      uint8_t Op = BC[Pos++];
      switch (Op) {
      case BC_VAR_X:
        Stack.push_back(X);
        break;
      case BC_VAR_Y:
        Stack.push_back(Y);
        break;
      case BC_CONST_U: {
        auto Imm = readULEB(BC, Pos);
        if (!Imm)
          return nullptr;
        Stack.push_back(
            ConstantInt::get(Ty, Imm->zextOrTrunc(Ty->getBitWidth())));
        break;
      }
      case BC_CTPOP:
      case BC_BSWAP:
      case BC_BITREVERSE: {
        Value *A = Pop();
        if (!A)
          return nullptr;
        switch (Op) {
        case BC_CTPOP:
          Stack.push_back(emitUnaryIntrinsic(B, Intrinsic::ctpop, Ty, A));
          break;
        case BC_BSWAP:
          Stack.push_back(emitUnaryIntrinsic(B, Intrinsic::bswap, Ty, A));
          break;
        case BC_BITREVERSE:
          Stack.push_back(emitUnaryIntrinsic(B, Intrinsic::bitreverse, Ty, A));
          break;
        }
        break;
      }
      case BC_ADD:
      case BC_SUB:
      case BC_MUL:
      case BC_AND:
      case BC_OR:
      case BC_XOR:
      case BC_SHL:
      case BC_LSHR:
      case BC_ASHR:
      case BC_ROTL:
      case BC_ROTR: {
        auto P = Pop2();
        if (!P)
          return nullptr;
        Value *L = P->first;
        Value *R = P->second;
        switch (Op) {
        case BC_ADD:
          Stack.push_back(B.CreateAdd(L, R, "ymba.add"));
          break;
        case BC_SUB:
          Stack.push_back(B.CreateSub(L, R, "ymba.sub"));
          break;
        case BC_MUL:
          Stack.push_back(B.CreateMul(L, R, "ymba.mul"));
          break;
        case BC_AND:
          Stack.push_back(B.CreateAnd(L, R, "ymba.and"));
          break;
        case BC_OR:
          Stack.push_back(B.CreateOr(L, R, "ymba.or"));
          break;
        case BC_XOR:
          Stack.push_back(B.CreateXor(L, R, "ymba.xor"));
          break;
        case BC_SHL:
          Stack.push_back(B.CreateShl(L, R, "ymba.shl"));
          break;
        case BC_LSHR:
          Stack.push_back(B.CreateLShr(L, R, "ymba.lshr"));
          break;
        case BC_ASHR:
          Stack.push_back(B.CreateAShr(L, R, "ymba.ashr"));
          break;
        case BC_ROTL:
          Stack.push_back(emitRotateIntrinsic(B, Intrinsic::fshl, Ty, L, R));
          break;
        case BC_ROTR:
          Stack.push_back(emitRotateIntrinsic(B, Intrinsic::fshr, Ty, L, R));
          break;
        }
        break;
      }
      case BC_SHL_IMM:
      case BC_LSHR_IMM:
      case BC_ASHR_IMM:
      case BC_ROTL_IMM:
      case BC_ROTR_IMM: {
        auto Imm = readULEB(BC, Pos);
        Value *A = Pop();
        if (!Imm || !A ||
            ((Op == BC_SHL_IMM || Op == BC_LSHR_IMM || Op == BC_ASHR_IMM) &&
             Imm->uge(Ty->getBitWidth())))
          return nullptr;
        Value *Amt = ConstantInt::get(Ty, Imm->zextOrTrunc(Ty->getBitWidth()));
        switch (Op) {
        case BC_SHL_IMM:
          Stack.push_back(B.CreateShl(A, Amt, "ymba.shl"));
          break;
        case BC_LSHR_IMM:
          Stack.push_back(B.CreateLShr(A, Amt, "ymba.lshr"));
          break;
        case BC_ASHR_IMM:
          Stack.push_back(B.CreateAShr(A, Amt, "ymba.ashr"));
          break;
        case BC_ROTL_IMM:
          Stack.push_back(emitRotateIntrinsic(B, Intrinsic::fshl, Ty, A, Amt));
          break;
        case BC_ROTR_IMM:
          Stack.push_back(emitRotateIntrinsic(B, Intrinsic::fshr, Ty, A, Amt));
          break;
        }
        break;
      }
      case BC_UDIV_CONST:
      case BC_UREM_CONST: {
        auto Imm = readULEB(BC, Pos);
        Value *A = Pop();
        if (!Imm || !A)
          return nullptr;
        APInt D = Imm->zextOrTrunc(Ty->getBitWidth());
        if (D.isZero())
          return nullptr;
        if (Op == BC_UDIV_CONST)
          Stack.push_back(
              B.CreateUDiv(A, ConstantInt::get(Ty, D), "ymba.udiv"));
        else
          Stack.push_back(
              B.CreateURem(A, ConstantInt::get(Ty, D), "ymba.urem"));
        break;
      }
      default:
        return nullptr;
      }
    }
    if (Stack.size() != 1)
      return nullptr;
    return Stack.back();
  }

private:
  bool verifyExprBytecode(const ExprRecord &E) const {
    if (!rangeInBounds(E.BytecodeOff, E.BytecodeLen, H.BytecodeSize))
      return false;
    ArrayRef<uint8_t> BC(Data.data() + BytecodeOff + E.BytecodeOff,
                         E.BytecodeLen);
    int Depth = 0;
    int MaxDepth = 0;
    uint32_t Pos = 0;
    while (Pos < BC.size()) {
      uint8_t Op = BC[Pos++];
      switch (Op) {
      case BC_VAR_X:
      case BC_VAR_Y:
        ++Depth;
        break;
      case BC_CONST_U:
        if (!readULEB(BC, Pos))
          return false;
        ++Depth;
        break;
      case BC_CTPOP:
      case BC_BSWAP:
      case BC_BITREVERSE:
        if (Depth < 1)
          return false;
        break;
      case BC_ADD:
      case BC_SUB:
      case BC_MUL:
      case BC_AND:
      case BC_OR:
      case BC_XOR:
      case BC_SHL:
      case BC_LSHR:
      case BC_ASHR:
      case BC_ROTL:
      case BC_ROTR:
        if (Depth < 2)
          return false;
        --Depth;
        break;
      case BC_SHL_IMM:
      case BC_LSHR_IMM:
      case BC_ASHR_IMM:
      case BC_ROTL_IMM:
      case BC_ROTR_IMM:
      case BC_UDIV_CONST:
      case BC_UREM_CONST:
        if (!readULEB(BC, Pos) || Depth < 1)
          return false;
        break;
      default:
        return false;
      }
      MaxDepth = std::max(MaxDepth, Depth);
    }
    return Depth == 1 && (E.MaxStack == 0 || MaxDepth <= E.MaxStack);
  }

  void parse() {
    if (Data.size() < 16)
      return;
    H.NumExprs = read32(Data, 0);
    H.NumRewrites = read32(Data, 4);
    H.NumRelations = read32(Data, 8);
    H.BytecodeSize = read32(Data, 12);

    ExprOff = HeaderSize;
    RewriteOff = ExprOff + H.NumExprs * ExprRecordSize;
    RelationOff = RewriteOff + H.NumRewrites * RewriteRecordSize;
    BytecodeOff = RelationOff + H.NumRelations * RelationRecordSize;

    if (!rangeInBounds(ExprOff, H.NumExprs * ExprRecordSize, Data.size()) ||
        !rangeInBounds(RewriteOff, H.NumRewrites * RewriteRecordSize,
                       Data.size()) ||
        !rangeInBounds(RelationOff, H.NumRelations * RelationRecordSize,
                       Data.size()) ||
        !rangeInBounds(BytecodeOff, H.BytecodeSize, Data.size()))
      return;

    for (uint32_t I = 0; I != H.NumExprs; ++I) {
      uint32_t O = ExprOff + I * ExprRecordSize;
      ExprRecord E;
      E.BytecodeOff = read32(Data, O + 0);
      E.BytecodeLen = read32(Data, O + 4);
      E.WidthMask = read16(Data, O + 8);
      E.VarMask = read16(Data, O + 10);
      E.Cost = read16(Data, O + 12);
      E.MaxStack = read16(Data, O + 14);
      if (!verifyExprBytecode(E))
        return;
      Exprs.push_back(E);
    }

    for (uint32_t I = 0; I != H.NumRewrites; ++I) {
      uint32_t O = RewriteOff + I * RewriteRecordSize;
      RewriteRecord R;
      R.Opcode = read16(Data, O + 0);
      R.WidthMask = read16(Data, O + 2);
      R.Expr = read32(Data, O + 4);
      if (R.Expr >= Exprs.size())
        return;
      Rewrites.push_back(R);
    }

    for (uint32_t I = 0; I != H.NumRelations; ++I) {
      uint32_t O = RelationOff + I * RelationRecordSize;
      RelationRecord R;
      R.LExpr = read32(Data, O + 0);
      R.RExpr = read32(Data, O + 4);
      R.WidthMask = read16(Data, O + 8);
      if (R.LExpr >= Exprs.size() || R.RExpr >= Exprs.size())
        return;
      Relations.push_back(R);
    }

    Valid = true;
  }
};

static const Catalog &catalog() {
  static const Catalog C(ArrayRef<uint8_t>(yanso_ymba_catalog::kCatalog,
                                           yanso_ymba_catalog::kCatalogSize));
  return C;
}

} // namespace

static double unitDouble(uint64_t X) {
  return static_cast<double>(X >> 11) * (1.0 / 9007199254740992.0);
}

static unsigned selectWeightedIndex(ArrayRef<unsigned> Costs, uint64_t Seed) {
  assert(!Costs.empty());
  double T = YMBACostTemperature;
  if (T <= 0.0) {
    unsigned Best = 0;
    for (unsigned I = 1, E = Costs.size(); I != E; ++I)
      if (Costs[I] < Costs[Best])
        Best = I;
    return Best;
  }

  double Total = 0.0;
  SmallVector<double, 16> Weights;
  Weights.reserve(Costs.size());
  for (unsigned C : Costs) {
    double W = std::exp(-static_cast<double>(C) / T);
    Weights.push_back(W);
    Total += W;
  }
  if (!(Total > 0.0))
    return yanso_mix64(Seed, 0xd6e8feb86659fd93ULL) % Costs.size();

  double Pick = unitDouble(yanso_mix64(Seed, 0xa4093822299f31d0ULL)) * Total;
  for (unsigned I = 0, E = Weights.size(); I != E; ++I) {
    if (Pick < Weights[I])
      return I;
    Pick -= Weights[I];
  }
  return Weights.size() - 1;
}

static const RewriteRecord *selectWeightedRewrite(
    ArrayRef<const RewriteRecord *> Rewrites, ArrayRef<unsigned> BuiltinCosts,
    uint64_t Seed, unsigned &BuiltinIndex) {
  SmallVector<unsigned, 32> Costs;
  Costs.reserve(Rewrites.size() + BuiltinCosts.size());
  for (const RewriteRecord *R : Rewrites)
    Costs.push_back(std::max(1U, catalog().exprCost(R->Expr)));
  Costs.append(BuiltinCosts.begin(), BuiltinCosts.end());

  unsigned Pick = selectWeightedIndex(Costs, Seed);
  if (Pick < Rewrites.size())
    return Rewrites[Pick];
  BuiltinIndex = Pick - Rewrites.size();
  return nullptr;
}

static const RelationRecord *selectWeightedRelation(
    ArrayRef<const RelationRecord *> Relations, ArrayRef<unsigned> BuiltinCosts,
    uint64_t Seed, unsigned &BuiltinIndex) {
  SmallVector<unsigned, 32> Costs;
  Costs.reserve(Relations.size() + BuiltinCosts.size());
  for (const RelationRecord *R : Relations) {
    unsigned LCost = catalog().exprCost(R->LExpr);
    unsigned RCost = catalog().exprCost(R->RExpr);
    Costs.push_back(std::max(1U, LCost + RCost));
  }
  Costs.append(BuiltinCosts.begin(), BuiltinCosts.end());

  unsigned Pick = selectWeightedIndex(Costs, Seed);
  if (Pick < Relations.size())
    return Relations[Pick];
  BuiltinIndex = Pick - Relations.size();
  return nullptr;
}


bool YMBA::isSupportedBinaryOpcode(unsigned LLVMOpcode) {
  switch (LLVMOpcode) {
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

unsigned YMBA::rewriteCount(unsigned LLVMOpcode, unsigned BitWidth) {
  auto Op = toYMBAOpcode(LLVMOpcode);
  if (!Op)
    return 0;
  return catalog().rewriteCount(*Op, BitWidth);
}

Value *YMBA::emitRewrite(IRBuilder<> &B, unsigned LLVMOpcode, IntegerType *Ty,
                         Value *X, Value *Y, unsigned Index, uint64_t) {
  auto Op = toYMBAOpcode(LLVMOpcode);
  if (!Op)
    return nullptr;
  const RewriteRecord *R =
      catalog().selectRewrite(*Op, Ty->getBitWidth(), Index);
  if (!R)
    return nullptr;
  return catalog().emitExpr(B, Ty, X, Y, R->Expr);
}

Value *YMBA::emitBinary(IRBuilder<> &B, unsigned LLVMOpcode, IntegerType *Ty,
                        Value *X, Value *Y, unsigned Variant, uint64_t Seed) {
  auto Op = toYMBAOpcode(LLVMOpcode);
  SmallVector<const RewriteRecord *, 16> Rewrites;
  if (Op)
    catalog().collectRewrites(*Op, Ty->getBitWidth(), Rewrites);
  SmallVector<unsigned, 8> BuiltinCosts;
  collectBuiltinBinaryCosts(LLVMOpcode, BuiltinCosts);

  unsigned BuiltinIndex = Variant;
  if (!BuiltinCosts.empty()) {
    if (const RewriteRecord *R =
            selectWeightedRewrite(Rewrites, BuiltinCosts, Seed, BuiltinIndex))
      if (Value *V = catalog().emitExpr(B, Ty, X, Y, R->Expr))
        return V;
  }

  return emitBuiltinBinary(B, LLVMOpcode, Ty, X, Y, BuiltinIndex, Seed);
}

static bool isSupportedIntrinsicRewriteShape(Intrinsic::ID ID,
                                             ArrayRef<Value *> Args) {
  switch (ID) {
  case Intrinsic::fshl:
  case Intrinsic::fshr:
    return Args.size() >= 3 && Args[0] == Args[1];
  case Intrinsic::ctpop:
  case Intrinsic::bswap:
  case Intrinsic::bitreverse:
    return Args.size() >= 1;
  default:
    return false;
  }
}

Value *YMBA::emitIntrinsic(IRBuilder<> &B, Intrinsic::ID ID, IntegerType *Ty,
                           ArrayRef<Value *> Args, unsigned Variant,
                           uint64_t Seed) {
  auto Op = intrinsicToYMBAOpcode(ID);
  SmallVector<const RewriteRecord *, 16> Rewrites;
  if (Op && isSupportedIntrinsicRewriteShape(ID, Args))
    catalog().collectRewrites(*Op, Ty->getBitWidth(), Rewrites);
  unsigned BuiltinIndex = Variant;
  if (!Rewrites.empty()) {
    unsigned SelectedBuiltin = 0;
    if (const RewriteRecord *R =
            selectWeightedRewrite(Rewrites, ArrayRef<unsigned>(1U),
                                  Seed, SelectedBuiltin)) {
      Value *X = Args.empty() ? ConstantInt::get(Ty, 0) : Args[0];
      Value *Y = Args.size() >= 2 ? Args[1] : ConstantInt::get(Ty, 0);
      // fshl/fshr have three LLVM operands. YMBA models rotate amount as Y.
      if ((ID == Intrinsic::fshl || ID == Intrinsic::fshr) && Args.size() >= 3)
        Y = Args[2];
      if (Value *V = catalog().emitExpr(B, Ty, X, Y, R->Expr))
        return V;
    }
    BuiltinIndex = SelectedBuiltin;
  }

  FunctionCallee Intr = Intrinsic::getOrInsertDeclaration(
      B.GetInsertBlock()->getModule(), ID, {Ty});
  Value *R = B.CreateCall(Intr, Args, "vm.intr.core");
  return decorateIntegerResult(B, Ty, R, BuiltinIndex, Seed, "vm.intr.out");
}


YMBA::Relation YMBA::emitRelation(IRBuilder<> &B, IntegerType *Ty, Value *X,
                                  Value *Y, unsigned Index, uint64_t Seed) {
  SmallVector<const RelationRecord *, 32> Relations;
  catalog().collectRelations(Ty->getBitWidth(), Relations);
  SmallVector<unsigned, 8> BuiltinCosts;
  for (unsigned I = 0; I != 3; ++I)
    BuiltinCosts.push_back(builtinRelationCost(I));

  unsigned BuiltinIndex = Index;
  if (const RelationRecord *R =
          selectWeightedRelation(Relations, BuiltinCosts, Seed, BuiltinIndex)) {
    Value *L = catalog().emitExpr(B, Ty, X, Y, R->LExpr);
    Value *RV = catalog().emitExpr(B, Ty, X, Y, R->RExpr);
    if (L && RV)
      return {L, RV};
  }

  return emitBuiltinRelation(B, Ty, X, Y, BuiltinIndex, Seed);
}
