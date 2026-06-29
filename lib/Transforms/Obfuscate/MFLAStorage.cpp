#include "MFLAInternal.h"
#include "CryptoUtils.h"
#include "Utils.h"

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
  if (FramePageResolver) {
    Function *F = FramePageResolver;
    FramePageResolver = nullptr;
    if (!F->use_empty())
      F->replaceAllUsesWith(UndefValue::get(F->getType()));
    F->eraseFromParent();
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

static uint64_t alignStorageOffset(uint64_t Offset, Align Alignment) {
  return llvm::alignTo(Offset, Alignment.value());
}

static Align storageAlign(Type *Ty, const DataLayout &DL) {
  Align A = DL.getABITypeAlign(Ty);
  if (auto *VTy = dyn_cast<FixedVectorType>(Ty))
    A = std::max(A, DL.getPrefTypeAlign(VTy));
  return A;
}

static uint64_t storageSlotSize(Type *Ty, const DataLayout &DL) {
  return DL.getTypeStoreSize(Ty).getFixedValue();
}

StorageRef FrameLayoutBuilder::reserve(StorageKind Kind, Type *Ty) {
  uint64_t Offset = alignStorageOffset(NextOffset, storageAlign(Ty, DL));
  uint64_t Size = storageSlotSize(Ty, DL);
  Align Alignment = storageAlign(Ty, DL);
  NextOffset = Offset + Size;
  return {Ty, Offset, Size, Alignment, Kind};
}

StorageRef FrameLayoutBuilder::reserveValue(Type *Ty) {
  return reserve(StorageKind::Spill, Ty);
}

StorageRef FrameLayoutBuilder::reserveFixedObject(Type *PtrTy, Type *ObjectTy,
                                                  Align Alignment) {
  TypeSize Size = DL.getTypeAllocSize(ObjectTy);
  assert(!Size.isScalable() && "scalable fixed object cannot be frame-reserved");
  Align ObjectAlign = std::max(Alignment, DL.getABITypeAlign(ObjectTy));
  uint64_t Offset = alignStorageOffset(NextOffset, ObjectAlign);
  uint64_t SizeBytes = Size.getFixedValue();
  NextOffset = Offset + SizeBytes;
  return {PtrTy, Offset, SizeBytes, ObjectAlign, StorageKind::FixedAllocaObject};
}

uint64_t FrameLayoutBuilder::frameSize() const {
  return std::max<uint64_t>(1, llvm::alignTo(NextOffset, 16));
}

static void addBaseToRef(StorageRef &Ref, uint64_t Base) { Ref.Offset += Base; }

void addBaseToLayout(FunctionLayout &L, uint64_t Base) {
  for (StorageRef &Ref : L.ArgOffsets)
    addBaseToRef(Ref, Base);
  for (auto &Entry : L.ArgOffsetFor)
    addBaseToRef(Entry.second, Base);
  if (L.RetOffset.Ty)
    addBaseToRef(L.RetOffset, Base);
  for (auto &Entry : L.PhiOffsets)
    addBaseToRef(Entry.second, Base);
  for (auto &Entry : L.CallResultOffsets)
    addBaseToRef(Entry.second, Base);
  for (auto &Entry : L.SpilledValueOffsets)
    addBaseToRef(Entry.second, Base);
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

static Type *tokenTy(LLVMContext &Ctx) { return Type::getInt64Ty(Ctx); }

FrameRef frameWithToken(const FunctionLayout &L, Value *Token) {
  return {L.Owner, Token};
}

FrameRef rootFrame(const FunctionLayout &L, LLVMContext &Ctx) {
  return frameWithToken(L, constI64(Ctx, 0));
}

FrameRef currentFrame(IRBuilder<> &B, MFLAArtifacts &A,
                      const FunctionLayout &L) {
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

Value *loadFrameFreeHead(IRBuilder<> &B, MFLAArtifacts &A, StringRef Name) {
  return B.CreateLoad(tokenTy(A.Mega->getContext()),
                      ctxBytePtr(B, A, A.FrameFreeHeadOffset), Name);
}

void storeFrameFreeHead(IRBuilder<> &B, MFLAArtifacts &A, Value *Head) {
  B.CreateStore(Head, ctxBytePtr(B, A, A.FrameFreeHeadOffset));
}

Value *loadFramePageTableHead(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                              StringRef Name) {
  return B.CreateLoad(PointerType::get(B.getContext(), 0),
                      ctxBytePtr(B, Ctx, A.FramePageTableHeadOffset), Name);
}

Value *loadFramePageTableHead(IRBuilder<> &B, MFLAArtifacts &A,
                              StringRef Name) {
  return loadFramePageTableHead(B, A, ctxArg(A), Name);
}

void storeFramePageTableHead(IRBuilder<> &B, MFLAArtifacts &A, Value *Head) {
  B.CreateStore(Head, ctxBytePtr(B, A, A.FramePageTableHeadOffset));
}

Value *resolveFramePage(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx,
                        Value *PageIndex) {
  if (A.FrameBackend == FramePageBackendKind::Static) {
    Value *PageOff = B.CreateMul(PageIndex,
                                 constI64(B.getContext(), A.FramePageSize),
                                 "mfla.static.page.off");
    Value *Off = B.CreateAdd(PageOff, constI64(B.getContext(), A.FrameArenaOffset),
                             "mfla.static.page.base.off");
    return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Ctx, Off,
                               "mfla.frame.page");
  }
  assert(A.FramePageResolver && "frame page resolver must be created");
  return B.CreateCall(A.FramePageResolver, {Ctx, PageIndex},
                      "mfla.frame.page");
}

Value *resolveFramePage(IRBuilder<> &B, MFLAArtifacts &A, Value *PageIndex) {
  return resolveFramePage(B, A, ctxArg(A), PageIndex);
}

static Value *frameRecordPtr(IRBuilder<> &B, MFLAArtifacts &A, Value *Token,
                             StringRef Name = "mfla.frame.record") {
  Type *I64 = Type::getInt64Ty(B.getContext());
  Value *Token64 = B.CreateZExtOrTrunc(Token, I64, "mfla.frame.record.token");
  Value *PageIndex =
      B.CreateUDiv(Token64, constI64(B.getContext(), A.FramesPerPage),
                   "mfla.frame.page.index");
  Value *SlotIndex =
      B.CreateURem(Token64, constI64(B.getContext(), A.FramesPerPage),
                   "mfla.frame.page.slot");
  Value *Page = resolveFramePage(B, A, PageIndex);
  Value *FrameOffset = B.CreateMul(SlotIndex,
                                   constI64(B.getContext(), A.FrameStride),
                                   "mfla.frame.record.off");
  return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Page,
                             FrameOffset, Name);
}

static Value *frameMetadataPtr(IRBuilder<> &B, MFLAArtifacts &A, Value *Token,
                               uint64_t Offset,
                               StringRef Name = "mfla.frame.meta") {
  Value *Record = frameRecordPtr(B, A, Token);
  return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Record,
                             constI64(B.getContext(), Offset), Name);
}

Value *loadFrameFreeNext(IRBuilder<> &B, MFLAArtifacts &A, Value *Token,
                         StringRef Name) {
  return B.CreateLoad(tokenTy(A.Mega->getContext()),
                      frameMetadataPtr(B, A, Token, FrameFreeNextOffsetValue),
                      Name);
}

void storeFrameFreeNext(IRBuilder<> &B, MFLAArtifacts &A, Value *Token,
                        Value *Next) {
  B.CreateStore(Next,
                frameMetadataPtr(B, A, Token, FrameFreeNextOffsetValue));
}

FrameRef pushFrame(IRBuilder<> &B, MFLAArtifacts &A,
                   const FunctionLayout &L) {
  Value *FreeHead = loadFrameFreeHead(B, A, "mfla.frame.alloc.free");
  Value *HasFree = B.CreateICmpNE(FreeHead, constI64(B.getContext(), 0),
                                  "mfla.frame.has.free");
  Value *FreshToken = loadFrameTop(B, A, "mfla.frame.alloc.top");
  Value *Token = B.CreateSelect(HasFree, FreeHead, FreshToken,
                                "mfla.frame.alloc.token");
  BasicBlock *Origin = B.GetInsertBlock();
  BasicBlock *ReuseBB = BasicBlock::Create(B.getContext(), "mfla.frame.reuse",
                                           A.Mega, A.Mega->back().getNextNode());
  BasicBlock *FreshBB = BasicBlock::Create(B.getContext(), "mfla.frame.fresh",
                                           A.Mega, A.Mega->back().getNextNode());
  BasicBlock *ContBB = BasicBlock::Create(B.getContext(), "mfla.frame.alloc.cont",
                                          A.Mega, A.Mega->back().getNextNode());
  B.CreateCondBr(HasFree, ReuseBB, FreshBB);

  IRBuilder<> ReuseB(ReuseBB);
  Value *FreeNext = loadFrameFreeNext(ReuseB, A, FreeHead);
  storeFrameFreeHead(ReuseB, A, FreeNext);
  ReuseB.CreateBr(ContBB);

  IRBuilder<> FreshB(FreshBB);
  Value *NextFresh = FreshB.CreateAdd(FreshToken, constI64(FreshB.getContext(), 1),
                                      "mfla.frame.next");
  storeFrameTop(FreshB, A, NextFresh);
  FreshB.CreateBr(ContBB);

  B.SetInsertPoint(ContBB);
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
  assert(Frame.Token && "frame slot requires a frame token");
  Type *I64 = Type::getInt64Ty(B.getContext());
  Value *Token = B.CreateZExtOrTrunc(Frame.Token, I64, "mfla.frame.token");
  Value *PageIndex =
      B.CreateUDiv(Token, constI64(B.getContext(), A.FramesPerPage),
                   "mfla.frame.slot.page.index");
  Value *SlotIndex =
      B.CreateURem(Token, constI64(B.getContext(), A.FramesPerPage),
                   "mfla.frame.slot.page.slot");
  Value *Page = resolveFramePage(B, A, Ctx, PageIndex);
  Value *Scaled = B.CreateMul(SlotIndex, constI64(B.getContext(), A.FrameStride),
                              "mfla.frame.off");
  Value *Base = B.CreateAdd(Scaled,
                            constI64(B.getContext(), FrameMetadataBytes +
                                                          Ref.Offset),
                            "mfla.frame.slot.off");
  return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Page, Base,
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

// Frame storage is only 16-aligned (page base + 16-multiple stride), so an
// over-aligned slot type (e.g. a 32-aligned vector) must not be tagged with
// its larger ABI alignment. Cap the access alignment at what storage offers.
static Align frameSlotAccessAlign(StorageRef Ref) {
  return commonAlignment(Align(16), FrameMetadataBytes + Ref.Offset);
}

LoadInst *loadFrameSlot(IRBuilder<> &B, Type *Ty, MFLAArtifacts &A,
                        FrameRef Frame, StorageRef Ref, StringRef Name) {
  return B.CreateAlignedLoad(Ty, frameSlotPtr(B, A, Frame, Ref),
                             frameSlotAccessAlign(Ref), Name);
}

LoadInst *loadFrameSlot(IRBuilder<> &B, Type *Ty, MFLAArtifacts &A, Value *Ctx,
                        FrameRef Frame, StorageRef Ref, StringRef Name) {
  return B.CreateAlignedLoad(Ty, frameSlotPtr(B, A, Ctx, Frame, Ref),
                             frameSlotAccessAlign(Ref), Name);
}

void storeFrameSlot(IRBuilder<> &B, Value *V, MFLAArtifacts &A,
                    FrameRef Frame, StorageRef Ref) {
  B.CreateAlignedStore(V, frameSlotPtr(B, A, Frame, Ref),
                       frameSlotAccessAlign(Ref));
}

void storeFrameSlot(IRBuilder<> &B, Value *V, MFLAArtifacts &A, Value *Ctx,
                    FrameRef Frame, StorageRef Ref) {
  B.CreateAlignedStore(V, frameSlotPtr(B, A, Ctx, Frame, Ref),
                       frameSlotAccessAlign(Ref));
}

void cleanupFrameStorage(IRBuilder<> &B, MFLAArtifacts &A, Value *Ctx) {
  if (A.FrameBackend != FramePageBackendKind::Malloc)
    return;

  LLVMContext &LLVMCtx = B.getContext();
  Type *I8 = Type::getInt8Ty(LLVMCtx);
  Type *PtrTy = PointerType::get(LLVMCtx, 0);
  Type *VoidTy = Type::getVoidTy(LLVMCtx);
  FunctionCallee Free = A.Mega->getParent()->getOrInsertFunction(
      "free", FunctionType::get(VoidTy, {PtrTy}, false));

  Value *Block = loadFramePageTableHead(B, A, Ctx, "mfla.cleanup.block");
  Value *HasBlock = B.CreateICmpNE(
      Block, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "mfla.cleanup.has.block");
  BasicBlock *Start = B.GetInsertBlock();
  Function *F = Start->getParent();
  BasicBlock *Loop = BasicBlock::Create(LLVMCtx, "mfla.cleanup.loop", F);
  BasicBlock *Done = BasicBlock::Create(LLVMCtx, "mfla.cleanup.done", F);
  B.CreateCondBr(HasBlock, Loop, Done);

  IRBuilder<> LoopB(Loop);
  PHINode *BlockPhi = LoopB.CreatePHI(PtrTy, 2, "mfla.cleanup.cur.block");
  BlockPhi->addIncoming(Block, Start);
  Value *NextBlock = LoopB.CreateLoad(PtrTy, BlockPhi, "mfla.cleanup.next.block");
  for (uint64_t I = 0; I != A.PageTableBlockEntries; ++I) {
    Value *SlotPtr = LoopB.CreateInBoundsGEP(
        I8, BlockPhi,
        constI64(LLVMCtx, 8 + I * A.Mega->getParent()->getDataLayout().getPointerSize()),
        "mfla.cleanup.page.slot");
    Value *Page = LoopB.CreateLoad(PtrTy, SlotPtr, "mfla.cleanup.page");
    Value *HasPage = LoopB.CreateICmpNE(
        Page, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
        "mfla.cleanup.has.page");
    BasicBlock *FreePageBB = BasicBlock::Create(LLVMCtx, "mfla.cleanup.free.page", F, Done);
    BasicBlock *AfterPageBB = BasicBlock::Create(LLVMCtx, "mfla.cleanup.after.page", F, Done);
    LoopB.CreateCondBr(HasPage, FreePageBB, AfterPageBB);
    IRBuilder<> FreePageB(FreePageBB);
    FreePageB.CreateCall(Free, {Page});
    FreePageB.CreateBr(AfterPageBB);
    LoopB.SetInsertPoint(AfterPageBB);
  }
  LoopB.CreateCall(Free, {BlockPhi});
  Value *HasNext = LoopB.CreateICmpNE(
      NextBlock, ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "mfla.cleanup.has.next");
  LoopB.CreateCondBr(HasNext, Loop, Done);
  BlockPhi->addIncoming(NextBlock, LoopB.GetInsertBlock());

  B.SetInsertPoint(Done);
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

static Value *dynamicContinuationRecordPtr(IRBuilder<> &B, MFLAArtifacts &A,
                                           Value *Ctx,
                                           ContinuationRecord Rec) {
  assert(Rec.Frame.Token && "continuation requires frame token");
  Type *I64 = Type::getInt64Ty(B.getContext());
  Value *Token = B.CreateZExtOrTrunc(Rec.Frame.Token, I64, "mfla.cont.frame");
  Value *PageIndex =
      B.CreateUDiv(Token, constI64(B.getContext(), A.FramesPerPage),
                   "mfla.cont.page.index");
  Value *SlotIndex =
      B.CreateURem(Token, constI64(B.getContext(), A.FramesPerPage),
                   "mfla.cont.page.slot");
  // Continuation slots are addressed as (frame slot within page, continuation slot).
  // A reused same-page frame token must not alias another slot's continuation.
  Value *RecordIndex =
      B.CreateAdd(B.CreateMul(SlotIndex,
                              constI64(B.getContext(), A.ContinuationSlots),
                              "mfla.cont.frame.base"),
                  constI64(B.getContext(), Rec.Slot.ID), "mfla.cont.index");
  Value *Scaled = B.CreateMul(RecordIndex,
                              constI64(B.getContext(), A.ContinuationStride),
                              "mfla.cont.off");
  Value *Base = B.CreateAdd(Scaled, constI64(B.getContext(), A.ContinuationOffset),
                            "mfla.cont.record.off");
  Value *Page = resolveFramePage(B, A, Ctx, PageIndex);
  return B.CreateInBoundsGEP(Type::getInt8Ty(B.getContext()), Page, Base,
                             "mfla.cont.record");
}

static Value *continuationFieldPtr(IRBuilder<> &B, MFLAArtifacts &A,
                                   Value *Ctx, ContinuationRecord Rec,
                                   uint64_t FieldOffset) {
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
  return {L.ContSlot, rootFrame(L, L.Owner->getContext()), {}, nullptr, {}, false};
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
