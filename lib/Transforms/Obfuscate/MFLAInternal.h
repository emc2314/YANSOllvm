#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <cstdint>

namespace yansollvm::mfla {

struct ContinuationSlot {
  unsigned ID = 0;
};

inline constexpr uint64_t FrameFreeNextOffsetValue = 0;
inline constexpr uint64_t FrameMetadataBytes = 16;

enum class StorageKind {
  Argument,
  ReturnValue,
  Phi,
  Spill,
  CallResult,
  FixedAllocaObject,
  ContinuationRecord,
  FrameMetadata,
  PageTableMetadata,
};

struct StorageRef {
  llvm::Type *Ty = nullptr;
  uint64_t Offset = 0;
  uint64_t SizeBytes = 0;
  llvm::Align Alignment = llvm::Align(1);
  StorageKind Kind = StorageKind::Spill;
};

inline StorageRef staticRef(llvm::Type *Ty, uint64_t Offset) {
  return {Ty, Offset, 0, llvm::Align(1), StorageKind::Spill};
}

enum class FramePageBackendKind {
  Static,
  Malloc,
};

struct FrameRef {
  llvm::Function *Owner = nullptr;
  llvm::Value *Token = nullptr;
};

struct ContinuationRecord {
  ContinuationSlot Slot;
  FrameRef Frame;
  FrameRef CallerFrame;
  llvm::BasicBlock *ResumeBlock = nullptr;
  StorageRef ResultSlot;
  bool HasResultSlot = false;
};

struct MFLAArtifacts {
  llvm::Function *Mega = nullptr;
  uint64_t FrameOffset = 0;
  uint64_t CurrentFrameOffset = 0;
  uint64_t FrameTopOffset = 0;
  uint64_t FrameFreeHeadOffset = 0;
  uint64_t FramePageTableHeadOffset = 0;
  uint64_t FrameArenaOffset = 0;
  uint64_t FrameStride = 0;
  uint64_t StaticFramePages = 0;
  FramePageBackendKind FrameBackend = FramePageBackendKind::Static;
  uint64_t FramesPerPage = 0;
  uint64_t FramePageSize = 0;
  uint64_t PageTableBlockEntries = 0;
  uint64_t PageTableBlockBytes = 0;
  llvm::Function *FramePageResolver = nullptr;
  uint64_t ContinuationOffset = 0;
  uint64_t ContinuationStride = 0;
  uint64_t ContinuationSlots = 0;
  uint64_t ReturnedFrameOffset = 0;
  uint64_t StateOffset = 0;
  uint64_t CtxSize = 0;
  uint64_t StateKeySalt = 0;
  llvm::DenseMap<llvm::Function *, uint64_t> EntryStateForFunction;
  llvm::DenseMap<llvm::Function *, llvm::GlobalVariable *> EntryEdgeForFunction;
  llvm::DenseMap<llvm::GlobalVariable *, llvm::GlobalVariable *> GlobalRemaps;
  llvm::SmallVector<llvm::GlobalVariable *, 8> EdgeConstants;
  llvm::SmallVector<llvm::GlobalVariable *, 8> RemappedGlobals;

  void eraseFromParent();
};

struct FunctionLayout {
  ContinuationSlot ContSlot;
  llvm::Function *Owner = nullptr;
  llvm::SmallVector<StorageRef, 4> ArgOffsets;
  llvm::DenseMap<llvm::Argument *, StorageRef> ArgOffsetFor;
  StorageRef RetOffset;
  llvm::DenseMap<llvm::PHINode *, StorageRef> PhiOffsets;
  llvm::DenseMap<llvm::CallInst *, StorageRef> CallResultOffsets;
  llvm::DenseMap<llvm::Instruction *, StorageRef> SpilledValueOffsets;
};

struct FrameLayoutBuilder {
  const llvm::DataLayout &DL;
  uint64_t NextOffset = 0;

  explicit FrameLayoutBuilder(const llvm::DataLayout &DL) : DL(DL) {}
  StorageRef reserve(StorageKind Kind, llvm::Type *Ty);
  StorageRef reserveValue(llvm::Type *Ty);
  StorageRef reserveFixedObject(llvm::Type *PtrTy, llvm::Type *ObjectTy,
                                llvm::Align Alignment);
  uint64_t frameSize() const;
};

void addBaseToLayout(FunctionLayout &L, uint64_t Base);
llvm::Constant *keyForState(MFLAArtifacts &A, uint64_t State);
llvm::Argument *ctxArg(MFLAArtifacts &A);
llvm::Value *ctxBytePtr(llvm::IRBuilder<> &B, llvm::Value *Ctx,
                        uint64_t Offset);
llvm::Value *ctxBytePtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                        uint64_t Offset);

FrameRef currentFrame(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                      const FunctionLayout &L);
FrameRef frameWithToken(const FunctionLayout &L, llvm::Value *Token);
FrameRef rootFrame(const FunctionLayout &L, llvm::LLVMContext &Ctx);
llvm::Value *loadCurrentFrameToken(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                   llvm::StringRef Name = "mfla.frame.cur");
void storeCurrentFrameToken(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                            llvm::Value *Token);
llvm::Value *loadFrameTop(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                          llvm::StringRef Name = "mfla.frame.top");
void storeFrameTop(llvm::IRBuilder<> &B, MFLAArtifacts &A, llvm::Value *Top);
llvm::Value *loadFrameFreeHead(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                               llvm::StringRef Name = "mfla.frame.free.head");
void storeFrameFreeHead(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                        llvm::Value *Head);
llvm::Value *loadFramePageTableHead(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                    llvm::Value *Ctx,
                                    llvm::StringRef Name = "mfla.frame.pages");
llvm::Value *loadFramePageTableHead(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                    llvm::StringRef Name = "mfla.frame.pages");
void storeFramePageTableHead(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                             llvm::Value *Head);
llvm::Value *resolveFramePage(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                              llvm::Value *PageIndex);
llvm::Value *resolveFramePage(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                              llvm::Value *Ctx, llvm::Value *PageIndex);
llvm::Value *loadFrameFreeNext(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                               llvm::Value *Token,
                               llvm::StringRef Name = "mfla.frame.free.next");
void storeFrameFreeNext(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                        llvm::Value *Token, llvm::Value *Next);
FrameRef pushFrame(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                   const FunctionLayout &L);
void restoreFrame(llvm::IRBuilder<> &B, MFLAArtifacts &A, llvm::Value *Token);
llvm::Value *loadReturnedFrameToken(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                    llvm::StringRef Name = "mfla.ret.frame");
void storeReturnedFrameToken(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                             llvm::Value *Token);
llvm::Value *frameSlotPtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                          FrameRef Frame, StorageRef Ref);
llvm::Value *frameSlotPtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                          llvm::Value *Ctx, FrameRef Frame, StorageRef Ref);
llvm::LoadInst *loadFrameSlot(llvm::IRBuilder<> &B, llvm::Type *Ty,
                              MFLAArtifacts &A, FrameRef Frame,
                              StorageRef Ref, llvm::StringRef Name = "");
llvm::LoadInst *loadFrameSlot(llvm::IRBuilder<> &B, llvm::Type *Ty,
                              MFLAArtifacts &A, llvm::Value *Ctx,
                              FrameRef Frame, StorageRef Ref,
                              llvm::StringRef Name = "");
void storeFrameSlot(llvm::IRBuilder<> &B, llvm::Value *V, MFLAArtifacts &A,
                    FrameRef Frame, StorageRef Ref);
void storeFrameSlot(llvm::IRBuilder<> &B, llvm::Value *V, MFLAArtifacts &A,
                    llvm::Value *Ctx, FrameRef Frame, StorageRef Ref);
void cleanupFrameStorage(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                         llvm::Value *Ctx);
uint64_t continuationRecordOffset(MFLAArtifacts &A, ContinuationRecord Rec);
uint64_t continuationXorOffset(MFLAArtifacts &A, ContinuationRecord Rec);
uint64_t continuationEdgeOffset(MFLAArtifacts &A, ContinuationRecord Rec);
llvm::Value *continuationXorPtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                ContinuationRecord Rec);
llvm::Value *continuationEdgePtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                 ContinuationRecord Rec);
llvm::Value *continuationXorPtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                llvm::Value *Ctx, ContinuationRecord Rec);
llvm::Value *continuationEdgePtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                 llvm::Value *Ctx, ContinuationRecord Rec);
ContinuationRecord continuationRecord(const FunctionLayout &L);
ContinuationRecord continuationRecord(const FunctionLayout &L,
                                      FrameRef Frame);
ContinuationRecord continuationRecord(const FunctionLayout &L,
                                      llvm::BasicBlock *ResumeBlock,
                                      FrameRef Frame,
                                      FrameRef CallerFrame,
                                      StorageRef ResultSlot,
                                      bool HasResultSlot);
llvm::Value *loadContinuationXor(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                 ContinuationRecord Rec,
                                 llvm::StringRef Name = "mfla.cont.xor");
llvm::Value *loadContinuationEdge(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                  ContinuationRecord Rec,
                                  llvm::StringRef Name = "mfla.cont.edge");
llvm::Value *loadContinuationCallerFrame(
    llvm::IRBuilder<> &B, MFLAArtifacts &A, ContinuationRecord Rec,
    llvm::StringRef Name = "mfla.cont.caller.frame");
void storeContinuation(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                       ContinuationRecord Rec, uint64_t CalleeEntryState,
                       uint64_t ResumeState, llvm::Value *ContEdge);
void clearContinuation(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                       ContinuationRecord Rec);
void initializeContinuation(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                            llvm::Value *Ctx, ContinuationRecord Rec);
llvm::Value *loadState(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                       llvm::StringRef Name = "mfla.state");
void storeState(llvm::IRBuilder<> &B, MFLAArtifacts &A, llvm::Value *State);

} // namespace yansollvm::mfla

