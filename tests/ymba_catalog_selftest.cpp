#include "YMBA.h"

#include "GeneratedYMBACatalog.inc"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {
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

int fail(const char *Msg) {
  std::cerr << "YMBA catalog self-test failed: " << Msg << "\n";
  return 1;
}

std::string hex(const APInt &V) {
  SmallString<40> S;
  V.toString(S, 16, false);
  return std::string(S.str());
}

uint16_t read16(uint32_t Off) {
  const uint8_t *Data = yanso_ymba_catalog::kCatalog;
  return uint16_t(Data[Off]) | (uint16_t(Data[Off + 1]) << 8);
}

uint32_t read32(uint32_t Off) {
  const uint8_t *Data = yanso_ymba_catalog::kCatalog;
  return uint32_t(Data[Off]) | (uint32_t(Data[Off + 1]) << 8) |
         (uint32_t(Data[Off + 2]) << 16) | (uint32_t(Data[Off + 3]) << 24);
}

uint16_t widthMaskFor(unsigned BW) {
  switch (BW) {
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

std::vector<unsigned> minMaxWidths(uint16_t WidthMask) {
  constexpr std::array<unsigned, 5> AllWidths = {8, 16, 32, 64, 128};
  std::vector<unsigned> Result;
  for (unsigned BW : AllWidths) {
    if (WidthMask & widthMaskFor(BW))
      Result.push_back(BW);
  }
  if (Result.size() > 2)
    Result = {Result.front(), Result.back()};
  return Result;
}

APInt expected(uint16_t Opcode, const APInt &X, const APInt &Y) {
  unsigned BW = X.getBitWidth();
  APInt WidthValue(BW, BW, false, true);
  switch (Opcode) {
  case YMBA::YMBA_OP_ADD:
    return X + Y;
  case YMBA::YMBA_OP_SUB:
    return X - Y;
  case YMBA::YMBA_OP_MUL:
    return X * Y;
  case YMBA::YMBA_OP_AND:
    return X & Y;
  case YMBA::YMBA_OP_OR:
    return X | Y;
  case YMBA::YMBA_OP_XOR:
    return X ^ Y;
  case YMBA::YMBA_OP_SHL:
    return Y.uge(WidthValue) ? APInt(BW, 0) : X.shl(unsigned(Y.getZExtValue()));
  case YMBA::YMBA_OP_LSHR:
    return Y.uge(WidthValue) ? APInt(BW, 0)
                             : X.lshr(unsigned(Y.getZExtValue()));
  case YMBA::YMBA_OP_ASHR:
    return Y.uge(WidthValue) ? APInt(BW, 0)
                             : X.ashr(unsigned(Y.getZExtValue()));
  case YMBA::YMBA_OP_ROTL:
    return X.rotl(unsigned(Y.urem(WidthValue).getZExtValue()));
  case YMBA::YMBA_OP_ROTR:
    return X.rotr(unsigned(Y.urem(WidthValue).getZExtValue()));
  case YMBA::YMBA_OP_CTPOP:
    return APInt(BW, X.popcount());
  case YMBA::YMBA_OP_BSWAP:
    return X.byteSwap();
  case YMBA::YMBA_OP_BITREVERSE:
    return X.reverseBits();
  default:
    std::cerr << "unknown rewrite opcode " << Opcode << "\n";
    std::exit(1);
  }
}

std::optional<unsigned> llvmOpcodeForYMBA(uint16_t Opcode) {
  switch (Opcode) {
  case YMBA::YMBA_OP_ADD:
    return BinaryOperator::Add;
  case YMBA::YMBA_OP_SUB:
    return BinaryOperator::Sub;
  case YMBA::YMBA_OP_MUL:
    return BinaryOperator::Mul;
  case YMBA::YMBA_OP_AND:
    return BinaryOperator::And;
  case YMBA::YMBA_OP_OR:
    return BinaryOperator::Or;
  case YMBA::YMBA_OP_XOR:
    return BinaryOperator::Xor;
  case YMBA::YMBA_OP_SHL:
    return BinaryOperator::Shl;
  case YMBA::YMBA_OP_LSHR:
    return BinaryOperator::LShr;
  case YMBA::YMBA_OP_ASHR:
    return BinaryOperator::AShr;
  default:
    return std::nullopt;
  }
}

std::vector<std::pair<APInt, APInt>> makeSamples(unsigned BW,
                                                 std::mt19937_64 &RNG) {
  APInt Mask = APInt::getAllOnes(BW);
  APInt AltA(BW, 0);
  APInt AltB(BW, 0);
  for (unsigned I = 0; I != BW; ++I) {
    if ((I & 1) == 0)
      AltA.setBit(I);
    else
      AltB.setBit(I);
  }

  std::vector<std::pair<APInt, APInt>> Samples = {
      {APInt(BW, 0), APInt(BW, 0)},
      {APInt(BW, 0), APInt(BW, 1)},
      {APInt(BW, 1), APInt(BW, 0)},
      {Mask, APInt(BW, 0)},
      {APInt(BW, 0), Mask},
      {Mask, Mask},
      {AltA, AltB},
      {APInt(BW, 0x123456789abcdef0ULL, false, true),
       APInt(BW, 0xfedcba9876543210ULL, false, true)}};
  for (unsigned I = 0; I != 128; ++I) {
    APInt X(BW, RNG(), false, true);
    APInt Y(BW, RNG(), false, true);
    if (BW > 64) {
      X.insertBits(APInt(64, RNG()), 64);
      Y.insertBits(APInt(64, RNG()), 64);
    }
    Samples.push_back({X, Y});
  }
  return Samples;
}

std::optional<APInt> evalIR(Value *V, unsigned BW) {
  if (auto *CI = dyn_cast<ConstantInt>(V))
    return CI->getValue();

  if (auto *BO = dyn_cast<BinaryOperator>(V)) {
    auto L = evalIR(BO->getOperand(0), BW);
    auto R = evalIR(BO->getOperand(1), BW);
    if (!L || !R)
      return std::nullopt;
    APInt WidthValue(BW, BW, false, true);
    switch (BO->getOpcode()) {
    case Instruction::Add:
      return *L + *R;
    case Instruction::Sub:
      return *L - *R;
    case Instruction::Mul:
      return *L * *R;
    case Instruction::And:
      return *L & *R;
    case Instruction::Or:
      return *L | *R;
    case Instruction::Xor:
      return *L ^ *R;
    case Instruction::Shl:
      if (R->uge(WidthValue))
        return std::nullopt;
      return L->shl(unsigned(R->getZExtValue()));
    case Instruction::LShr:
      if (R->uge(WidthValue))
        return std::nullopt;
      return L->lshr(unsigned(R->getZExtValue()));
    case Instruction::AShr:
      if (R->uge(WidthValue))
        return std::nullopt;
      return L->ashr(unsigned(R->getZExtValue()));
    case Instruction::UDiv:
      if (R->isZero())
        return std::nullopt;
      return L->udiv(*R);
    case Instruction::URem:
      if (R->isZero())
        return std::nullopt;
      return L->urem(*R);
    default:
      return std::nullopt;
    }
  }

  if (auto *II = dyn_cast<IntrinsicInst>(V)) {
    Intrinsic::ID ID = II->getIntrinsicID();
    if (ID == Intrinsic::ctpop || ID == Intrinsic::bswap ||
        ID == Intrinsic::bitreverse) {
      auto A = evalIR(II->getArgOperand(0), BW);
      if (!A)
        return std::nullopt;
      if (ID == Intrinsic::ctpop)
        return APInt(BW, A->popcount());
      if (ID == Intrinsic::bswap)
        return A->byteSwap();
      return A->reverseBits();
    }
    if (ID == Intrinsic::fshl || ID == Intrinsic::fshr) {
      auto L = evalIR(II->getArgOperand(0), BW);
      auto R = evalIR(II->getArgOperand(1), BW);
      auto Amt = evalIR(II->getArgOperand(2), BW);
      if (!L || !R || !Amt || *L != *R)
        return std::nullopt;
      APInt WidthValue(BW, BW, false, true);
      unsigned Shift = unsigned(Amt->urem(WidthValue).getZExtValue());
      return ID == Intrinsic::fshl ? L->rotl(Shift) : L->rotr(Shift);
    }
  }

  return std::nullopt;
}

std::optional<APInt> emitAndEvalRewrite(unsigned Opcode, unsigned BW,
                                        const APInt &X, const APInt &Y,
                                        unsigned Index) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("ymba.selftest", Ctx);
  IRBuilder<> B(Ctx);
  IntegerType *Ty = IntegerType::get(Ctx, BW);
  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *F = Function::Create(FT, Function::ExternalLinkage, "f", M.get());
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  Value *Got = YMBA::emitRewrite(B, Opcode, Ty, ConstantInt::get(Ty, X),
                                 ConstantInt::get(Ty, Y), Index, 0);
  if (!Got)
    return std::nullopt;
  B.CreateRetVoid();
  if (verifyFunction(*F, &errs()))
    return std::nullopt;
  return evalIR(Got, BW);
}

std::optional<std::pair<APInt, APInt>> emitAndEvalRelation(unsigned BW,
                                                           const APInt &X,
                                                           const APInt &Y,
                                                           unsigned Index) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("ymba.selftest", Ctx);
  IRBuilder<> B(Ctx);
  IntegerType *Ty = IntegerType::get(Ctx, BW);
  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *F = Function::Create(FT, Function::ExternalLinkage, "f", M.get());
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  YMBA::Relation R = YMBA::emitRelation(B, Ty, ConstantInt::get(Ty, X),
                                        ConstantInt::get(Ty, Y), Index, 0);
  if (!R.L || !R.R)
    return std::nullopt;
  B.CreateRetVoid();
  if (verifyFunction(*F, &errs()))
    return std::nullopt;
  auto L = evalIR(R.L, BW);
  auto RV = evalIR(R.R, BW);
  if (!L || !RV)
    return std::nullopt;
  return std::make_pair(*L, *RV);
}

std::vector<RewriteRecord> readRewrites() {
  const uint32_t NumExprs = read32(0);
  const uint32_t NumRewrites = read32(4);
  const uint32_t RewriteOff = 16 + NumExprs * 16;
  std::vector<RewriteRecord> Rewrites;
  for (uint32_t I = 0; I != NumRewrites; ++I) {
    uint32_t O = RewriteOff + I * 8;
    Rewrites.push_back({read16(O + 0), read16(O + 2), read32(O + 4)});
  }
  return Rewrites;
}

std::vector<RelationRecord> readRelations() {
  const uint32_t NumExprs = read32(0);
  const uint32_t NumRewrites = read32(4);
  const uint32_t NumRelations = read32(8);
  const uint32_t RelationOff = 16 + NumExprs * 16 + NumRewrites * 8;
  std::vector<RelationRecord> Relations;
  for (uint32_t I = 0; I != NumRelations; ++I) {
    uint32_t O = RelationOff + I * 10;
    Relations.push_back({read32(O + 0), read32(O + 4), read16(O + 8)});
  }
  return Relations;
}

unsigned rewriteIndexForWidth(ArrayRef<RewriteRecord> Rewrites,
                              unsigned RecordIndex, uint16_t Opcode,
                              unsigned BW) {
  uint16_t WM = widthMaskFor(BW);
  unsigned Index = 0;
  for (unsigned I = 0; I != RecordIndex; ++I) {
    if (Rewrites[I].Opcode == Opcode && (Rewrites[I].WidthMask & WM))
      ++Index;
  }
  return Index;
}

} // namespace

int main() {
  std::vector<RewriteRecord> Rewrites = readRewrites();
  std::vector<RelationRecord> Relations = readRelations();
  unsigned RewriteChecks = 0;
  unsigned RelationChecks = 0;
  std::random_device RD;
  std::mt19937_64 RNG(RD());

  for (unsigned RecordIndex = 0; RecordIndex != Rewrites.size();
       ++RecordIndex) {
    const RewriteRecord &R = Rewrites[RecordIndex];
    auto LLVMOp = llvmOpcodeForYMBA(R.Opcode);
    if (!LLVMOp)
      return fail("rewrite opcode has no public LLVM opcode mapping");
    std::vector<unsigned> Widths = minMaxWidths(R.WidthMask);
    if (Widths.empty())
      return fail("rewrite has empty width mask");
    for (unsigned BW : Widths) {
      auto Samples = makeSamples(BW, RNG);
      unsigned Count = YMBA::rewriteCount(*LLVMOp, BW);
      unsigned PublicIndex =
          rewriteIndexForWidth(Rewrites, RecordIndex, R.Opcode, BW);
      if (PublicIndex >= Count) {
        std::cerr << "rewrite index unavailable through YMBA public API:"
                  << " record=" << RecordIndex << " opcode=" << R.Opcode
                  << " bw=" << BW << " public_index=" << PublicIndex
                  << " count=" << Count << "\n";
        return 1;
      }
      for (auto &[X, Y] : Samples) {
        auto Got = emitAndEvalRewrite(*LLVMOp, BW, X, Y, PublicIndex);
        if (!Got)
          return fail("rewrite IR eval failed");
        APInt Expect = expected(R.Opcode, X, Y);
        if (*Got != Expect) {
          std::cerr << "rewrite mismatch opcode=" << R.Opcode << " bw=" << BW
                    << " index=" << PublicIndex << " x=" << hex(X)
                    << " y=" << hex(Y) << " got=" << hex(*Got)
                    << " expect=" << hex(Expect) << "\n";
          return 1;
        }
        ++RewriteChecks;
      }
    }
  }

  for (unsigned Index = 0; Index != Relations.size(); ++Index) {
    const RelationRecord &R = Relations[Index];
    std::vector<unsigned> Widths = minMaxWidths(R.WidthMask);
    if (Widths.empty())
      return fail("relation has empty width mask");
    for (unsigned BW : Widths) {
      auto Samples = makeSamples(BW, RNG);
      for (auto &[X, Y] : Samples) {
        auto Got = emitAndEvalRelation(BW, X, Y, Index);
        if (!Got)
          return fail("relation IR eval failed");
        if (Got->first != Got->second) {
          std::cerr << "relation mismatch bw=" << BW << " index=" << Index
                    << " x=" << hex(X) << " y=" << hex(Y)
                    << " lhs=" << hex(Got->first) << " rhs=" << hex(Got->second)
                    << "\n";
          return 1;
        }
        ++RelationChecks;
      }
    }
  }

  if (RewriteChecks == 0 || RelationChecks == 0)
    return fail("no checks executed");

  std::cout << "YMBA catalog self-test passed\n"
            << "rewrite rules: " << Rewrites.size() << "\n"
            << "relation rules: " << Relations.size() << "\n"
            << "checks: " << (RewriteChecks + RelationChecks) << "\n";
  return 0;
}
