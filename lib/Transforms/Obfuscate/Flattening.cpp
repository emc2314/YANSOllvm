#include "Flattening.h"
#include "CryptoUtils.h"
#include "Utils.h"
#include "YANSOllvmCommon.h"
#include "YANSOllvmSeed.h"

#include <algorithm>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <unordered_set>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "flattening"
STATISTIC(Flattened, "Functions flattened");

namespace {

struct EntryKey {
  unsigned Region = 0;
  unsigned Index = 0;

  bool operator==(const EntryKey &Other) const {
    return Region == Other.Region && Index == Other.Index;
  }
  bool operator<(const EntryKey &Other) const {
    return Region < Other.Region ||
           (Region == Other.Region && Index < Other.Index);
  }
};

struct Region {
  struct Entry {
    BasicBlock *BB = nullptr;
    uint64_t StateId = 0;
    uint64_t DispatchHash = 0;
  };

  struct Exit {
    BasicBlock *BB = nullptr;
    std::optional<EntryKey> KnownSource;
  };

  std::vector<BasicBlock *> Blocks;
  std::vector<Entry> Entries;
  std::vector<Exit> Exits;
};

struct FlattenPlan {
  Function &F;
  BasicBlock *EntryBB = nullptr;
  BasicBlock *EntryTarget = nullptr;

  std::vector<BasicBlock *> Blocks;
  DenseMap<BasicBlock *, unsigned> BlockIndex;
  std::vector<unsigned> Parent;
  std::set<std::pair<BasicBlock *, BasicBlock *>> AtomicEdges;
  std::set<BasicBlock *> BlockedEntries;

  std::vector<Region> Regions;
  DenseMap<BasicBlock *, unsigned> RegionOf;
  DenseMap<BasicBlock *, EntryKey> EntryOfBlock;

  explicit FlattenPlan(Function &F) : F(F) {}
};

struct Dispatcher {
  BasicBlock *BB = nullptr;
  BasicBlock *HashMissBB = nullptr;
  AllocaInst *StatePtr = nullptr;
  AllocaInst *HashStatePtr = nullptr;
  SwitchInst *Switch = nullptr;
};

static unsigned findRoot(FlattenPlan &P, unsigned I) {
  while (P.Parent[I] != I) {
    P.Parent[I] = P.Parent[P.Parent[I]];
    I = P.Parent[I];
  }
  return I;
}

static unsigned findRoot(const FlattenPlan &P, unsigned I) {
  while (P.Parent[I] != I)
    I = P.Parent[I];
  return I;
}

static bool inDomain(const FlattenPlan &P, BasicBlock *BB) {
  return P.BlockIndex.find(BB) != P.BlockIndex.end();
}

static void unionBlocks(FlattenPlan &P, BasicBlock *A, BasicBlock *B) {
  auto AI = P.BlockIndex.find(A);
  auto BI = P.BlockIndex.find(B);
  if (AI == P.BlockIndex.end() || BI == P.BlockIndex.end())
    return;
  unsigned AR = findRoot(P, AI->second);
  unsigned BR = findRoot(P, BI->second);
  if (AR != BR)
    P.Parent[BR] = AR;
}

static bool sameRegion(const FlattenPlan &P, BasicBlock *A, BasicBlock *B) {
  auto AR = P.RegionOf.find(A);
  auto BR = P.RegionOf.find(B);
  return AR != P.RegionOf.end() && BR != P.RegionOf.end() &&
         AR->second == BR->second;
}

static bool isAtomicEdge(const FlattenPlan &P, BasicBlock *A, BasicBlock *B) {
  return P.AtomicEdges.count({A, B}) != 0;
}

static void addAtomicEdge(FlattenPlan &P, BasicBlock *A, BasicBlock *B,
                          StringRef Reason) {
  if (!A || !B || !inDomain(P, A) || !inDomain(P, B))
    return;
  P.AtomicEdges.insert({A, B});
  unionBlocks(P, A, B);
  YANSO_WARN_EDGE("fla", P.F, *A, *B, Reason);
}

static bool hasFuncletPad(BasicBlock *BB) {
  if (!BB)
    return false;
  for (Instruction &I : *BB) {
    if (isa<FuncletPadInst>(&I))
      return true;
    if (!isa<PHINode>(&I))
      break;
  }
  return false;
}

static bool hasFuncletOperandBundle(BasicBlock *BB) {
  for (Instruction &I : *BB) {
    auto *CB = dyn_cast<CallBase>(&I);
    if (CB && CB->getOperandBundle(LLVMContext::OB_funclet).has_value())
      return true;
  }
  return false;
}

static bool canUseAsDirectEntry(const FlattenPlan &P, BasicBlock *BB) {
  return P.BlockedEntries.count(BB) == 0;
}

static bool canCreateSyntheticEntryBefore(BasicBlock *BB) {
  return BB && !BB->isEHPad();
}

static BasicBlock *createSyntheticEntry(FlattenPlan &P, BasicBlock *Target,
                                        StringRef Name) {
  BasicBlock *Entry = BasicBlock::Create(P.F.getContext(), Name, &P.F, Target);
  BranchInst::Create(Target, Entry);
  P.BlockIndex[Entry] = P.Blocks.size();
  P.Blocks.push_back(Entry);
  P.Parent.push_back(P.Parent.size());
  return Entry;
}

static bool addEntry(FlattenPlan &P, BasicBlock *BB) {
  if (!BB || !inDomain(P, BB))
    return false;
  if (!canUseAsDirectEntry(P, BB))
    return false;
  if (P.EntryOfBlock.find(BB) != P.EntryOfBlock.end())
    return true;

  auto RI = P.RegionOf.find(BB);
  if (RI == P.RegionOf.end())
    return false;
  Region &R = P.Regions[RI->second];
  EntryKey Key{RI->second, static_cast<unsigned>(R.Entries.size())};
  R.Entries.push_back({BB, 0, 0});
  P.EntryOfBlock[BB] = Key;
  return true;
}

static Region::Entry &entryFor(FlattenPlan &P, EntryKey Key) {
  return P.Regions[Key.Region].Entries[Key.Index];
}

static const Region::Entry &entryFor(const FlattenPlan &P, EntryKey Key) {
  return P.Regions[Key.Region].Entries[Key.Index];
}

static bool shouldFlatten(Function &F) {
  if (F.isVarArg()) {
    YANSO_WARN_SKIP_FUNCTION("fla", F, "vararg function");
    return false;
  }

  if (F.hasFnAttribute(Attribute::Cold)) {
    YANSO_WARN_SKIP_FUNCTION("fla", F, "cold function");
    return false;
  }

  if (F.size() <= 2)
    return false;

  Instruction *EntryTerm = F.getEntryBlock().getTerminator();
  if (EntryTerm->getNumSuccessors() == 0) {
    YANSO_WARN_SKIP_FUNCTION("fla", F, "entry terminator has no successor");
    return false;
  }

  // This must run before pre-demotion/cleanup.  If we later abort after
  // mutation, flattenImpl returns Changed and run() preserves no analyses.
  for (BasicBlock &BB : F) {
    if (isa<IndirectBrInst>(BB.getTerminator())) {
      YANSO_ERROR_SKIP_FUNCTION("fla", F, "contains indirectbr");
      return false;
    }
  }

  // Very large optimized loop nests can make the current flattening state
  // machine explode enough to look hung under lit/test-suite. Keep them as
  // explicit, diagnosed coverage skips until region construction is cheaper.
  if (F.size() > 32) {
    YANSO_WARN_SKIP_FUNCTION("fla", F, "function has too many basic blocks");
    return false;
  }

  return true;
}

static void preDemote(Function &F) {
  std::set<Instruction *> PreDemoteSkipRegs;
  std::set<BasicBlock *> PreDemoteSkipPhiBlocks;
  for (BasicBlock &BB : F) {
    if (BB.isEHPad())
      PreDemoteSkipPhiBlocks.insert(&BB);
    for (Instruction &I : BB)
      if (!I.getType()->isSized() || isa<LandingPadInst>(&I) ||
          isa<FuncletPadInst>(&I))
        PreDemoteSkipRegs.insert(&I);
  }
  yansollvm_fix_stack(&F, &PreDemoteSkipPhiBlocks, &PreDemoteSkipRegs);
}

static void removeTrivialBlocks(Function &F) {
  std::vector<BasicBlock *> TrivialBlocks;
  BasicBlock *OriginalEntry = &F.getEntryBlock();
  for (BasicBlock &BB : F) {
    if (&BB == OriginalEntry)
      continue;
    if (BB.size() == 1 && isa<BranchInst>(BB.getTerminator()) &&
        BB.getTerminator()->getNumSuccessors() == 1 && !BB.hasAddressTaken())
      TrivialBlocks.push_back(&BB);
  }

  for (BasicBlock *BB : TrivialBlocks) {
    if (BB->isEHPad())
      continue;
    bool HasEHPred = false;
    for (BasicBlock *Pred : predecessors(BB)) {
      Instruction *PredTerm = Pred->getTerminator();
      if (isa<InvokeInst>(PredTerm) || isa<CatchSwitchInst>(PredTerm) ||
          isa<CatchReturnInst>(PredTerm) || isa<CleanupReturnInst>(PredTerm)) {
        HasEHPred = true;
        break;
      }
    }
    if (HasEHPred)
      continue;
    if (BB->hasNPredecessors(1))
      BB->replaceSuccessorsPhiUsesWith(BB->getSinglePredecessor());
    std::vector<BasicBlock *> TrivialPreds;
    for (BasicBlock *Pred : predecessors(BB))
      TrivialPreds.push_back(Pred);
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
}

static bool collectBlocksAndEntry(FlattenPlan &P) {
  P.EntryBB = &P.F.getEntryBlock();
  P.EntryTarget = P.EntryBB->getTerminator()->getSuccessor(0);

  for (BasicBlock &BB : P.F) {
    if (&BB != P.EntryBB) {
      P.BlockIndex[&BB] = P.Blocks.size();
      P.Blocks.push_back(&BB);
    }
  }
  P.Parent.resize(P.Blocks.size());
  std::iota(P.Parent.begin(), P.Parent.end(), 0);

  // Split a multi-successor entry so the dispatcher can own the entry edge.
  Instruction *EntryTerminator = P.EntryBB->getTerminator();
  if (EntryTerminator->getNumSuccessors() > 1) {
    BasicBlock *EntryRegion =
        P.EntryBB->splitBasicBlock(EntryTerminator, "entry.region");
    P.BlockIndex[EntryRegion] = P.Blocks.size();
    P.Blocks.push_back(EntryRegion);
    P.Parent.push_back(P.Parent.size());
    P.EntryTarget = EntryRegion;
  }
  return P.EntryTarget != nullptr;
}

static bool collectConstraints(FlattenPlan &P) {
  DominatorTree DT(P.F);
  for (BasicBlock *BB : P.Blocks) {
    Instruction *Term = BB->getTerminator();
    if (isa<IndirectBrInst>(Term)) {
      YANSO_ERROR_SKIP_FUNCTION("fla", P.F, "contains indirectbr");
      return false;
    }

    if (BB->hasAddressTaken()) {
      P.BlockedEntries.insert(BB);
      YANSO_WARN_BLOCK("fla", P.F, *BB, "address-taken block");
    }
    if (BB->isEHPad()) {
      P.BlockedEntries.insert(BB);
      YANSO_WARN_BLOCK("fla", P.F, *BB, "EH pad flatten entry disabled");
    }

    for (Instruction &I : *BB) {
      if (PHINode *PNode = dyn_cast<PHINode>(&I)) {
        for (unsigned I = 0, E = PNode->getNumIncomingValues(); I < E; ++I) {
          if (InvokeInst *II =
                  dyn_cast<InvokeInst>(PNode->getIncomingValue(I))) {
            if (II->getParent() == PNode->getIncomingBlock(I)) {
              P.BlockedEntries.insert(BB);
              YANSO_ERROR_BLOCK("fla", P.F, *BB,
                                "PHI incoming value is invoke result");
            }
          }
        }
      } else if (I.isUsedOutsideOfBlock(BB) && !I.getType()->isSized()) {
        for (Use &U : I.uses()) {
          if (Instruction *UserI = dyn_cast<Instruction>(U.getUser())) {
            BasicBlock *UseBB = nullptr;
            if (PHINode *PN = dyn_cast<PHINode>(UserI))
              UseBB = PN->getIncomingBlock(U);
            else
              UseBB = UserI->getParent();
            if (UseBB != BB) {
              if (UseBB)
                P.BlockedEntries.insert(UseBB);
              YANSO_ERROR_BLOCK("fla", P.F, *BB,
                                "unsized value used across block boundary");
              for (BasicBlock *MidBB : P.Blocks) {
                if (UseBB && MidBB != BB && MidBB != UseBB &&
                    DT.dominates(MidBB, UseBB) && DT.dominates(BB, MidBB)) {
                  P.BlockedEntries.insert(MidBB);
                  YANSO_ERROR_BLOCK("fla", P.F, *MidBB,
                                    "dominates local-only block using unsized "
                                    "cross-block value");
                }
              }
            }
          }
        }
      }
    }

    if (auto *II = dyn_cast<InvokeInst>(Term)) {
      if (!II->getType()->isVoidTy())
        addAtomicEdge(P, BB, II->getNormalDest(), "invoke result normal edge");
      addAtomicEdge(P, BB, II->getUnwindDest(), "EH invoke unwind edge");
    } else if (auto *CRI = dyn_cast<CleanupReturnInst>(Term)) {
      addAtomicEdge(P, BB, CRI->getUnwindDest(), "EH cleanup unwind edge");
    } else if (auto *CSI = dyn_cast<CatchSwitchInst>(Term)) {
      for (BasicBlock *Handler : CSI->handlers())
        addAtomicEdge(P, BB, Handler, "EH catchswitch handler edge");
      if (BasicBlock *UnwindDest = CSI->getUnwindDest())
        addAtomicEdge(P, BB, UnwindDest, "EH catchswitch unwind edge");
    } else if (auto *CRI = dyn_cast<CatchReturnInst>(Term)) {
      addAtomicEdge(P, BB, CRI->getSuccessor(), "EH catchret edge");
    }

    if (BB->isEHPad() || hasFuncletPad(BB) || hasFuncletOperandBundle(BB)) {
      for (BasicBlock *Succ : successors(BB)) {
        if (!isAtomicEdge(P, BB, Succ))
          addAtomicEdge(P, BB, Succ, "EH funclet-internal edge");
      }
    }
  }

  if (P.EntryTarget && !canUseAsDirectEntry(P, P.EntryTarget)) {
    if (!canCreateSyntheticEntryBefore(P.EntryTarget)) {
      YANSO_ERROR_BLOCK("fla", P.F, *P.EntryTarget,
                        "initial target cannot be a flatten entry");
      return false;
    }
    P.EntryTarget = createSyntheticEntry(P, P.EntryTarget, "entry.wrap");
  }

  // A direct edge into a block that cannot be a region entry must stay inside a
  // region.  This keeps the final model simple: only listed entries are entry
  // ports; everything else is region-internal.
  for (BasicBlock *BB : P.Blocks) {
    for (BasicBlock *Succ : successors(BB)) {
      if (inDomain(P, Succ) && P.BlockedEntries.count(Succ))
        unionBlocks(P, BB, Succ);
    }
  }

  return true;
}

static bool materializeRegions(FlattenPlan &P) {
  DenseMap<unsigned, unsigned> RootToRegion;
  for (BasicBlock *BB : P.Blocks) {
    unsigned Root = findRoot(P, P.BlockIndex[BB]);
    auto It = RootToRegion.find(Root);
    unsigned R = 0;
    if (It == RootToRegion.end()) {
      R = P.Regions.size();
      RootToRegion[Root] = R;
      P.Regions.emplace_back();
    } else {
      R = It->second;
    }
    P.RegionOf[BB] = R;
    P.Regions[R].Blocks.push_back(BB);
  }

  if (!addEntry(P, P.EntryTarget)) {
    YANSO_ERROR_BLOCK("fla", P.F, *P.EntryTarget,
                      "failed to add initial flatten entry");
    return false;
  }

  for (BasicBlock *BB : P.Blocks) {
    for (BasicBlock *Succ : successors(BB)) {
      if (!inDomain(P, Succ)) {
        YANSO_ERROR_EDGE("fla", P.F, *BB, *Succ,
                         "successor is outside flatten block set");
        return false;
      }
      if (sameRegion(P, BB, Succ))
        continue;
      if (isAtomicEdge(P, BB, Succ)) {
        YANSO_ERROR_EDGE("fla", P.F, *BB, *Succ,
                         "atomic edge crosses regions after region build");
        return false;
      }

      Region &R = P.Regions[P.RegionOf[BB]];
      bool HasExit = false;
      for (const Region::Exit &Exit : R.Exits) {
        if (Exit.BB == BB) {
          HasExit = true;
          break;
        }
      }
      if (!HasExit)
        R.Exits.push_back({BB, std::nullopt});

      if (!addEntry(P, Succ)) {
        YANSO_ERROR_EDGE("fla", P.F, *BB, *Succ,
                         "cross-region successor cannot be a flatten entry");
        return false;
      }
    }
  }
  return true;
}

static bool buildRegions(FlattenPlan &P) {
  return collectConstraints(P) && materializeRegions(P);
}

static void
addPossibleSource(DenseMap<BasicBlock *, std::vector<EntryKey>> &Possible,
                  std::vector<std::pair<BasicBlock *, EntryKey>> &Worklist,
                  BasicBlock *BB, EntryKey Source) {
  std::vector<EntryKey> &Sources = Possible[BB];
  if (std::find(Sources.begin(), Sources.end(), Source) == Sources.end()) {
    Sources.push_back(Source);
    Worklist.push_back({BB, Source});
  }
}

static void analyzeStateSources(FlattenPlan &P) {
  DenseMap<BasicBlock *, std::vector<EntryKey>> Possible;
  std::vector<std::pair<BasicBlock *, EntryKey>> Worklist;

  for (unsigned R = 0; R < P.Regions.size(); ++R) {
    for (unsigned I = 0; I < P.Regions[R].Entries.size(); ++I)
      addPossibleSource(Possible, Worklist, P.Regions[R].Entries[I].BB,
                        EntryKey{R, I});
  }

  while (!Worklist.empty()) {
    auto [BB, Source] = Worklist.back();
    Worklist.pop_back();
    for (BasicBlock *Succ : successors(BB)) {
      if (inDomain(P, Succ) && sameRegion(P, BB, Succ))
        addPossibleSource(Possible, Worklist, Succ, Source);
    }
  }

  for (Region &R : P.Regions) {
    for (Region::Exit &Exit : R.Exits) {
      auto It = Possible.find(Exit.BB);
      if (It != Possible.end() && It->second.size() == 1)
        Exit.KnownSource = It->second.front();
    }
  }
}

static bool validateFlattenPlan(const FlattenPlan &P) {
  if (!P.EntryTarget || !inDomain(P, P.EntryTarget)) {
    YANSO_ERROR_SKIP_FUNCTION("fla", P.F,
                              "initial target is outside flatten domain");
    return false;
  }
  return true;
}

static void allocateStates(FlattenPlan &P, YansoRNG &RNG) {
  std::unordered_set<uint64_t> UsedIndex;
  std::unordered_set<uint64_t> UsedHash;
  for (Region &R : P.Regions) {
    for (Region::Entry &Entry : R.Entries) {
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
        Entry.StateId = Index;
        Entry.DispatchHash = RoundHashes.back();
        Accepted = true;
      }
      if (!Accepted)
        report_fatal_error(
            "failed to allocate unique flattening dispatch hash");
    }
  }
}

static std::vector<EntryKey> allEntries(const FlattenPlan &P) {
  std::vector<EntryKey> Entries;
  for (unsigned R = 0; R < P.Regions.size(); ++R)
    for (unsigned I = 0; I < P.Regions[R].Entries.size(); ++I)
      Entries.push_back({R, I});
  return Entries;
}

static Dispatcher createDispatcher(FlattenPlan &P, YansoRNG &RNG) {
  BasicBlock &EntryBB = *P.EntryBB;
  Dispatcher D;
  D.BB = BasicBlock::Create(*CONTEXT, "DispatcherBB", &P.F, &EntryBB);
  EntryBB.moveBefore(D.BB);
  EntryBB.getTerminator()->eraseFromParent();
  BranchInst *EntryToDispatcher = BranchInst::Create(D.BB, &EntryBB);

  EntryKey InitialEntry = P.EntryOfBlock[P.EntryTarget];
  D.HashStatePtr =
      new AllocaInst(TYPE_I64, 0, "hashState.ptr", it(EntryToDispatcher));
  new StoreInst(CONST_I64(YansoMixBasis), D.HashStatePtr, false, Align(8),
                it(EntryToDispatcher));
  D.StatePtr = new AllocaInst(TYPE_I64, 0, "state.ptr", it(EntryToDispatcher));
  new StoreInst(CONST_I64(entryFor(P, InitialEntry).StateId), D.StatePtr, false,
                Align(8), it(EntryToDispatcher));

  LoadInst *StateLoad =
      new LoadInst(TYPE_I64, D.StatePtr, "state", false, Align(8), D.BB);
  LoadInst *HashStateLoad = new LoadInst(TYPE_I64, D.HashStatePtr, "hashState",
                                         false, Align(8), D.BB);
  Value *DispatchHash =
      yanso_create_mix64_ir(StateLoad, HashStateLoad, D.BB, *P.F.getParent());
  new StoreInst(DispatchHash, D.HashStatePtr, false, Align(8), D.BB);
  D.HashMissBB = BasicBlock::Create(*CONTEXT, "hashMissBB", &P.F, D.BB);
  BranchInst::Create(D.BB, D.HashMissBB);
  D.Switch = SwitchInst::Create(DispatchHash, D.HashMissBB, 0, D.BB);

  std::vector<BasicBlock *> BlockOrder = P.Blocks;
  RNG.shuffle(BlockOrder);
  for (BasicBlock *BB : BlockOrder)
    BB->moveBefore(D.BB);

  std::vector<EntryKey> Entries = allEntries(P);
  RNG.shuffle(Entries);
  for (EntryKey Key : Entries) {
    const Region::Entry &Entry = entryFor(P, Key);
    D.Switch->addCase(CONST_I64(Entry.DispatchHash), Entry.BB);
  }
  return D;
}

static std::optional<EntryKey> targetEntry(FlattenPlan &P, BasicBlock *From,
                                           BasicBlock *Succ) {
  if (!inDomain(P, Succ)) {
    YANSO_ERROR_EDGE("fla", P.F, *From, *Succ,
                     "successor is outside flatten block set");
    return std::nullopt;
  }
  if (sameRegion(P, From, Succ))
    return std::nullopt;
  auto It = P.EntryOfBlock.find(Succ);
  if (It == P.EntryOfBlock.end()) {
    YANSO_ERROR_EDGE("fla", P.F, *From, *Succ,
                     "cross-region successor has no flatten entry");
    return std::nullopt;
  }
  return It->second;
}

static Value *createEncodedState(FlattenPlan &P, Dispatcher &D,
                                 const std::vector<EntryKey> &EntryOrder,
                                 YansoRNG &RNG, EntryKey FromEntry,
                                 EntryKey FalseEntry, EntryKey TrueEntry,
                                 Value *Cond, Instruction *InsertBefore) {
  uint64_t RandomXor = RNG.next64();
  LoadInst *CurState = new LoadInst(TYPE_I64, D.StatePtr, "state.cur", false,
                                    Align(8), it(InsertBefore));
  Value *TempVal = BinaryOperator::CreateXor(CONST_I64(RandomXor), CurState, "",
                                             it(InsertBefore));
  std::vector<EntryKey> ShuffledEntries = EntryOrder;
  RNG.shuffle(ShuffledEntries);
  int GarbageInterval = ShuffledEntries.size() / 2;
  GarbageInterval = GarbageInterval > 1 ? GarbageInterval : 1;
  for (EntryKey Dst : ShuffledEntries) {
    if (Dst == FalseEntry) {
      TempVal = BinaryOperator::CreateXor(
          CONST_I64(entryFor(P, FromEntry).StateId ^
                    entryFor(P, FalseEntry).StateId ^ RandomXor),
          TempVal, "", it(InsertBefore));
    } else if (Dst == TrueEntry) {
      Value *MaskVal = BinaryOperator::CreateAnd(
          new SExtInst(Cond, TYPE_I64, "", it(InsertBefore)),
          CONST_I64(entryFor(P, TrueEntry).StateId ^
                    entryFor(P, FalseEntry).StateId),
          "", it(InsertBefore));
      TempVal =
          BinaryOperator::CreateXor(MaskVal, TempVal, "", it(InsertBefore));
    } else if (RNG.range(GarbageInterval) == 0) {
      Value *MaskVal = BinaryOperator::CreateAnd(
          CONST_I64(0), CONST_I64(RNG.next64()), "", it(InsertBefore));
      TempVal =
          BinaryOperator::CreateXor(MaskVal, TempVal, "", it(InsertBefore));
    }
  }
  return TempVal;
}

static void storeAbsoluteState(FlattenPlan &P, Dispatcher &D,
                               EntryKey SuccEntry, Instruction *InsertBefore) {
  new StoreInst(CONST_I64(entryFor(P, SuccEntry).StateId), D.StatePtr, false,
                Align(8), it(InsertBefore));
  new StoreInst(CONST_I64(YansoMixBasis), D.HashStatePtr, false, Align(8),
                it(InsertBefore));
}

static void storeStateTransition(FlattenPlan &P, Dispatcher &D,
                                 const std::vector<EntryKey> &EntryOrder,
                                 YansoRNG &RNG, const Region::Exit &Exit,
                                 EntryKey SuccEntry,
                                 Instruction *InsertBefore) {
  if (!Exit.KnownSource) {
    storeAbsoluteState(P, D, SuccEntry, InsertBefore);
    return;
  }
  Value *Next = createEncodedState(
      P, D, EntryOrder, RNG, *Exit.KnownSource, SuccEntry, *Exit.KnownSource,
      ConstantInt::getFalse(P.F.getContext()), InsertBefore);
  new StoreInst(Next, D.StatePtr, false, Align(8), it(InsertBefore));
  new StoreInst(CONST_I64(YansoMixBasis), D.HashStatePtr, false, Align(8),
                it(InsertBefore));
}

static BasicBlock *
createTransitionBlock(FlattenPlan &P, Dispatcher &D,
                      const std::vector<EntryKey> &EntryOrder, YansoRNG &RNG,
                      const Region::Exit &Exit, EntryKey SuccEntry,
                      StringRef Name) {
  BasicBlock *TransitionBB = BasicBlock::Create(*CONTEXT, Name, &P.F, D.BB);
  BranchInst *ToDispatch = BranchInst::Create(D.BB, TransitionBB);
  storeStateTransition(P, D, EntryOrder, RNG, Exit, SuccEntry, ToDispatch);
  return TransitionBB;
}

static void rewriteExit(FlattenPlan &P, Dispatcher &D,
                        const std::vector<EntryKey> &EntryOrder, YansoRNG &RNG,
                        const Region::Exit &Exit) {
  BasicBlock *BB = Exit.BB;
  Instruction *Term = BB->getTerminator();
  if (Term->getNumSuccessors() == 0)
    return;

  if (Term->getNumSuccessors() == 1 && isa<BranchInst>(Term)) {
    BasicBlock *Succ = Term->getSuccessor(0);
    auto Target = targetEntry(P, BB, Succ);
    if (!Target)
      return;
    storeStateTransition(P, D, EntryOrder, RNG, Exit, *Target, Term);
    Term->eraseFromParent();
    BranchInst::Create(D.BB, BB);
    return;
  }

  if (Term->getNumSuccessors() == 2 && isa<BranchInst>(Term)) {
    BranchInst *BR = cast<BranchInst>(Term);
    BasicBlock *TrueSucc = BR->getSuccessor(0);
    BasicBlock *FalseSucc = BR->getSuccessor(1);
    auto TrueEntry = targetEntry(P, BB, TrueSucc);
    auto FalseEntry = targetEntry(P, BB, FalseSucc);
    bool TrueFlat = TrueEntry.has_value();
    bool FalseFlat = FalseEntry.has_value();

    if (TrueFlat && FalseFlat && Exit.KnownSource) {
      Value *Next =
          createEncodedState(P, D, EntryOrder, RNG, *Exit.KnownSource,
                             *FalseEntry, *TrueEntry, BR->getCondition(), BR);
      new StoreInst(Next, D.StatePtr, false, Align(8), it(BR));
      new StoreInst(CONST_I64(YansoMixBasis), D.HashStatePtr, false, Align(8),
                    it(BR));
      BR->eraseFromParent();
      BranchInst::Create(D.BB, BB);
    } else if (TrueFlat || FalseFlat) {
      BasicBlock *TrueDest = TrueSucc;
      BasicBlock *FalseDest = FalseSucc;
      if (TrueFlat)
        TrueDest = createTransitionBlock(P, D, EntryOrder, RNG, Exit,
                                         *TrueEntry, "cond.trans");
      if (FalseFlat)
        FalseDest = createTransitionBlock(P, D, EntryOrder, RNG, Exit,
                                          *FalseEntry, "cond.trans");
      Value *Cond = BR->getCondition();
      BR->eraseFromParent();
      BranchInst::Create(TrueDest, FalseDest, Cond, BB);
    }
    return;
  }

  if (InvokeInst *II = dyn_cast<InvokeInst>(Term)) {
    BasicBlock *NormalSucc = II->getNormalDest();
    if (!sameRegion(P, BB, NormalSucc)) {
      auto NormalEntry = targetEntry(P, BB, NormalSucc);
      if (NormalEntry) {
        BasicBlock *NormalDest = createTransitionBlock(
            P, D, EntryOrder, RNG, Exit, *NormalEntry, "invoke.trans");
        II->setNormalDest(NormalDest);
      }
    }
    return;
  }

  if (SwitchInst *SI = dyn_cast<SwitchInst>(Term)) {
    DenseMap<BasicBlock *, BasicBlock *> TransitionForTarget;
    auto getSwitchSuccessor = [&](BasicBlock *Succ) -> BasicBlock * {
      if (sameRegion(P, BB, Succ))
        return Succ;
      auto SuccEntry = targetEntry(P, BB, Succ);
      if (!SuccEntry)
        return Succ;

      auto It = TransitionForTarget.find(Succ);
      if (It != TransitionForTarget.end())
        return It->second;

      // Preserve address-taken labels: blockaddress constants must still land
      // on the original label body, not on the dispatcher. Rewriting their
      // terminators makes an indirect goto jump into the flattened state
      // machine with a stale state value and can loop in hashMissBB.
      if (BB->hasAddressTaken()) {
        YANSO_WARN_EDGE("fla", P.F, *BB, *Succ,
                        "switch source is address-taken; preserving successor");
        return Succ;
      }

      BasicBlock *TransitionBB = createTransitionBlock(
          P, D, EntryOrder, RNG, Exit, *SuccEntry, "switch.trans");
      TransitionForTarget[Succ] = TransitionBB;
      return TransitionBB;
    };

    BasicBlock *NewDefault = getSwitchSuccessor(SI->getDefaultDest());
    SwitchInst *NewSwitch = SwitchInst::Create(SI->getCondition(), NewDefault,
                                               SI->getNumCases(), it(SI));
    for (auto Case : SI->cases())
      NewSwitch->addCase(Case.getCaseValue(),
                         getSwitchSuccessor(Case.getCaseSuccessor()));
    SI->eraseFromParent();
  }
}

static void rewriteRegionExits(FlattenPlan &P, Dispatcher &D, YansoRNG &RNG) {
  std::vector<EntryKey> EntryOrder = allEntries(P);
  for (Region &R : P.Regions) {
    for (const Region::Exit &Exit : R.Exits)
      rewriteExit(P, D, EntryOrder, RNG, Exit);
  }
}

} // namespace

PreservedAnalyses FlatteningPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  Function *Fn = &F;
  if (toObfuscate(flag, Fn, "fla") && shouldFlatten(*Fn)) {
    INIT_CONTEXT(F);
    if (flattenImpl(*Fn))
      ++Flattened;
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

bool FlatteningPass::flattenImpl(Function &F) {
  // Checks above this point are read-only skips.  From here on the pass may
  // mutate IR; flattenImpl() failures are diagnosed and still preserve no
  // analyses.
  preDemote(F);
  removeTrivialBlocks(F);

  FlattenPlan Plan(F);
  collectBlocksAndEntry(Plan);
  if (!validateFlattenPlan(Plan))
    return false;
  if (!buildRegions(Plan))
    return false;

  analyzeStateSources(Plan);

  YansoRNG RNG(yanso_function_seed(F, "fla"));
  allocateStates(Plan, RNG);
  Dispatcher D = createDispatcher(Plan, RNG);
  rewriteRegionExits(Plan, D, RNG);
  return true;
}
