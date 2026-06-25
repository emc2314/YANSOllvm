#include "MFLAInternal.h"
#include "CryptoUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/MathExtras.h"

#include <cassert>

using namespace llvm;

namespace yansollvm::mfla {

void MFLAArtifacts::eraseFromParent() {
  if (Mega) {
    Mega->eraseFromParent();
    Mega = nullptr;
  }
  for (GlobalVariable *GV : reverse(EdgeConstants)) {
    if (GV)
      GV->eraseFromParent();
  }
  EdgeConstants.clear();
  for (GlobalVariable *GV : reverse(RemappedGlobals)) {
    if (GV)
      GV->eraseFromParent();
  }
  RemappedGlobals.clear();
  GlobalRemaps.clear();
  EntryEdgeForFunction.clear();
}

Constant *constI64(LLVMContext &Ctx, uint64_t V) {
  return ConstantInt::get(Type::getInt64Ty(Ctx), V);
}

Constant *keyForState(MFLAArtifacts &A, uint64_t State) {
  return ConstantInt::get(Type::getInt64Ty(A.Mega->getContext()),
                          yanso_mix64(State, A.StateKeySalt));
}

Argument *ctxArg(MFLAArtifacts &A) { return A.Mega->getArg(0); }

Value *ctxBytePtr(IRBuilder<> &B, Value *Ctx, uint64_t Offset) {
  return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Ctx,
                             constI64(B.getContext(), Offset));
}

Value *ctxBytePtr(IRBuilder<> &B, MFLAArtifacts &A, uint64_t Offset) {
  return ctxBytePtr(B, ctxArg(A), Offset);
}

Value *storagePtr(IRBuilder<> &B, MFLAArtifacts &A, StorageRef Ref) {
  return ctxBytePtr(B, A, A.FrameOffset + Ref.Offset);
}

Value *storagePtr(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                  StorageRef Ref) {
  return ctxBytePtr(B, Ctx, A.FrameOffset + Ref.Offset);
}

LoadInst *loadSlot(IRBuilder<> &B, Type *Ty, MFLAArtifacts &A, StorageRef Ref,
                   StringRef Name) {
  return B.CreateLoad(Ty, storagePtr(B, A, Ref), Name);
}

LoadInst *loadSlot(IRBuilder<> &B, Type *Ty, MFLAArtifacts &A, Value *Ctx,
                   StorageRef Ref, StringRef Name) {
  return B.CreateLoad(Ty, storagePtr(B, A, Ctx, Ref), Name);
}

void storeSlot(IRBuilder<> &B, Value *V, MFLAArtifacts &A, StorageRef Ref) {
  B.CreateStore(V, storagePtr(B, A, Ref));
}

void storeSlot(IRBuilder<> &B, Value *V, MFLAArtifacts &A, Value *Ctx,
               StorageRef Ref) {
  B.CreateStore(V, storagePtr(B, A, Ctx, Ref));
}

uint64_t continuationRecordOffset(MFLAArtifacts &A, ContinuationRecord Rec) {
  return A.ContinuationOffset + Rec.Slot.ID * A.ContinuationStride;
}

uint64_t continuationXorOffset(MFLAArtifacts &A, ContinuationRecord Rec) {
  return continuationRecordOffset(A, Rec);
}

uint64_t continuationEdgeOffset(MFLAArtifacts &A, ContinuationRecord Rec) {
  return continuationRecordOffset(A, Rec) + 8;
}

Value *continuationXorPtr(IRBuilder<> &B, MFLAArtifacts &A,
                          ContinuationRecord Rec) {
  return ctxBytePtr(B, A, continuationXorOffset(A, Rec));
}

Value *continuationEdgePtr(IRBuilder<> &B, MFLAArtifacts &A,
                           ContinuationRecord Rec) {
  return ctxBytePtr(B, A, continuationEdgeOffset(A, Rec));
}

Value *continuationXorPtr(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                          ContinuationRecord Rec) {
  return ctxBytePtr(B, Ctx, continuationXorOffset(A, Rec));
}

Value *continuationEdgePtr(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                           ContinuationRecord Rec) {
  return ctxBytePtr(B, Ctx, continuationEdgeOffset(A, Rec));
}

ContinuationRecord continuationRecord(const FunctionLayout &L) {
  return {L.ContSlot, nullptr, {}, false};
}

ContinuationRecord continuationRecord(const FunctionLayout &L,
                                      BasicBlock *ResumeBlock,
                                      StorageRef ResultSlot,
                                      bool HasResultSlot) {
  return {L.ContSlot, ResumeBlock, ResultSlot, HasResultSlot};
}

Value *loadContinuationXor(IRBuilder<> &B, MFLAArtifacts &A,
                           ContinuationRecord Rec, StringRef Name) {
  return B.CreateLoad(Type::getInt64Ty(A.Mega->getContext()),
                      continuationXorPtr(B, A, Rec), Name);
}

Value *loadContinuationEdge(IRBuilder<> &B, MFLAArtifacts &A,
                            ContinuationRecord Rec, StringRef Name) {
  return B.CreateLoad(Type::getInt64Ty(A.Mega->getContext()),
                      continuationEdgePtr(B, A, Rec), Name);
}

void storeContinuation(IRBuilder<> &B, MFLAArtifacts &A, ContinuationRecord Rec,
                       uint64_t CalleeEntryState, uint64_t ResumeState,
                       Value *ContEdge) {
  assert(Rec.ResumeBlock && "push continuation requires a resume block");
  (void)Rec;
  B.CreateStore(constI64(A.Mega->getContext(), CalleeEntryState ^ ResumeState),
                continuationXorPtr(B, A, Rec));
  B.CreateStore(ContEdge, continuationEdgePtr(B, A, Rec));
}

void clearContinuation(IRBuilder<> &B, MFLAArtifacts &A,
                       ContinuationRecord Rec) {
  B.CreateStore(constI64(A.Mega->getContext(), 0),
                continuationXorPtr(B, A, Rec));
  B.CreateStore(constI64(A.Mega->getContext(), 0),
                continuationEdgePtr(B, A, Rec));
}

void initializeContinuation(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                            ContinuationRecord Rec) {
  B.CreateStore(ConstantInt::get(Type::getInt64Ty(A.Mega->getContext()), 0),
                continuationXorPtr(B, A, Ctx, Rec));
  B.CreateStore(ConstantInt::get(Type::getInt64Ty(A.Mega->getContext()), 0),
                continuationEdgePtr(B, A, Ctx, Rec));
}

Value *loadState(IRBuilder<> &B, MFLAArtifacts &A, StringRef Name) {
  return B.CreateLoad(Type::getInt64Ty(A.Mega->getContext()),
                      ctxBytePtr(B, A, A.StateOffset), Name);
}

void storeState(IRBuilder<> &B, MFLAArtifacts &A, Value *State) {
  B.CreateStore(State, ctxBytePtr(B, A, A.StateOffset));
}

} // namespace yansollvm::mfla
