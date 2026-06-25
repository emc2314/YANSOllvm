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

FrameRef currentFrame(const FunctionLayout &L) { return {L.Owner, nullptr, false}; }

static Type *tokenTy(LLVMContext &Ctx) { return Type::getInt64Ty(Ctx); }

FrameRef frameWithToken(const FunctionLayout &L, Value *Token) {
  return {L.Owner, Token, true};
}

FrameRef rootFrame(const FunctionLayout &L, LLVMContext &Ctx) {
  return frameWithToken(L, constI64(Ctx, 0));
}

FrameRef currentFrame(IRBuilder<> &B, MFLAArtifacts &A,
                      const FunctionLayout &L) {
  if (A.MaxFrames <= 1)
    return currentFrame(L);
  return frameWithToken(L, loadCurrentFrameToken(B, A));
}

Value *loadCurrentFrameToken(IRBuilder<> &B, MFLAArtifacts &A,
                             StringRef Name) {
  return B.CreateLoad(tokenTy(A.Mega->getContext()),
                      ctxBytePtr(B, A, A.CurrentFrameOffset), Name);
}

void storeCurrentFrameToken(IRBuilder<> &B, MFLAArtifacts &A, Value *Token) {
  B.CreateStore(Token, ctxBytePtr(B, A, A.CurrentFrameOffset));
}

Value *loadFrameTop(IRBuilder<> &B, MFLAArtifacts &A, StringRef Name) {
  return B.CreateLoad(tokenTy(A.Mega->getContext()),
                      ctxBytePtr(B, A, A.FrameTopOffset), Name);
}

void storeFrameTop(IRBuilder<> &B, MFLAArtifacts &A, Value *Top) {
  B.CreateStore(Top, ctxBytePtr(B, A, A.FrameTopOffset));
}

FrameRef pushFrame(IRBuilder<> &B, MFLAArtifacts &A,
                   const FunctionLayout &L) {
  Value *Token = loadFrameTop(B, A, "mfla.frame.push");
  Value *Next = B.CreateAdd(Token, constI64(B.getContext(), 1),
                            "mfla.frame.next");
  storeFrameTop(B, A, Next);
  storeCurrentFrameToken(B, A, Token);
  return frameWithToken(L, Token);
}

void restoreFrame(IRBuilder<> &B, MFLAArtifacts &A, Value *Token) {
  storeCurrentFrameToken(B, A, Token);
}

Value *loadReturnedFrameToken(IRBuilder<> &B, MFLAArtifacts &A,
                              StringRef Name) {
  return B.CreateLoad(tokenTy(A.Mega->getContext()),
                      ctxBytePtr(B, A, A.ReturnedFrameOffset), Name);
}

void storeReturnedFrameToken(IRBuilder<> &B, MFLAArtifacts &A, Value *Token) {
  B.CreateStore(Token, ctxBytePtr(B, A, A.ReturnedFrameOffset));
}

static Value *frameSlotPtrImpl(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                               FrameRef Frame, StorageRef Ref) {
  if (!Frame.Dynamic)
    return storagePtr(B, A, Ctx, Ref);

  assert(Frame.Token && "dynamic frame requires a frame token");
  Type *I64 = Type::getInt64Ty(B.getContext());
  Value *Token = B.CreateZExtOrTrunc(Frame.Token, I64, "mfla.frame.token");
  Value *Scaled = B.CreateMul(Token, constI64(B.getContext(), A.FrameStride),
                              "mfla.frame.off");
  Value *Base = B.CreateAdd(Scaled,
                            constI64(B.getContext(), A.FrameArenaOffset +
                                                          Ref.Offset),
                            "mfla.frame.slot.off");
  return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Ctx, Base,
                             "mfla.frame.slot");
}

Value *frameSlotPtr(IRBuilder<> &B, MFLAArtifacts &A, FrameRef Frame,
                    StorageRef Ref) {
  return frameSlotPtrImpl(B, A, ctxArg(A), Frame, Ref);
}

Value *frameSlotPtr(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                    FrameRef Frame, StorageRef Ref) {
  return frameSlotPtrImpl(B, A, Ctx, Frame, Ref);
}

LoadInst *loadFrameSlot(IRBuilder<> &B, Type *Ty, MFLAArtifacts &A,
                        FrameRef Frame, StorageRef Ref, StringRef Name) {
  return B.CreateLoad(Ty, frameSlotPtr(B, A, Frame, Ref), Name);
}

LoadInst *loadFrameSlot(IRBuilder<> &B, Type *Ty, MFLAArtifacts &A, Value *Ctx,
                        FrameRef Frame, StorageRef Ref, StringRef Name) {
  return B.CreateLoad(Ty, frameSlotPtr(B, A, Ctx, Frame, Ref), Name);
}

void storeFrameSlot(IRBuilder<> &B, Value *V, MFLAArtifacts &A,
                    FrameRef Frame, StorageRef Ref) {
  B.CreateStore(V, frameSlotPtr(B, A, Frame, Ref));
}

void storeFrameSlot(IRBuilder<> &B, Value *V, MFLAArtifacts &A, Value *Ctx,
                    FrameRef Frame, StorageRef Ref) {
  B.CreateStore(V, frameSlotPtr(B, A, Ctx, Frame, Ref));
}

uint64_t continuationRecordOffset(MFLAArtifacts &A, ContinuationRecord Rec) {
  assert(!Rec.Frame.Dynamic && "dynamic continuation offset needs IR pointer");
  return A.ContinuationOffset + Rec.Slot.ID * A.ContinuationStride;
}

uint64_t continuationXorOffset(MFLAArtifacts &A, ContinuationRecord Rec) {
  return continuationRecordOffset(A, Rec);
}

uint64_t continuationEdgeOffset(MFLAArtifacts &A, ContinuationRecord Rec) {
  return continuationRecordOffset(A, Rec) + 8;
}

static Value *dynamicContinuationRecordPtr(IRBuilder<> &B, MFLAArtifacts &A,
                                           Value *Ctx,
                                           ContinuationRecord Rec) {
  assert(Rec.Frame.Dynamic && Rec.Frame.Token &&
         "dynamic continuation requires frame token");
  Type *I64 = Type::getInt64Ty(B.getContext());
  Value *Token = B.CreateZExtOrTrunc(Rec.Frame.Token, I64, "mfla.cont.frame");
  Value *RecordIndex =
      B.CreateAdd(B.CreateMul(Token,
                              constI64(B.getContext(), A.ContinuationSlots),
                              "mfla.cont.frame.base"),
                  constI64(B.getContext(), Rec.Slot.ID), "mfla.cont.index");
  Value *Scaled = B.CreateMul(RecordIndex,
                              constI64(B.getContext(), A.ContinuationStride),
                              "mfla.cont.off");
  Value *Base = B.CreateAdd(Scaled, constI64(B.getContext(), A.ContinuationOffset),
                            "mfla.cont.record.off");
  return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Ctx, Base,
                             "mfla.cont.record");
}

static Value *continuationFieldPtr(IRBuilder<> &B, MFLAArtifacts &A,
                                   Value *Ctx, ContinuationRecord Rec,
                                   uint64_t FieldOffset) {
  if (!Rec.Frame.Dynamic)
    return ctxBytePtr(B, Ctx, continuationRecordOffset(A, Rec) + FieldOffset);
  Value *Record = dynamicContinuationRecordPtr(B, A, Ctx, Rec);
  return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Record,
                             constI64(B.getContext(), FieldOffset));
}

Value *continuationXorPtr(IRBuilder<> &B, MFLAArtifacts &A,
                          ContinuationRecord Rec) {
  return continuationFieldPtr(B, A, ctxArg(A), Rec, 0);
}

Value *continuationEdgePtr(IRBuilder<> &B, MFLAArtifacts &A,
                           ContinuationRecord Rec) {
  return continuationFieldPtr(B, A, ctxArg(A), Rec, 8);
}

Value *continuationXorPtr(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                          ContinuationRecord Rec) {
  return continuationFieldPtr(B, A, Ctx, Rec, 0);
}

Value *continuationEdgePtr(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                           ContinuationRecord Rec) {
  return continuationFieldPtr(B, A, Ctx, Rec, 8);
}

ContinuationRecord continuationRecord(const FunctionLayout &L) {
  return {L.ContSlot, currentFrame(L), {}, nullptr, {}, false};
}

ContinuationRecord continuationRecord(const FunctionLayout &L, FrameRef Frame) {
  return {L.ContSlot, Frame, {}, nullptr, {}, false};
}

ContinuationRecord continuationRecord(const FunctionLayout &L,
                                      BasicBlock *ResumeBlock,
                                      FrameRef Frame,
                                      FrameRef CallerFrame,
                                      StorageRef ResultSlot,
                                      bool HasResultSlot) {
  return {L.ContSlot, Frame, CallerFrame, ResumeBlock, ResultSlot,
          HasResultSlot};
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

Value *loadContinuationCallerFrame(IRBuilder<> &B, MFLAArtifacts &A,
                                   ContinuationRecord Rec, StringRef Name) {
  return B.CreateLoad(Type::getInt64Ty(A.Mega->getContext()),
                      continuationFieldPtr(B, A, ctxArg(A), Rec, 16), Name);
}

void storeContinuation(IRBuilder<> &B, MFLAArtifacts &A, ContinuationRecord Rec,
                       uint64_t CalleeEntryState, uint64_t ResumeState,
                       Value *ContEdge) {
  assert(Rec.ResumeBlock && "push continuation requires a resume block");
  B.CreateStore(constI64(A.Mega->getContext(), CalleeEntryState ^ ResumeState),
                continuationXorPtr(B, A, Rec));
  B.CreateStore(ContEdge, continuationEdgePtr(B, A, Rec));
  Value *CallerToken = Rec.CallerFrame.Token
                           ? Rec.CallerFrame.Token
                           : constI64(A.Mega->getContext(), 0);
  B.CreateStore(CallerToken,
                continuationFieldPtr(B, A, ctxArg(A), Rec, 16));
}

void clearContinuation(IRBuilder<> &B, MFLAArtifacts &A,
                       ContinuationRecord Rec) {
  B.CreateStore(constI64(A.Mega->getContext(), 0),
                continuationXorPtr(B, A, Rec));
  B.CreateStore(constI64(A.Mega->getContext(), 0),
                continuationEdgePtr(B, A, Rec));
  B.CreateStore(constI64(A.Mega->getContext(), 0),
                continuationFieldPtr(B, A, ctxArg(A), Rec, 16));
}

void initializeContinuation(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                            ContinuationRecord Rec) {
  B.CreateStore(ConstantInt::get(Type::getInt64Ty(A.Mega->getContext()), 0),
                continuationXorPtr(B, A, Ctx, Rec));
  B.CreateStore(ConstantInt::get(Type::getInt64Ty(A.Mega->getContext()), 0),
                continuationEdgePtr(B, A, Ctx, Rec));
  B.CreateStore(ConstantInt::get(Type::getInt64Ty(A.Mega->getContext()), 0),
                continuationFieldPtr(B, A, Ctx, Rec, 16));
}

Value *loadState(IRBuilder<> &B, MFLAArtifacts &A, StringRef Name) {
  return B.CreateLoad(Type::getInt64Ty(A.Mega->getContext()),
                      ctxBytePtr(B, A, A.StateOffset), Name);
}

void storeState(IRBuilder<> &B, MFLAArtifacts &A, Value *State) {
  B.CreateStore(State, ctxBytePtr(B, A, A.StateOffset));
}

} // namespace yansollvm::mfla
