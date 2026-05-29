#include "CryptoUtils.h"

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
  __uint128_t R = static_cast<__uint128_t>(A) * B;
  B += static_cast<uint64_t>(R >> 64) + YansoMixAddend;
  return B ^ static_cast<uint64_t>(R);
}

uint64_t yanso_hash_string(StringRef S, uint64_t Seed) {
  uint64_t H = Seed;
  for (unsigned char C : S.bytes())
    H = yanso_mix64(static_cast<uint64_t>(C) + 1, H);
  H = yanso_mix64(static_cast<uint64_t>(S.size()), H);
  return H;
}

Value *yanso_create_mix64_ir(Value *A, Value *B, BasicBlock *InsertAtEnd,
                             Module &M) {
  LLVMContext &Ctx = M.getContext();
  IntegerType *I64Ty = Type::getInt64Ty(Ctx);
  IntegerType *I128Ty = IntegerType::get(Ctx, 128);
  FunctionCallee FunnelShiftLeft =
      Intrinsic::getOrInsertDeclaration(&M, Intrinsic::fshl, {I64Ty});
  Value *Sum = BinaryOperator::CreateAdd(B, A, "", InsertAtEnd);
  Value *Rot = CallInst::Create(
      FunnelShiftLeft, {Sum, Sum, ConstantInt::get(I64Ty, 64 - YansoMixRotate)},
      "", InsertAtEnd);
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
