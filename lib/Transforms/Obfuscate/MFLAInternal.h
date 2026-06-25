#ifndef YANSOLLVM_MFLA_INTERNAL_H
#define YANSOLLVM_MFLA_INTERNAL_H

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

struct StorageRef {
  llvm::Type *Ty = nullptr;
  uint64_t Offset = 0;
};

inline StorageRef staticRef(llvm::Type *Ty, uint64_t Offset) {
  return {Ty, Offset};
}

struct ContinuationRecord {
  ContinuationSlot Slot;
  llvm::BasicBlock *ResumeBlock = nullptr;
  StorageRef ResultSlot;
  bool HasResultSlot = false;
};

struct MFLAArtifacts {
  llvm::Function *Mega = nullptr;
  uint64_t FrameOffset = 0;
  uint64_t ContinuationOffset = 0;
  uint64_t ContinuationStride = 0;
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

llvm::Constant *keyForState(MFLAArtifacts &A, uint64_t State);
llvm::Argument *ctxArg(MFLAArtifacts &A);
llvm::Value *ctxBytePtr(llvm::IRBuilder<> &B, llvm::Value *Ctx,
                        uint64_t Offset);
llvm::Value *ctxBytePtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                        uint64_t Offset);

llvm::Value *storagePtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                        StorageRef Ref);
llvm::Value *storagePtr(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                        llvm::Value *Ctx, StorageRef Ref);
llvm::LoadInst *loadSlot(llvm::IRBuilder<> &B, llvm::Type *Ty,
                         MFLAArtifacts &A, StorageRef Ref,
                         llvm::StringRef Name = "");
llvm::LoadInst *loadSlot(llvm::IRBuilder<> &B, llvm::Type *Ty,
                         MFLAArtifacts &A, llvm::Value *Ctx, StorageRef Ref,
                         llvm::StringRef Name = "");
void storeSlot(llvm::IRBuilder<> &B, llvm::Value *V, MFLAArtifacts &A,
               StorageRef Ref);
void storeSlot(llvm::IRBuilder<> &B, llvm::Value *V, MFLAArtifacts &A,
               llvm::Value *Ctx, StorageRef Ref);

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
                                      llvm::BasicBlock *ResumeBlock,
                                      StorageRef ResultSlot,
                                      bool HasResultSlot);
llvm::Value *loadContinuationXor(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                 ContinuationRecord Rec,
                                 llvm::StringRef Name = "mfla.cont.xor");
llvm::Value *loadContinuationEdge(llvm::IRBuilder<> &B, MFLAArtifacts &A,
                                  ContinuationRecord Rec,
                                  llvm::StringRef Name = "mfla.cont.edge");
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

#endif // YANSOLLVM_MFLA_INTERNAL_H
