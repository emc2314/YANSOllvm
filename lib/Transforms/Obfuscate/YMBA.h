#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <cstdint>

namespace llvm {

/// Runtime reader/emitter for the embedded YMBA catalog.
///
/// The catalog is intentionally data-only: a versioned binary blob containing
/// postfix bytecode expressions plus rewrite/relation records. This keeps the
/// VM variant policy in C++ while allowing yanso-mba-relations to regenerate the
/// database without generating C++ emitter functions.
class YMBA {
public:
  struct Relation {
    Value *L = nullptr;
    Value *R = nullptr;
  };

  static bool isSupportedBinaryOpcode(unsigned LLVMOpcode);
  static Value *emitBinary(IRBuilder<> &B, unsigned LLVMOpcode, IntegerType *Ty,
                           Value *X, Value *Y, unsigned Variant,
                           uint64_t Seed);

  // Catalog-only hooks kept for the standalone self-test/debugging. Normal VM
  // emission should use emitBinary(), which owns catalog-vs-default fallback.
  static unsigned rewriteCount(unsigned LLVMOpcode, unsigned BitWidth);
  static Value *emitRewrite(IRBuilder<> &B, unsigned LLVMOpcode,
                            IntegerType *Ty, Value *X, Value *Y, unsigned Index,
                            uint64_t Seed);

  static Value *emitIntrinsic(IRBuilder<> &B, Intrinsic::ID ID, IntegerType *Ty,
                              ArrayRef<Value *> Args, unsigned Variant,
                              uint64_t Seed);

  static Relation emitRelation(IRBuilder<> &B, IntegerType *Ty, Value *X,
                               Value *Y, unsigned Index, uint64_t Seed);

  /// Public for tests/debugging and for keeping generator constants stable.
  enum : uint16_t {
    YMBA_OP_ADD = 1,
    YMBA_OP_SUB = 2,
    YMBA_OP_MUL = 3,
    YMBA_OP_AND = 4,
    YMBA_OP_OR = 5,
    YMBA_OP_XOR = 6,
    YMBA_OP_SHL = 7,
    YMBA_OP_LSHR = 8,
    YMBA_OP_ASHR = 9,
    YMBA_OP_ROTL = 10,
    YMBA_OP_ROTR = 11,
    YMBA_OP_CTPOP = 12,
    YMBA_OP_BSWAP = 13,
    YMBA_OP_BITREVERSE = 14,
  };

  enum : uint16_t {
    YMBA_W8 = 1u << 0,
    YMBA_W16 = 1u << 1,
    YMBA_W32 = 1u << 2,
    YMBA_W64 = 1u << 3,
    YMBA_W128 = 1u << 4,
  };
};

} // namespace llvm
