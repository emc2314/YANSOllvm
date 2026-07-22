#pragma once

#include "YMBA.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <cstdint>
#include <optional>

namespace llvm {

/// Per-node handler-body diversification for VM super-ops (mutation, relation).
/// Binary expr choice is owned by YMBA; VMPass only plans the rewrite.
class VMVariantEmitter {
public:
  enum class BinaryMutation : uint8_t {
    None,
    BitRebuild,
    DataMuxAddSub,
    DataMuxOrAbsorb,
    DataMuxXorRoundTrip
  };
  using IntegerDecoration = YMBA::IntegerDecoration;
  enum class PredicateExpr : uint8_t { Direct, DoubleNot, XorRoundTrip };
  enum class SelectExpr : uint8_t { Direct, Inverted, XorRoundTrip };
  enum class ProjectorKind : uint8_t { LowBit, Parity, KeyedBit };
  enum class RelationApplication : uint8_t {
    DiffFold,
    PairedMulBranch,
    PairedAffineBranch,
    OpaqueFork
  };

  struct RelationVariant {
    ProjectorKind Projector = ProjectorKind::LowBit;
    RelationApplication Application = RelationApplication::DiffFold;
  };

  struct BinaryVariant {
    BinaryMutation Mutation = BinaryMutation::None;
    std::optional<RelationVariant> Relation;
  };

  struct PredicateVariant {
    PredicateExpr Primary = PredicateExpr::Direct;
    std::optional<PredicateExpr> Alternate;
  };

  struct SelectVariant {
    SelectExpr Primary = SelectExpr::Direct;
    std::optional<SelectExpr> Alternate;
  };

  struct IntrinsicVariant {
    std::optional<RelationVariant> Relation;
  };

  struct CastVariant {
    IntegerDecoration InputDecoration = IntegerDecoration::Identity;
    IntegerDecoration OutputDecoration = IntegerDecoration::Identity;
    std::optional<RelationVariant> Relation;
  };

  static BinaryVariant selectBinaryVariant(unsigned Opcode, IntegerType *Ty,
                                           uint64_t Seed,
                                           unsigned MutationPermille,
                                           unsigned RelationAppPermille);

  static Value *emitBinaryValue(IRBuilder<> &B, unsigned Opcode,
                                IntegerType *Ty, Value *X, Value *Y,
                                const BinaryVariant &Variant, uint64_t Seed);
  static PredicateVariant selectPredicateVariant(Type *Ty, uint64_t Seed,
                                                 unsigned MutationPermille);
  static Value *emitICmpValue(IRBuilder<> &B, CmpInst::Predicate Pred, Type *Ty,
                              Value *X, Value *Y,
                              const PredicateVariant &Variant, uint64_t Seed);
  static SelectVariant selectSelectVariant(Type *Ty, uint64_t Seed,
                                           unsigned MutationPermille);
  static Value *emitSelectValue(IRBuilder<> &B, Type *Ty, Value *Cond,
                                Value *TrueV, Value *FalseV,
                                const SelectVariant &Variant, uint64_t Seed);
  static IntrinsicVariant selectIntrinsicVariant(IntegerType *Ty,
                                                 uint64_t Seed);
  static CastVariant selectCastVariant(Type *SrcTy, Type *DstTy, uint64_t Seed);
  static Value *emitIntrinsicValue(IRBuilder<> &B, Intrinsic::ID ID,
                                   IntegerType *Ty, ArrayRef<Value *> Args,
                                   const IntrinsicVariant &Variant,
                                   uint64_t Seed);
  static Value *emitCastValue(IRBuilder<> &B, unsigned Opcode, Type *SrcTy,
                              Type *DstTy, Value *X, const CastVariant &Variant,
                              uint64_t Seed);

private:
  struct Relation {
    Value *L = nullptr;
    Value *R = nullptr;
  };

  static Value *loConst(IntegerType *Ty, uint64_t V);

  static Relation emitRelation(IRBuilder<> &B, IntegerType *Ty, Value *X,
                               Value *Y, uint64_t Seed);
  static Value *emitProjector(IRBuilder<> &B, IntegerType *Ty, Value *V,
                              ProjectorKind Kind, uint64_t Seed,
                              StringRef Name);

  static Value *applyRelation(IRBuilder<> &B, IntegerType *Ty, Value *R,
                              Value *X, Value *Y,
                              const std::optional<RelationVariant> &Variant,
                              uint64_t Seed);
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
  static Value *applyRelationOpaqueFork(IRBuilder<> &B, IntegerType *Ty,
                                        Value *R, const Relation &Rel,
                                        ProjectorKind Projector, uint64_t Seed);
  static bool supportsLoopMutation(unsigned Opcode, unsigned BitWidth);
  static bool supportsDataMuxMutation(unsigned Opcode, unsigned BitWidth);
  static bool supportsRelation(unsigned BitWidth);
  static bool supportsPredicateMutation(Type *Ty);
  static bool supportsSelectMutation(Type *Ty);

  static Value *emitICmpExpr(IRBuilder<> &B, CmpInst::Predicate Pred, Type *Ty,
                             Value *X, Value *Y, PredicateExpr Expr,
                             uint64_t Seed);
  static Value *emitSelectExpr(IRBuilder<> &B, Type *Ty, Value *Cond,
                               Value *TrueV, Value *FalseV, SelectExpr Expr,
                               uint64_t Seed);
  static Value *emitControlFlowBitRebuild(IRBuilder<> &B, Function *F,
                                          IntegerType *Ty, Value *Input);
  static Value *emitDataMuxBinaryValue(IRBuilder<> &B, unsigned Opcode,
                                       IntegerType *Ty, Value *X, Value *Y,
                                       const BinaryVariant &Variant,
                                       uint64_t Seed);
};

} // namespace llvm
