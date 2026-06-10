#pragma once

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <cstdint>
#include <string>

namespace llvm {

/// VMVariantEmitter owns VM helper-body diversification. VMPass decides which
/// source instruction is virtualized; this helper decides how the replacement
/// handler computes the same operation.
///
/// Binary handler variation is split into four dimensions:
/// - ExprVariant: local semantic expression template for the requested opcode.
/// - Mutation: structural wrapper around one or more equivalent expressions.
/// - Relation: an equality provider, producing L == R for later use.
/// - RelationApplication: embeds that equality into the result, optionally via a
///   Projector P where P(L) == P(R).
///
/// This keeps VMPass as a semantic-rewrite planner and leaves handler-body
/// mutation policy behind this boundary. A later backend can replace the built-in
/// expression/relation templates with mined MBA/template databases without
/// changing VMPass.
class VMVariantEmitter {
public:
  enum class MutationKind : uint8_t { None, BitRebuild, OpaqueFork, DataMux };
  enum class RelationKind : uint8_t { None, PopcountCarry };
  enum class ProjectorKind : uint8_t { LowBit, Parity, KeyedBit };
  enum class RelationApplication : uint8_t {
    DiffFold,
    PairedMulBranch,
    PairedAffineBranch
  };

  struct BinaryVariant {
    unsigned ExprVariant = 0;
    MutationKind Mutation = MutationKind::None;
    unsigned MutationVariant = 0;
    RelationKind Relation = RelationKind::None;
    ProjectorKind Projector = ProjectorKind::LowBit;
    RelationApplication RelationApp = RelationApplication::DiffFold;
    unsigned RelationVariant = 0;
  };

  struct PredicateVariant {
    unsigned ExprVariant = 0;
    MutationKind Mutation = MutationKind::None;
    unsigned MutationVariant = 0;
  };

  struct SelectVariant {
    unsigned ExprVariant = 0;
    MutationKind Mutation = MutationKind::None;
    unsigned MutationVariant = 0;
  };

  static BinaryVariant selectBinaryVariant(unsigned Opcode, IntegerType *Ty,
                                           uint64_t Seed,
                                           unsigned ControlFlowPermille,
                                           unsigned ForkPermille,
                                           unsigned DataMuxPermille,
                                           unsigned RelationPermille);

  static std::string suffix(const BinaryVariant &Variant);
  static std::string suffix(const PredicateVariant &Variant);
  static std::string suffix(const SelectVariant &Variant);

  static void emitBinary(IRBuilder<> &B, unsigned Opcode, IntegerType *Ty,
                         Value *X, Value *Y, const BinaryVariant &Variant,
                         uint64_t Seed);
  static PredicateVariant selectPredicateVariant(Type *Ty, uint64_t Seed,
                                                 unsigned ForkPermille,
                                                 unsigned DataMuxPermille);
  static void emitICmp(IRBuilder<> &B, CmpInst::Predicate Pred, Type *Ty,
                       Value *X, Value *Y, const PredicateVariant &Variant,
                       uint64_t Seed);
  static SelectVariant selectSelectVariant(Type *Ty, uint64_t Seed,
                                           unsigned ForkPermille,
                                           unsigned DataMuxPermille);
  static void emitSelect(IRBuilder<> &B, Type *Ty, Value *Cond, Value *TrueV,
                         Value *FalseV, const SelectVariant &Variant,
                         uint64_t Seed);

private:
  struct Relation {
    Value *L = nullptr;
    Value *R = nullptr;
  };

  static Value *loConst(IntegerType *Ty, uint64_t V);
  static Value *notV(IRBuilder<> &B, Value *V);

  static Value *emitXorExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                            Value *Y, unsigned Variant);
  static Value *emitAndExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                            Value *Y, unsigned Variant);
  static Value *emitOrExpr(IRBuilder<> &B, IntegerType *Ty, Value *X, Value *Y,
                           unsigned Variant);
  static Value *emitAddExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                            Value *Y, unsigned Variant);
  static Value *emitSubExpr(IRBuilder<> &B, IntegerType *Ty, Value *X,
                            Value *Y, unsigned Variant);
  static Value *emitShiftExpr(IRBuilder<> &B, unsigned Opcode, IntegerType *Ty,
                              Value *X, Value *Y, unsigned Variant);
  static Value *emitBinaryExpr(IRBuilder<> &B, unsigned Opcode, IntegerType *Ty,
                               Value *X, Value *Y, unsigned ExprVariant,
                               uint64_t Seed);

  static Value *emitRotateRight(IRBuilder<> &B, IntegerType *Ty, Value *X,
                                unsigned Amount);
  static Relation emitRelation(IRBuilder<> &B, IntegerType *Ty, Value *X,
                               Value *Y, RelationKind Kind, unsigned Variant,
                               uint64_t Seed);
  static Value *emitProjector(IRBuilder<> &B, IntegerType *Ty, Value *V,
                              ProjectorKind Kind, uint64_t Seed,
                              StringRef Name);

  static Value *applyRelation(IRBuilder<> &B, IntegerType *Ty, Value *R,
                              Value *X, Value *Y,
                              const BinaryVariant &Variant, uint64_t Seed);
  static Value *applyRelationDiffFold(IRBuilder<> &B, IntegerType *Ty, Value *R,
                                      const Relation &Rel);
  static Value *applyRelationPairedMulBranch(IRBuilder<> &B, IntegerType *Ty,
                                             Value *R, const Relation &Rel,
                                             ProjectorKind Projector,
                                             uint64_t Seed);
  static Value *applyRelationPairedAffineBranch(IRBuilder<> &B, IntegerType *Ty,
                                                Value *R, const Relation &Rel,
                                                ProjectorKind Projector,
                                                uint64_t Seed);
  static uint64_t oddInverse64(uint64_t V);

  static bool supportsLoopMutation(unsigned Opcode, unsigned BitWidth);
  static bool supportsForkMutation(unsigned Opcode, unsigned BitWidth);
  static bool supportsDataMuxMutation(unsigned Opcode, unsigned BitWidth);
  static bool supportsRelation(unsigned BitWidth);
  static bool supportsPredicateMutation(Type *Ty);
  static bool supportsSelectMutation(Type *Ty);

  static Value *emitICmpExpr(IRBuilder<> &B, CmpInst::Predicate Pred, Type *Ty,
                             Value *X, Value *Y, unsigned ExprVariant,
                             uint64_t Seed);
  static Value *emitSelectExpr(IRBuilder<> &B, Type *Ty, Value *Cond,
                               Value *TrueV, Value *FalseV,
                               unsigned ExprVariant, uint64_t Seed);
  static void emitPredicateFork(IRBuilder<> &B, Value *Pred,
                                const PredicateVariant &Variant,
                                uint64_t Seed);
  static void emitSelectFork(IRBuilder<> &B, Type *Ty, Value *Cond,
                             Value *TrueV, Value *FalseV,
                             const SelectVariant &Variant, uint64_t Seed);

  static void emitControlFlowBitRebuild(IRBuilder<> &B, Function *F,
                                        IntegerType *Ty, Value *Input);
  static void emitOpaqueForkedBinary(IRBuilder<> &B, unsigned Opcode,
                                     IntegerType *Ty, Value *X, Value *Y,
                                     const BinaryVariant &Variant,
                                     uint64_t Seed);
  static void emitDataMuxBinary(IRBuilder<> &B, unsigned Opcode,
                                IntegerType *Ty, Value *X, Value *Y,
                                const BinaryVariant &Variant, uint64_t Seed);
};

} // namespace llvm
