#include "YMBA.h"

#include "GeneratedYMBACatalog.inc"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"

#include <algorithm>
#include <cstddef>
#include <optional>

using namespace llvm;

namespace {

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

static FunctionCallee getIntrinsic(IRBuilder<> &B, Intrinsic::ID ID,
                                   IntegerType *Ty) {
  return Intrinsic::getOrInsertDeclaration(B.GetInsertBlock()->getModule(), ID,
                                           Ty);
}

static Value *emitBinaryIntrinsic(IRBuilder<> &B, Intrinsic::ID ID,
                                  IntegerType *Ty, Value *L, Value *R) {
  return B.CreateCall(getIntrinsic(B, ID, Ty), {L, R});
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

  unsigned relationCount(unsigned BitWidth) const {
    if (!Valid)
      return 0;
    uint16_t WM = widthMaskFor(BitWidth);
    if (!WM)
      return 0;
    return std::count_if(
        Relations.begin(), Relations.end(), [&](const auto &R) {
          return (R.WidthMask & WM) && R.LExpr < Exprs.size() &&
                 R.RExpr < Exprs.size() && (Exprs[R.LExpr].WidthMask & WM) &&
                 (Exprs[R.RExpr].WidthMask & WM);
        });
  }

  const RelationRecord *selectRelation(unsigned BitWidth,
                                       unsigned Index) const {
    if (!Valid)
      return nullptr;
    uint16_t WM = widthMaskFor(BitWidth);
    if (!WM)
      return nullptr;
    SmallVector<const RelationRecord *, 32> Bucket;
    for (const RelationRecord &R : Relations) {
      if ((R.WidthMask & WM) && R.LExpr < Exprs.size() &&
          R.RExpr < Exprs.size() && (Exprs[R.LExpr].WidthMask & WM) &&
          (Exprs[R.RExpr].WidthMask & WM))
        Bucket.push_back(&R);
    }
    if (Bucket.empty())
      return nullptr;
    return Bucket[Index % Bucket.size()];
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

bool YMBA::hasRewrite(unsigned LLVMOpcode, unsigned BitWidth) {
  return rewriteCount(LLVMOpcode, BitWidth) != 0;
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

bool YMBA::hasIntrinsicRewrite(Intrinsic::ID ID, unsigned BitWidth) {
  return intrinsicRewriteCount(ID, BitWidth) != 0;
}

unsigned YMBA::intrinsicRewriteCount(Intrinsic::ID ID, unsigned BitWidth) {
  auto Op = intrinsicToYMBAOpcode(ID);
  if (!Op)
    return 0;
  return catalog().rewriteCount(*Op, BitWidth);
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

Value *YMBA::emitIntrinsicRewrite(IRBuilder<> &B, Intrinsic::ID ID,
                                  IntegerType *Ty, ArrayRef<Value *> Args,
                                  unsigned Index, uint64_t) {
  auto Op = intrinsicToYMBAOpcode(ID);
  if (!Op || Args.empty() || !isSupportedIntrinsicRewriteShape(ID, Args))
    return nullptr;
  const RewriteRecord *R =
      catalog().selectRewrite(*Op, Ty->getBitWidth(), Index);
  if (!R)
    return nullptr;
  Value *X = Args[0];
  Value *Y = Args.size() >= 2 ? Args[1] : ConstantInt::get(Ty, 0);
  // fshl/fshr have three LLVM operands. YMBA models rotate amount as Y.
  if ((ID == Intrinsic::fshl || ID == Intrinsic::fshr) && Args.size() >= 3)
    Y = Args[2];
  return catalog().emitExpr(B, Ty, X, Y, R->Expr);
}

bool YMBA::hasRelation(unsigned BitWidth) {
  return relationCount(BitWidth) != 0;
}

unsigned YMBA::relationCount(unsigned BitWidth) {
  return catalog().relationCount(BitWidth);
}

YMBA::Relation YMBA::emitRelation(IRBuilder<> &B, IntegerType *Ty, Value *X,
                                  Value *Y, unsigned Index, uint64_t) {
  const RelationRecord *R = catalog().selectRelation(Ty->getBitWidth(), Index);
  if (!R)
    return {};
  Value *L = catalog().emitExpr(B, Ty, X, Y, R->LExpr);
  Value *RV = catalog().emitExpr(B, Ty, X, Y, R->RExpr);
  return {L, RV};
}
