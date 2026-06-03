#include "Flattening.h"
#include "YANSOllvmCommon.h"
#include "CryptoUtils.h"
#include "Utils.h"
#include "YANSOllvmSeed.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

#define DEBUG_TYPE "flattening"
STATISTIC(Flattened, "Functions flattened");

PreservedAnalyses FlatteningPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  Function *Fn = &F;
  if (toObfuscate(flag, Fn, "fla")) {
    INIT_CONTEXT(F);
    if (flatten(*Fn)) {
      ++Flattened;
      return PreservedAnalyses::none();
    }
  }
  return PreservedAnalyses::all();
}

bool FlatteningPass::flatten(Function &F) {
  if (F.isVarArg()) {
    YANSO_WARN_FUNCTION("fla", F, "vararg function");
    return false;
  }

  if (F.hasPersonalityFn()) {
    YANSO_WARN_FUNCTION("fla", F,
                        "personality/EH function until EH regions are modeled");
    return false;
  }

  if (F.size() <= 2) {
    return false;
  }

  yansollvm_fix_stack(&F);

  vector<BasicBlock *> TrivialBlocks;
  BasicBlock *OriginalEntry = &F.getEntryBlock();
  for (BasicBlock &BB : F) {
    if (&BB == OriginalEntry)
      continue;
    if (BB.size() == 1 && isa<BranchInst>(BB.getTerminator()) &&
        BB.getTerminator()->getNumSuccessors() == 1 && !BB.hasAddressTaken())
      TrivialBlocks.push_back(&BB);
  }
  for (BasicBlock *BB : TrivialBlocks) {
    if (BB->hasNPredecessors(1))
      BB->replaceSuccessorsPhiUsesWith(BB->getSinglePredecessor());
    vector<BasicBlock *> TrivialPreds;
    for (BasicBlock *Pred : predecessors(BB)) {
      TrivialPreds.push_back(Pred);
    }
    for (BasicBlock *Pred : TrivialPreds) {
      auto OI = Pred->getTerminator()->op_begin();
      auto OE = Pred->getTerminator()->op_end();
      for (; OI != OE; OI++) {
        if (*OI == BB) {
          *OI = BB->getTerminator()->getSuccessor(0);
          break;
        }
      }
    }
    BB->eraseFromParent();
  }

  // Keep all original blocks except the entry block.
  vector<BasicBlock *> FlattenBlocks;
  BasicBlock &EntryBB = F.getEntryBlock();
  BasicBlock *EntryTarget = EntryBB.getTerminator()->getNumSuccessors() > 0
                                ? EntryBB.getTerminator()->getSuccessor(0)
                                : nullptr;
  for (BasicBlock &BB : F) {
    if (&BB != &EntryBB)
      FlattenBlocks.push_back(&BB);
  }
  // Split a multi-successor entry so the dispatcher can own the entry edge.
  Instruction *EntryTerminator = EntryBB.getTerminator();
  if (EntryTerminator->getNumSuccessors() > 1) {
    BasicBlock *EntryRegion =
        EntryBB.splitBasicBlock(EntryTerminator, "entry.region");
    FlattenBlocks.insert(FlattenBlocks.begin(), EntryRegion);
    EntryTarget = EntryRegion;
  }

  set<BasicBlock *> LocalOnlyBlocks;
  DominatorTree DT(F);
  for (BasicBlock *BB : FlattenBlocks) {
    Instruction *Term = BB->getTerminator();
    if (isa<IndirectBrInst>(Term)) {
      YANSO_ERROR_FUNCTION("fla", F, "contains indirectbr");
      return false;
    }
    if (BB->hasAddressTaken()) {
      LocalOnlyBlocks.insert(BB);
      YANSO_WARN_BLOCK("fla", F, *BB, "address-taken block");
    }
    for (Instruction &I : *BB) {
      if (PHINode *P = dyn_cast<PHINode>(&I)) {
        for (unsigned i = 0, e = P->getNumIncomingValues(); i < e; ++i) {
          if (InvokeInst *II = dyn_cast<InvokeInst>(P->getIncomingValue(i))) {
            if (II->getParent() == P->getIncomingBlock(i)) {
              LocalOnlyBlocks.insert(BB);
              YANSO_ERROR_BLOCK("fla", F, *BB,
                               "PHI incoming value is invoke result");
            }
          }
        }
      } else if (I.isUsedOutsideOfBlock(BB) && !I.getType()->isSized()) {
        for (Use &U : I.uses()) {
          if (Instruction *II = dyn_cast<Instruction>(U.getUser())) {
            BasicBlock *UseBB = nullptr;
            if (PHINode *PN = dyn_cast<PHINode>(II))
              UseBB = PN->getIncomingBlock(U);
            else
              UseBB = II->getParent();
            if (UseBB != BB) {
              if (UseBB)
                LocalOnlyBlocks.insert(UseBB);
              YANSO_ERROR_BLOCK("fla", F, *BB,
                                "unsized value used across block boundary");
              for (BasicBlock *midBB : FlattenBlocks) {
                if (UseBB && midBB != BB && midBB != UseBB &&
                    DT.dominates(midBB, UseBB) && DT.dominates(BB, midBB)) {
                  LocalOnlyBlocks.insert(midBB);
                  YANSO_ERROR_BLOCK(
                      "fla", F, *midBB,
                      "dominates local-only block using unsized cross-block value");
                }
              }
            }
          }
        }
      }
    }
    Instruction *term = BB->getTerminator();
    if (auto *II = dyn_cast<InvokeInst>(term)) {
      LocalOnlyBlocks.insert(II->getUnwindDest());
      YANSO_WARN_BLOCK("fla", F, *II->getUnwindDest(), "invoke unwind destination");
    } else if (auto *CRI = dyn_cast<CleanupReturnInst>(term)) {
      LocalOnlyBlocks.insert(CRI->getUnwindDest());
      YANSO_WARN_BLOCK("fla", F, *CRI->getUnwindDest(),
                       "cleanupret unwind destination");
    } else if (auto *CSI = dyn_cast<CatchSwitchInst>(term)) {
      LocalOnlyBlocks.insert(CSI->getUnwindDest());
      YANSO_WARN_BLOCK("fla", F, *CSI->getUnwindDest(),
                       "catchswitch unwind destination");
    }
  }

  // Create dispatcher block.
  BasicBlock *DispatcherBB =
      BasicBlock::Create(*CONTEXT, "DispatcherBB", &F, &EntryBB);
  // Replace the entry terminator with a branch to the dispatcher.
  EntryBB.moveBefore(DispatcherBB);
  EntryBB.getTerminator()->eraseFromParent();
  BranchInst *EntryToDispatcher = BranchInst::Create(DispatcherBB, &EntryBB);

  // Dual-state dispatcher: keep CFG safety handling, but dispatch on
  // mix(index, hash-state) instead of the raw switch state. Successor state is
  // encoded branchlessly with xor/mask where both successors are flattenable.
  YansoRNG RNG(yanso_function_seed(F, "fla"));
  vector<uint64_t> StateIds(FlattenBlocks.size());
  vector<uint64_t> DispatchHashes(FlattenBlocks.size());
  unordered_set<uint64_t> UsedIndex;
  unordered_set<uint64_t> UsedHash;
  unordered_map<BasicBlock *, size_t> BlockToIndex;
  for (size_t I = 0; I < FlattenBlocks.size(); ++I) {
    BlockToIndex[FlattenBlocks[I]] = I;

    bool Accepted = false;
    for (unsigned Attempt = 0; Attempt < 1024 && !Accepted; ++Attempt) {
      uint64_t Index = 0;
      do {
        Index = RNG.next64();
      } while (!Index || UsedIndex.count(Index));

      SmallVector<uint64_t, 4> RoundHashes;
      uint64_t CaseHash = yanso_mix64(Index, YansoMixBasis);
      RoundHashes.push_back(CaseHash);
      size_t ExtraRounds = RNG.range(4);
      for (size_t R = 0; R < ExtraRounds; ++R) {
        CaseHash = yanso_mix64(Index, CaseHash);
        RoundHashes.push_back(CaseHash);
      }

      bool Collides = false;
      for (uint64_t H : RoundHashes) {
        if (UsedHash.count(H)) {
          Collides = true;
          break;
        }
      }
      if (Collides)
        continue;

      UsedIndex.insert(Index);
      for (uint64_t H : RoundHashes)
        UsedHash.insert(H);
      StateIds[I] = Index;
      DispatchHashes[I] = RoundHashes.back();
      Accepted = true;
    }
    if (!Accepted)
      report_fatal_error("failed to allocate unique flattening dispatch hash");
  }

  size_t EntryIndex = 0;
  if (EntryTarget) {
    auto It = BlockToIndex.find(EntryTarget);
    if (It != BlockToIndex.end())
      EntryIndex = It->second;
  }

  AllocaInst *HashStatePtr =
      new AllocaInst(TYPE_I64, 0, "hashState.ptr", it(EntryToDispatcher));
  new StoreInst(CONST_I64(YansoMixBasis), HashStatePtr, false, Align(8),
                it(EntryToDispatcher));
  AllocaInst *StatePtr =
      new AllocaInst(TYPE_I64, 0, "state.ptr", it(EntryToDispatcher));
  new StoreInst(CONST_I64(StateIds[EntryIndex]), StatePtr, false, Align(8),
                it(EntryToDispatcher));

  // Load state, hash it, and switch on the hash value.
  LoadInst *StateLoad =
      new LoadInst(TYPE_I64, StatePtr, "state", false, Align(8), DispatcherBB);
  LoadInst *HashStateLoad = new LoadInst(TYPE_I64, HashStatePtr, "hashState",
                                         false, Align(8), DispatcherBB);
  Value *DispatchHash = yanso_create_mix64_ir(StateLoad, HashStateLoad,
                                              DispatcherBB, *F.getParent());
  new StoreInst(DispatchHash, HashStatePtr, false, Align(8), DispatcherBB);
  BasicBlock *HashMissBB =
      BasicBlock::Create(*CONTEXT, "hashMissBB", &F, DispatcherBB);
  BranchInst::Create(DispatcherBB, HashMissBB);
  SwitchInst *DispatcherSwitch =
      SwitchInst::Create(DispatchHash, HashMissBB, 0, DispatcherBB);

  vector<size_t> BlockOrder(FlattenBlocks.size());
  iota(BlockOrder.begin(), BlockOrder.end(), 0);
  RNG.shuffle(BlockOrder);
  for (size_t SeqIdx : BlockOrder) {
    BasicBlock *BB = FlattenBlocks[SeqIdx];
    BB->moveBefore(DispatcherBB);
    if (LocalOnlyBlocks.find(BB) == LocalOnlyBlocks.end())
      DispatcherSwitch->addCase(CONST_I64(DispatchHashes[SeqIdx]), BB);
  }

  // Rewrite original terminators to update state and return to dispatcher.
  for (size_t CurIndex = 0; CurIndex < FlattenBlocks.size(); ++CurIndex) {
    BasicBlock *BB = FlattenBlocks[CurIndex];
    Instruction *term = BB->getTerminator();
    if (term->getNumSuccessors() == 0)
      continue;

    auto FindDispatcherTarget = [&](BasicBlock *Succ,
                                    size_t &OutIndex) -> bool {
      auto It = BlockToIndex.find(Succ);
      if (It == BlockToIndex.end()) {
        YANSO_ERROR_EDGE("fla", F, *BB, *Succ,
                         "successor is outside flatten block set");
        return false;
      }
      if (LocalOnlyBlocks.find(Succ) != LocalOnlyBlocks.end())
        return false;
      OutIndex = It->second;
      return true;
    };
    auto CreateEncodedState = [&](size_t FalseIndex, size_t TrueIndex,
                                  Value *Cond,
                                  Instruction *InsertBefore) -> Value * {
      uint64_t RandomXor = RNG.next64();
      LoadInst *CurState = new LoadInst(TYPE_I64, StatePtr, "state.cur", false,
                                        Align(8), it(InsertBefore));
      Value *TempVal = BinaryOperator::CreateXor(CONST_I64(RandomXor), CurState,
                                                 "", it(InsertBefore));
      vector<size_t> ShuffledBlocks = BlockOrder;
      RNG.shuffle(ShuffledBlocks);
      int GarbageInterval = ShuffledBlocks.size() / 2;
      GarbageInterval = GarbageInterval > 1 ? GarbageInterval : 1;
      for (size_t D : ShuffledBlocks) {
        if (D == FalseIndex) {
          TempVal = BinaryOperator::CreateXor(
              CONST_I64(StateIds[CurIndex] ^ StateIds[FalseIndex] ^ RandomXor),
              TempVal, "", it(InsertBefore));
        } else if (D == TrueIndex) {
          Value *MaskVal = BinaryOperator::CreateAnd(
              new SExtInst(Cond, TYPE_I64, "", it(InsertBefore)),
              CONST_I64(StateIds[TrueIndex] ^ StateIds[FalseIndex]), "",
              it(InsertBefore));
          TempVal = BinaryOperator::CreateXor(MaskVal, TempVal, "",
                                              it(InsertBefore));
        } else if (RNG.range(GarbageInterval) == 0) {
          Value *MaskVal = BinaryOperator::CreateAnd(
              CONST_I64(0), CONST_I64(RNG.next64()), "", it(InsertBefore));
          TempVal = BinaryOperator::CreateXor(MaskVal, TempVal, "",
                                              it(InsertBefore));
        }
      }
      return TempVal;
    };

    auto StoreStateTransition = [&](size_t SuccIndex,
                                    Instruction *InsertBefore) {
      Value *Next = CreateEncodedState(SuccIndex, CurIndex,
                                       ConstantInt::getFalse(F.getContext()),
                                       InsertBefore);
      new StoreInst(Next, StatePtr, false, Align(8), it(InsertBefore));
      new StoreInst(CONST_I64(YansoMixBasis), HashStatePtr, false, Align(8),
                    it(InsertBefore));
    };

    if (term->getNumSuccessors() == 1 && isa<BranchInst>(term)) {
      BasicBlock *Succ = term->getSuccessor(0);
      size_t FalseIndex = 0;
      if (!FindDispatcherTarget(Succ, FalseIndex)) {
        if (LocalOnlyBlocks.find(Succ) != LocalOnlyBlocks.end())
          YANSO_ERROR_EDGE("fla", F, *BB, *Succ,
                          "successor is local-only, leaving direct branch");
        continue;
      }
      StoreStateTransition(FalseIndex, term);
      term->eraseFromParent();
      BranchInst::Create(DispatcherBB, BB);
    } else if (term->getNumSuccessors() == 2 && isa<BranchInst>(term)) {
      BranchInst *BR = cast<BranchInst>(term);
      BasicBlock *TrueSucc = BR->getSuccessor(0);
      BasicBlock *FalseSucc = BR->getSuccessor(1);
      size_t TrueIndex = 0, FalseIndex = 0;
      bool TrueFlat = FindDispatcherTarget(TrueSucc, TrueIndex);
      bool FalseFlat = FindDispatcherTarget(FalseSucc, FalseIndex);
      if (TrueFlat && FalseFlat) {
        Value *Next =
            CreateEncodedState(FalseIndex, TrueIndex, BR->getCondition(), BR);
        new StoreInst(Next, StatePtr, false, Align(8), it(BR));
        new StoreInst(CONST_I64(YansoMixBasis), HashStatePtr, false, Align(8),
                      it(BR));
        BR->eraseFromParent();
        BranchInst::Create(DispatcherBB, BB);
      } else if (TrueFlat || FalseFlat) {
        BasicBlock *DirectDest = TrueFlat ? FalseSucc : TrueSucc;
        YANSO_ERROR_EDGE(
            "fla", F, *BB, *DirectDest,
            "one conditional successor is local-only/direct; keeping mixed branch");
        size_t FlatIndex = TrueFlat ? TrueIndex : FalseIndex;
        Value *FlatCond =
            TrueFlat
                ? BR->getCondition()
                : BinaryOperator::CreateNot(BR->getCondition(), "", it(BR));
        StoreStateTransition(FlatIndex, BR);
        BR->eraseFromParent();
        BranchInst::Create(DispatcherBB, DirectDest, FlatCond, BB);
      }
    } else if (SwitchInst *SI = dyn_cast<SwitchInst>(term)) {
      unordered_map<BasicBlock *, BasicBlock *> TransitionForTarget;
      auto GetSwitchSuccessor = [&](BasicBlock *Succ) -> BasicBlock * {
        size_t SuccIndex = 0;
        if (!FindDispatcherTarget(Succ, SuccIndex))
          return Succ;
        auto It = TransitionForTarget.find(Succ);
        if (It != TransitionForTarget.end())
          return It->second;

        // Preserve address-taken labels: blockaddress constants must still land
        // on the original label body, not on the dispatcher. Rewriting their
        // terminators makes an indirect goto jump into the flattened state
        // machine with a stale state value and can loop in hashMissBB.
        if (BB->hasAddressTaken()) {
          YANSO_WARN_EDGE("fla", F, *BB, *Succ,
                          "switch source is address-taken; preserving successor");
          return Succ;
        }

        BasicBlock *TransitionBB =
            BasicBlock::Create(*CONTEXT, "switch.trans", &F, DispatcherBB);
        BranchInst *ToDispatch = BranchInst::Create(DispatcherBB, TransitionBB);
        StoreStateTransition(SuccIndex, ToDispatch);
        TransitionForTarget[Succ] = TransitionBB;
        return TransitionBB;
      };

      BasicBlock *NewDefault = GetSwitchSuccessor(SI->getDefaultDest());
      SwitchInst *NewSwitch = SwitchInst::Create(SI->getCondition(), NewDefault,
                                                 SI->getNumCases(), it(SI));
      for (auto Case : SI->cases())
        NewSwitch->addCase(Case.getCaseValue(),
                           GetSwitchSuccessor(Case.getCaseSuccessor()));
      SI->eraseFromParent();
    }
  }
  return true;
}
