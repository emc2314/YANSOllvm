#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <cstdint>
#include <string>

namespace llvm {

/// Per-node handler-body diversification for VM super-ops (mutation, relation).
/// Binary expr choice is owned by YMBA; VMPass only plans the rewrite.
class VMVariantEmitter {
public:
  enum class MutationKind : uint8_t { None, BitRebuild, DataMux };
  enum class ProjectorKind : uint8_t { LowBit, Parity, KeyedBit };
  enum class RelationApplication : uint8_t {
    DiffFold,
    PairedMulBranch,
    PairedAffineBranch,
    OpaqueFork
  };

  struct BinaryVariant {
    MutationKind Mutation = MutationKind::None;
    unsigned MutationVariant = 0;
    bool ApplyRelation = false;
    ProjectorKind Projector = ProjectorKind::LowBit;
    RelationApplication RelationApp = RelationApplication::DiffFold;
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

  struct ScalarVariant {
    unsigned ExprVariant = 0;
    bool ApplyRelation = false;
    ProjectorKind Projector = ProjectorKind::LowBit;
    RelationApplication RelationApp = RelationApplication::DiffFold;
  };

  static BinaryVariant selectBinaryVariant(unsigned Opcode, IntegerType *Ty,
                                           uint64_t Seed,
                                           unsigned MutationPermille,
                                           unsigned RelationAppPermille);

  static std::string suffix(const BinaryVariant &Variant);
  static std::string suffix(const PredicateVariant &Variant);
  static std::string suffix(const SelectVariant &Variant);

  static void emitBinary(IRBuilder<> &B, unsigned Opcode, IntegerType *Ty,
                         Value *X, Value *Y, const BinaryVariant &Variant,
                         uint64_t Seed);
  static Value *emitBinaryValue(IRBuilder<> &B, unsigned Opcode,
                                IntegerType *Ty, Value *X, Value *Y,
                                const BinaryVariant &Variant, uint64_t Seed);
  static PredicateVariant selectPredicateVariant(Type *Ty, uint64_t Seed,
                                                 unsigned MutationPermille);
  static void emitICmp(IRBuilder<> &B, CmpInst::Predicate Pred, Type *Ty,
                       Value *X, Value *Y, const PredicateVariant &Variant,
                       uint64_t Seed);
  static Value *emitICmpValue(IRBuilder<> &B, CmpInst::Predicate Pred, Type *Ty,
                              Value *X, Value *Y,
                              const PredicateVariant &Variant, uint64_t Seed);
  static SelectVariant selectSelectVariant(Type *Ty, uint64_t Seed,
                                           unsigned MutationPermille);
  static void emitSelect(IRBuilder<> &B, Type *Ty, Value *Cond, Value *TrueV,
                         Value *FalseV, const SelectVariant &Variant,
                         uint64_t Seed);
  static Value *emitSelectValue(IRBuilder<> &B, Type *Ty, Value *Cond,
                                Value *TrueV, Value *FalseV,
                                const SelectVariant &Variant, uint64_t Seed);
  static ScalarVariant selectIntrinsicVariant(Intrinsic::ID ID, IntegerType *Ty,
                                              uint64_t Seed);
  static ScalarVariant selectCastVariant(unsigned Opcode, Type *SrcTy,
                                         Type *DstTy, uint64_t Seed);
  static std::string suffix(const ScalarVariant &Variant);
  static void emitIntrinsic(IRBuilder<> &B, Intrinsic::ID ID, IntegerType *Ty,
                            ArrayRef<Value *> Args,
                            const ScalarVariant &Variant, uint64_t Seed);
  static Value *emitIntrinsicValue(IRBuilder<> &B, Intrinsic::ID ID,
                                   IntegerType *Ty, ArrayRef<Value *> Args,
                                   const ScalarVariant &Variant, uint64_t Seed);
  static void emitCast(IRBuilder<> &B, unsigned Opcode, Type *SrcTy,
                       Type *DstTy, Value *X, const ScalarVariant &Variant,
                       uint64_t Seed);
  static Value *emitCastValue(IRBuilder<> &B, unsigned Opcode, Type *SrcTy,
                              Type *DstTy, Value *X,
                              const ScalarVariant &Variant, uint64_t Seed);

private:
  struct Relation {
    Value *L = nullptr;
    Value *R = nullptr;
  };

  static Value *loConst(IntegerType *Ty, uint64_t V);
  static Value *notV(IRBuilder<> &B, Value *V);

  static Value *decorateIntegerResult(IRBuilder<> &B, IntegerType *Ty, Value *V,
                                      unsigned Variant, uint64_t Seed,
                                      StringRef NamePrefix);

  static Relation emitRelation(IRBuilder<> &B, IntegerType *Ty, Value *X,
                               Value *Y, uint64_t Seed);
  static Value *emitProjector(IRBuilder<> &B, IntegerType *Ty, Value *V,
                              ProjectorKind Kind, uint64_t Seed,
                              StringRef Name);

  static Value *applyRelation(IRBuilder<> &B, IntegerType *Ty, Value *R,
                              Value *X, Value *Y, const BinaryVariant &Variant,
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
  static Value *applyScalarRelation(IRBuilder<> &B, IntegerType *Ty, Value *R,
                                    Value *X, Value *Y,
                                    const ScalarVariant &Variant,
                                    uint64_t Seed);
  static bool supportsLoopMutation(unsigned Opcode, unsigned BitWidth);
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
  static Value *emitControlFlowBitRebuild(IRBuilder<> &B, Function *F,
                                          IntegerType *Ty, Value *Input);
  static Value *emitDataMuxBinaryValue(IRBuilder<> &B, unsigned Opcode,
                                       IntegerType *Ty, Value *X, Value *Y,
                                       const BinaryVariant &Variant,
                                       uint64_t Seed);
  static void emitDataMuxBinary(IRBuilder<> &B, unsigned Opcode,
                                IntegerType *Ty, Value *X, Value *Y,
                                const BinaryVariant &Variant, uint64_t Seed);
};

} // namespace llvm
