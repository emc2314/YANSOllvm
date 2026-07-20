#include "CryptoUtils.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace llvm {

static uint64_t rotr64(uint64_t V, unsigned Amount) {
  return (V >> Amount) | (V << (64 - Amount));
}

uint64_t yanso_mix64(uint64_t A, uint64_t B) {
  A ^= rotr64(A + B, YansoMixRotate);
  APInt R = APInt(64, A).zext(128) * APInt(64, B).zext(128);
  B += R.extractBitsAsZExtValue(64, 64) + YansoMixAddend;
  return B ^ R.extractBitsAsZExtValue(64, 0);
}

uint64_t yanso_hash_string(StringRef S, uint64_t Seed) {
  uint64_t H = yanso_mix64(Seed, YansoMixBasis);
  for (unsigned char C : S.bytes())
    H = yanso_mix64(static_cast<uint64_t>(C) + 1, H);
  H = yanso_mix64(static_cast<uint64_t>(S.size()), H);
  return H;
}

uint64_t yanso_mod_inverse(uint64_t A) {
  uint64_t X = A;
  for (unsigned I = 0; I != 6; ++I)
    X *= 2 - A * X;
  return X;
}

APInt yanso_mod_inverse(const APInt &A) {
  unsigned BW = A.getBitWidth();
  APInt X = A;
  for (unsigned Bits = 1; Bits < BW; Bits <<= 1)
    X *= APInt(BW, 2) - A * X;
  return X;
}

uint64_t yanso_mod_inverse(uint64_t A, uint64_t Modulus) {
  if (Modulus == 0)
    return 0;
  APInt Inv = yanso_mod_inverse(APInt(65, A), APInt(65, Modulus));
  return Inv.getZExtValue();
}

APInt yanso_mod_inverse(const APInt &A, const APInt &Modulus) {
  if (Modulus.isZero())
    return APInt(Modulus.getBitWidth(), 0);

  unsigned InputBW = std::max(A.getBitWidth(), Modulus.getBitWidth());
  unsigned WorkBW = InputBW * 2 + 2;
  APInt M = Modulus.zextOrTrunc(WorkBW);
  APInt R = M;
  APInt NewR = A.zextOrTrunc(WorkBW).urem(M);
  APInt T(WorkBW, 0);
  APInt NewT(WorkBW, 1);

  while (!NewR.isZero()) {
    APInt Q = R.udiv(NewR);
    APInt NextT = T - Q * NewT;
    APInt NextR = R - Q * NewR;
    T = NewT;
    NewT = NextT;
    R = NewR;
    NewR = NextR;
  }

  if (R != APInt(WorkBW, 1))
    return APInt(Modulus.getBitWidth(), 0);
  if (T.isNegative())
    T += M;
  return T.urem(M).trunc(Modulus.getBitWidth());
}

Value *yanso_create_mix64_ir(Value *A, Value *B, BasicBlock *InsertAtEnd,
                             Module &M) {
  LLVMContext &Ctx = M.getContext();
  IntegerType *I64Ty = Type::getInt64Ty(Ctx);
  IntegerType *I128Ty = IntegerType::get(Ctx, 128);
  Value *Sum = BinaryOperator::CreateAdd(B, A, "", InsertAtEnd);
  Value *RotRight = BinaryOperator::CreateLShr(
      Sum, ConstantInt::get(I64Ty, YansoMixRotate), "", InsertAtEnd);
  Value *RotLeft = BinaryOperator::CreateShl(
      Sum, ConstantInt::get(I64Ty, 64 - YansoMixRotate), "", InsertAtEnd);
  Value *Rot = BinaryOperator::CreateOr(RotRight, RotLeft, "", InsertAtEnd);
  A = BinaryOperator::CreateXor(Rot, A, "", InsertAtEnd);

  Value *WideA = new ZExtInst(A, I128Ty, "", InsertAtEnd);
  Value *WideB = new ZExtInst(B, I128Ty, "", InsertAtEnd);
  Value *WideR = BinaryOperator::CreateMul(WideA, WideB, "", InsertAtEnd);
  Value *HighWide = BinaryOperator::CreateLShr(
      WideR, ConstantInt::get(I128Ty, 64), "", InsertAtEnd);
  Value *High = new TruncInst(HighWide, I64Ty, "", InsertAtEnd);
  Value *MixedB = BinaryOperator::CreateAdd(
      B, ConstantInt::get(I64Ty, YansoMixAddend), "", InsertAtEnd);
  MixedB = BinaryOperator::CreateAdd(MixedB, High, "", InsertAtEnd);
  Value *Low = new TruncInst(WideR, I64Ty, "", InsertAtEnd);
  return BinaryOperator::CreateXor(MixedB, Low, "", InsertAtEnd);
}

uint32_t YansoRNG::range(uint32_t Max) {
  if (Max == 0)
    return 0;
  uint32_t Limit = UINT32_MAX - (UINT32_MAX % Max);
  uint32_t V;
  do {
    V = next32();
  } while (V >= Limit);
  return V % Max;
}

void YansoRNG::fill_bytes(char *Buffer, size_t Len) {
  size_t Off = 0;
  while (Off < Len) {
    uint64_t V = next64();
    for (unsigned I = 0; I < 8 && Off < Len; ++I, ++Off)
      Buffer[Off] = static_cast<char>((V >> (I * 8)) & 0xFF);
  }
}

} // namespace llvm
