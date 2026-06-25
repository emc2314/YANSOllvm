#include "BB2FuncPass.h"
#include "Utils.h"
#include "YANSOllvmCommon.h"
#include "YANSOllvmSeed.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

#include <algorithm>
#include <optional>
#include <vector>

using namespace llvm;

namespace {
constexpr unsigned MaxExtracts = 16;
constexpr unsigned MaxRegionBlocks = 6;
constexpr unsigned MaxArmBlocks = 2;
constexpr unsigned MinSwitchSuccessors = 3;
constexpr unsigned MaxLinearWindows = 4;
constexpr unsigned MinGlueBlocks = 2;
constexpr unsigned MaxGlueBlocks = 5;
constexpr unsigned MinSplitSemanticInsts = 10;
constexpr unsigned MinSegmentSemanticInsts = 3;
constexpr unsigned MinCandidateSemanticInsts = 3;

enum class CandidateKind {
  ResplitSingleBlock,
  ResplitSubchain,
  BranchArmRegion,
  SwitchSubsetRegion,
  OriginalDenseBlock,
};

struct RegionCandidate {
  SmallVector<BasicBlock *, MaxRegionBlocks> Blocks;
  CandidateKind Kind = CandidateKind::OriginalDenseBlock;
  unsigned Score = 0;
};

struct LinearChain {
  SmallVector<BasicBlock *, 8> Blocks;
};

struct ShapedChain {
  SmallVector<BasicBlock *, 8> Blocks;
};

struct ShapeResult {
  bool Changed = false;
  bool CandidateReady = false;
};

bool isUnsupportedTerminator(const Instruction *T) {
  return isa<InvokeInst>(T) || isa<IndirectBrInst>(T) || isa<CallBrInst>(T) ||
         isa<CatchSwitchInst>(T) || isa<CatchReturnInst>(T) ||
         isa<CleanupReturnInst>(T) || isa<ResumeInst>(T);
}

const char *unsafeReason(BasicBlock &BB) {
  if (BB.hasAddressTaken())
    return "address-taken block";
  if (BB.isEHPad())
    return "EH pad";
  if (yansollvm_has_dynamic_stack_state(BB))
    return "dynamic stack state (VLA/stacksave/stackrestore)";
  if (isUnsupportedTerminator(BB.getTerminator()))
    return "unsupported terminator";
  return nullptr;
}

bool isUnsafeBlock(BasicBlock &BB) { return unsafeReason(BB) != nullptr; }

bool hasOutsidePred(BasicBlock *BB,
                    const SmallPtrSetImpl<BasicBlock *> &Region) {
  for (BasicBlock *Pred : predecessors(BB))
    if (Pred->getParent() == BB->getParent() && !Region.contains(Pred))
      return true;
  return false;
}

bool hasSingleEntry(ArrayRef<BasicBlock *> Blocks) {
  if (Blocks.empty())
    return false;

  SmallPtrSet<BasicBlock *, 8> Region(Blocks.begin(), Blocks.end());
  BasicBlock *Header = Blocks.front();
  for (BasicBlock *BB : Blocks) {
    for (BasicBlock *Pred : predecessors(BB)) {
      if (Region.contains(Pred))
        continue;
      if (BB != Header)
        return false;
    }
  }
  return true;
}

bool coversTooMuch(Function &F, ArrayRef<BasicBlock *> Blocks) {
  if (Blocks.size() == 1 && F.size() <= 3)
    return false;
  return Blocks.size() + 2 >= F.size();
}

bool canAddBlock(BasicBlock *BB, BasicBlock *Header, DominatorTree &DT,
                 const SmallPtrSetImpl<BasicBlock *> &Region) {
  return BB && !Region.contains(BB) && BB->getParent() == Header->getParent() &&
         !isUnsafeBlock(*BB) && DT.dominates(Header, BB) &&
         !hasOutsidePred(BB, Region);
}

void addBlock(BasicBlock *BB, SmallPtrSetImpl<BasicBlock *> &Region,
              SmallVectorImpl<BasicBlock *> &Blocks) {
  if (Region.insert(BB).second)
    Blocks.push_back(BB);
}

bool hasLocalSinglePred(BasicBlock *BB, BasicBlock *Pred) {
  BasicBlock *OnlyPred = BB->getSinglePredecessor();
  return OnlyPred && OnlyPred == Pred;
}

bool isLinearEdge(BasicBlock *A, BasicBlock *B) {
  if (!A || !B || A->getParent() != B->getParent())
    return false;
  if (isUnsafeBlock(*A) || isUnsafeBlock(*B))
    return false;
  auto *BI = dyn_cast<BranchInst>(A->getTerminator());
  if (!BI || !BI->isUnconditional() || BI->getSuccessor(0) != B)
    return false;
  if (!hasLocalSinglePred(B, A))
    return false;
  if (isa<PHINode>(B->begin()))
    return false;
  return true;
}

bool isSemanticInstruction(Instruction &I) {
  if (I.isTerminator() || isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
      isa<AllocaInst>(I) || isa<LandingPadInst>(I))
    return false;
  if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::dbg_declare:
    case Intrinsic::dbg_value:
    case Intrinsic::dbg_assign:
      return false;
    default:
      return true;
    }
  }
  if (isa<CastInst>(I) || isa<GetElementPtrInst>(I))
    return false;
  return true;
}

unsigned semanticInstCount(BasicBlock &BB) {
  unsigned Count = 0;
  for (Instruction &I : BB)
    if (isSemanticInstruction(I))
      ++Count;
  return Count;
}

unsigned semanticInstCount(ArrayRef<BasicBlock *> Blocks) {
  unsigned Count = 0;
  for (BasicBlock *BB : Blocks)
    Count += semanticInstCount(*BB);
  return Count;
}

unsigned candidateScore(ArrayRef<BasicBlock *> Blocks, CandidateKind Kind) {
  unsigned Sem = semanticInstCount(Blocks);
  unsigned BlockBonus = Blocks.size() > 1 ? 4 + Blocks.size() : 1;
  unsigned KindBonus = 0;
  switch (Kind) {
  case CandidateKind::ResplitSubchain:
    KindBonus = 18;
    break;
  case CandidateKind::ResplitSingleBlock:
    KindBonus = 14;
    break;
  case CandidateKind::BranchArmRegion:
    KindBonus = 9;
    break;
  case CandidateKind::SwitchSubsetRegion:
    KindBonus = 8;
    break;
  case CandidateKind::OriginalDenseBlock:
    KindBonus = 3;
    break;
  }
  return Sem * 3 + BlockBonus + KindBonus;
}

bool candidateHasEnoughSemantics(ArrayRef<BasicBlock *> Blocks,
                                 CandidateKind Kind) {
  unsigned Min = 0;
  switch (Kind) {
  case CandidateKind::OriginalDenseBlock:
    Min = 5;
    break;
  case CandidateKind::ResplitSingleBlock:
    Min = MinSegmentSemanticInsts;
    break;
  case CandidateKind::ResplitSubchain:
    Min = MinCandidateSemanticInsts;
    break;
  case CandidateKind::BranchArmRegion:
  case CandidateKind::SwitchSubsetRegion:
    Min = 1;
    break;
  }
  return semanticInstCount(Blocks) >= Min;
}

bool isSafeSplitBefore(Instruction &I) {
  if (isa<PHINode>(I) || I.isTerminator() || isa<LandingPadInst>(I))
    return false;
  if (auto *Call = dyn_cast<CallInst>(I.getPrevNode()))
    if (Call->isMustTailCall())
      return false;
  return true;
}

void collectSplitPoints(BasicBlock &BB,
                        SmallVectorImpl<BasicBlock::iterator> &Points) {
  unsigned SemSinceStart = 0;
  for (auto It = BB.getFirstInsertionPt(), E = BB.end(); It != E; ++It) {
    Instruction &I = *It;
    if (SemSinceStart >= MinSegmentSemanticInsts && isSafeSplitBefore(I)) {
      Points.push_back(It);
      SemSinceStart = 0;
    }
    if (isSemanticInstruction(I))
      ++SemSinceStart;
  }
}

bool splitGluedBlock(BasicBlock *BB, YansoRNG &RNG,
                     SmallVectorImpl<BasicBlock *> &Parts) {
  if (!BB || BB->getParent() == nullptr || isUnsafeBlock(*BB))
    return false;
  if (semanticInstCount(*BB) < MinSplitSemanticInsts)
    return false;

  SmallVector<BasicBlock::iterator, 8> Points;
  collectSplitPoints(*BB, Points);
  if (Points.empty())
    return false;

  unsigned MaxSplits = std::min<unsigned>(Points.size(), MaxRegionBlocks - 1);
  unsigned DesiredSplits = std::min<unsigned>(MaxSplits, 1 + RNG.range(MaxSplits));
  if (DesiredSplits == 0)
    return false;

  SmallVector<unsigned, 8> Indexes;
  for (unsigned I = 0, E = Points.size(); I != E; ++I)
    Indexes.push_back(I);
  RNG.shuffle(Indexes);
  Indexes.resize(DesiredSplits);
  llvm::sort(Indexes);

  BasicBlock *Cur = BB;
  Parts.push_back(Cur);
  for (unsigned Index : Indexes) {
    BasicBlock::iterator Point = Points[Index];
    BasicBlock *NewBB = Cur->splitBasicBlock(Point, Cur->getName() + ".bb2f");
    Parts.push_back(NewBB);
    Cur = NewBB;
  }
  return Parts.size() >= 2;
}

void collectLinearChains(Function &F, SmallVectorImpl<LinearChain> &Chains) {
  SmallPtrSet<BasicBlock *, 32> Seen;
  for (BasicBlock &BB : F) {
    if (Seen.contains(&BB) || isUnsafeBlock(BB))
      continue;
    if (pred_size(&BB) == 1) {
      BasicBlock *Pred = *pred_begin(&BB);
      if (Pred->getParent() == &F && isLinearEdge(Pred, &BB))
        continue;
    }

    LinearChain Chain;
    BasicBlock *Cur = &BB;
    while (Cur && !Seen.contains(Cur) && !isUnsafeBlock(*Cur)) {
      Seen.insert(Cur);
      Chain.Blocks.push_back(Cur);
      auto *BI = dyn_cast<BranchInst>(Cur->getTerminator());
      if (!BI || !BI->isUnconditional())
        break;
      BasicBlock *Next = BI->getSuccessor(0);
      if (!isLinearEdge(Cur, Next))
        break;
      Cur = Next;
    }
    if (Chain.Blocks.size() >= MinGlueBlocks)
      Chains.push_back(std::move(Chain));
  }
}

ShapeResult mergeWindow(ArrayRef<BasicBlock *> Window, BasicBlock *&Merged) {
  ShapeResult Result;
  if (Window.size() < MinGlueBlocks)
    return Result;
  for (unsigned I = 1, E = Window.size(); I != E; ++I)
    if (!isLinearEdge(Window[I - 1], Window[I]))
      return Result;

  Merged = Window.front();
  for (unsigned I = 1, E = Window.size(); I != E; ++I) {
    BasicBlock *Next = Window[I];
    if (!MergeBlockIntoPredecessor(Next))
      return Result;
    Result.Changed = true;
  }
  Result.CandidateReady = true;
  return Result;
}

ShapeResult shapeOneLinearWindow(LinearChain &Chain, YansoRNG &RNG,
                                 ShapedChain &Out) {
  ShapeResult Result;
  if (Chain.Blocks.size() < MinGlueBlocks)
    return Result;

  unsigned MaxLen = std::min<unsigned>(MaxGlueBlocks, Chain.Blocks.size());
  unsigned Len = MinGlueBlocks;
  if (MaxLen > MinGlueBlocks)
    Len += RNG.range(MaxLen - MinGlueBlocks + 1);

  unsigned StartLimit = Chain.Blocks.size() - Len + 1;
  unsigned Start = StartLimit > 1 ? RNG.range(StartLimit) : 0;

  SmallVector<BasicBlock *, MaxGlueBlocks> Window;
  for (unsigned I = 0; I < Len; ++I)
    Window.push_back(Chain.Blocks[Start + I]);

  unsigned TotalSem = semanticInstCount(Window);
  if (TotalSem < MinSplitSemanticInsts)
    return Result;

  BasicBlock *Merged = nullptr;
  Result = mergeWindow(Window, Merged);
  if (!Result.CandidateReady)
    return Result;

  Result.CandidateReady = splitGluedBlock(Merged, RNG, Out.Blocks);
  Result.Changed = true;
  return Result;
}

bool shapeLinearWindows(Function &F, YansoRNG &RNG,
                        SmallVectorImpl<ShapedChain> &Shaped,
                        DenseSet<BasicBlock *> &ShapedBlocks) {
  SmallVector<LinearChain, 16> Chains;
  collectLinearChains(F, Chains);
  RNG.shuffle(Chains);

  unsigned Changed = 0;
  for (LinearChain &Chain : Chains) {
    if (Changed >= MaxLinearWindows)
      break;
    ShapedChain SC;
    ShapeResult Result = shapeOneLinearWindow(Chain, RNG, SC);
    if (Result.Changed)
      ++Changed;
    if (Result.CandidateReady) {
      for (BasicBlock *BB : SC.Blocks)
        ShapedBlocks.insert(BB);
      Shaped.push_back(std::move(SC));
    }
  }
  return Changed != 0;
}

bool shapeDenseBlocks(Function &F, YansoRNG &RNG,
                      SmallVectorImpl<ShapedChain> &Shaped,
                      DenseSet<BasicBlock *> &ShapedBlocks) {
  SmallVector<BasicBlock *, 16> Blocks;
  for (BasicBlock &BB : F) {
    if (ShapedBlocks.contains(&BB))
      continue;
    if (isUnsafeBlock(BB))
      continue;
    if (semanticInstCount(BB) < MinSplitSemanticInsts)
      continue;
    Blocks.push_back(&BB);
  }

  RNG.shuffle(Blocks);
  unsigned Changed = 0;
  for (BasicBlock *BB : Blocks) {
    if (Changed >= MaxLinearWindows)
      break;
    if (BB->getParent() != &F || isUnsafeBlock(*BB))
      continue;
    if (ShapedBlocks.contains(BB))
      continue;
    ShapedChain SC;
    if (splitGluedBlock(BB, RNG, SC.Blocks)) {
      for (BasicBlock *Part : SC.Blocks)
        ShapedBlocks.insert(Part);
      Shaped.push_back(std::move(SC));
      ++Changed;
    }
  }
  return Changed != 0;
}

void collectShapedCandidates(ArrayRef<ShapedChain> Shaped,
                             SmallVectorImpl<RegionCandidate> &Candidates) {
  for (const ShapedChain &SC : Shaped) {
    if (SC.Blocks.empty())
      continue;
    for (BasicBlock *BB : SC.Blocks) {
      RegionCandidate C;
      C.Kind = CandidateKind::ResplitSingleBlock;
      C.Blocks.push_back(BB);
      C.Score = candidateScore(C.Blocks, C.Kind);
      if (candidateHasEnoughSemantics(C.Blocks, C.Kind))
        Candidates.push_back(C);
    }

    if (SC.Blocks.size() < 3)
      continue;
    for (unsigned Start = 0, E = SC.Blocks.size(); Start != E; ++Start) {
      for (unsigned Len = 2; Len <= MaxRegionBlocks && Start + Len <= E; ++Len) {
        if (Start == 0 && Start + Len == E)
          continue;
        RegionCandidate C;
        C.Kind = CandidateKind::ResplitSubchain;
        for (unsigned I = 0; I < Len; ++I)
          C.Blocks.push_back(SC.Blocks[Start + I]);
        C.Score = candidateScore(C.Blocks, C.Kind);
        if (candidateHasEnoughSemantics(C.Blocks, C.Kind) &&
            hasSingleEntry(C.Blocks))
          Candidates.push_back(C);
      }
    }
  }
}

void extendLinearArm(BasicBlock *Start, BasicBlock *Header, DominatorTree &DT,
                     YansoRNG &RNG, SmallPtrSetImpl<BasicBlock *> &Region,
                     SmallVectorImpl<BasicBlock *> &Blocks) {
  unsigned Limit = 1 + RNG.range(MaxArmBlocks);
  BasicBlock *Cur = Start;
  for (unsigned I = 0; I < Limit && Blocks.size() < MaxRegionBlocks; ++I) {
    auto *BI = dyn_cast<BranchInst>(Cur->getTerminator());
    if (!BI || !BI->isUnconditional())
      return;

    BasicBlock *Next = BI->getSuccessor(0);
    if (!canAddBlock(Next, Header, DT, Region))
      return;

    addBlock(Next, Region, Blocks);
    Cur = Next;
  }
}

std::optional<RegionCandidate> buildBranchArmRegion(BasicBlock &Header,
                                                    DominatorTree &DT,
                                                    YansoRNG &RNG) {
  auto *BI = dyn_cast<BranchInst>(Header.getTerminator());
  if (!BI || !BI->isConditional())
    return std::nullopt;
  if (const char *Reason = unsafeReason(Header)) {
    YANSO_WARN_BLOCK("bb2func", *Header.getParent(), Header, Reason);
    return std::nullopt;
  }

  SmallPtrSet<BasicBlock *, 8> Region;
  RegionCandidate Candidate;
  Candidate.Kind = CandidateKind::BranchArmRegion;
  addBlock(&Header, Region, Candidate.Blocks);

  unsigned First = RNG.range(2);
  for (unsigned N = 0; N < 2 && Candidate.Blocks.size() == 1; ++N) {
    BasicBlock *Succ = BI->getSuccessor((First + N) % 2);
    if (Succ && Succ->getParent() == Header.getParent())
      if (const char *Reason = unsafeReason(*Succ)) {
        YANSO_WARN_BLOCK("bb2func", *Header.getParent(), *Succ, Reason);
        continue;
      }
    if (!canAddBlock(Succ, &Header, DT, Region))
      continue;
    addBlock(Succ, Region, Candidate.Blocks);
    extendLinearArm(Succ, &Header, DT, RNG, Region, Candidate.Blocks);
  }

  if (Candidate.Blocks.size() < 2 || !hasSingleEntry(Candidate.Blocks) ||
      !candidateHasEnoughSemantics(Candidate.Blocks, Candidate.Kind))
    return std::nullopt;
  Candidate.Score = candidateScore(Candidate.Blocks, Candidate.Kind);
  return Candidate;
}

void uniqueSuccessors(SwitchInst &SI, SmallVectorImpl<BasicBlock *> &Succs) {
  Succs.push_back(SI.getDefaultDest());
  for (auto &C : SI.cases())
    Succs.push_back(C.getCaseSuccessor());

  llvm::sort(Succs);
  Succs.erase(std::unique(Succs.begin(), Succs.end()), Succs.end());
}

std::optional<RegionCandidate> buildSwitchSubsetRegion(BasicBlock &Header,
                                                       DominatorTree &DT,
                                                       YansoRNG &RNG) {
  auto *SI = dyn_cast<SwitchInst>(Header.getTerminator());
  if (!SI)
    return std::nullopt;
  if (const char *Reason = unsafeReason(Header)) {
    YANSO_WARN_BLOCK("bb2func", *Header.getParent(), Header, Reason);
    return std::nullopt;
  }

  SmallVector<BasicBlock *, 8> Succs;
  uniqueSuccessors(*SI, Succs);
  if (Succs.size() < MinSwitchSuccessors)
    return std::nullopt;

  RNG.shuffle(Succs);
  unsigned Pick = Succs.size() >= 4 ? 2 + RNG.range(Succs.size() - 2)
                                    : 1 + RNG.range(Succs.size() - 1);

  SmallPtrSet<BasicBlock *, 8> Region;
  RegionCandidate Candidate;
  Candidate.Kind = CandidateKind::SwitchSubsetRegion;
  addBlock(&Header, Region, Candidate.Blocks);

  for (unsigned I = 0; I < Pick && Candidate.Blocks.size() < MaxRegionBlocks;
       ++I) {
    BasicBlock *Succ = Succs[I];
    if (Succ && Succ->getParent() == Header.getParent())
      if (const char *Reason = unsafeReason(*Succ)) {
        YANSO_WARN_BLOCK("bb2func", *Header.getParent(), *Succ, Reason);
        continue;
      }
    if (!canAddBlock(Succ, &Header, DT, Region))
      continue;
    addBlock(Succ, Region, Candidate.Blocks);
  }

  if (Candidate.Blocks.size() < 2 || Candidate.Blocks.size() == Succs.size() + 1 ||
      !hasSingleEntry(Candidate.Blocks) ||
      !candidateHasEnoughSemantics(Candidate.Blocks, Candidate.Kind))
    return std::nullopt;
  Candidate.Score = candidateScore(Candidate.Blocks, Candidate.Kind);
  return Candidate;
}

bool overlaps(ArrayRef<BasicBlock *> Blocks, const DenseSet<BasicBlock *> &Used) {
  return llvm::any_of(Blocks,
                      [&](BasicBlock *BB) { return Used.contains(BB); });
}

void markUsed(ArrayRef<BasicBlock *> Blocks, DenseSet<BasicBlock *> &Used) {
  for (BasicBlock *BB : Blocks)
    Used.insert(BB);
}

bool extractRegion(Function &F, ArrayRef<BasicBlock *> Blocks, CandidateKind Kind) {
  if (Blocks.empty())
    return false;
  for (BasicBlock *BB : Blocks)
    if (BB->getParent() != &F)
      return false;
  if (!hasSingleEntry(Blocks))
    return false;
  if (Kind == CandidateKind::ResplitSubchain) {
    for (unsigned I = 1, E = Blocks.size(); I != E; ++I)
      if (!isLinearEdge(Blocks[I - 1], Blocks[I]))
        return false;
  }

  CodeExtractor CE(Blocks);
  if (!CE.isEligible())
    return false;

  CodeExtractorAnalysisCache CEAC(F);
  if (Function *Extracted = CE.extractCodeRegion(CEAC)) {
    Extracted->removeFnAttr(Attribute::AlwaysInline);
    Extracted->addFnAttr(Attribute::NoInline);
    return true;
  }
  return false;
}

void collectRegionCandidates(Function &F, DominatorTree &DT, YansoRNG &RNG,
                             SmallVectorImpl<RegionCandidate> &Candidates) {
  for (BasicBlock &BB : F) {
    if (auto C = buildSwitchSubsetRegion(BB, DT, RNG))
      Candidates.push_back(*C);
    if (auto C = buildBranchArmRegion(BB, DT, RNG))
      Candidates.push_back(*C);
  }
}

void collectSingleBlockCandidates(Function &F,
                                  SmallVectorImpl<RegionCandidate> &Candidates) {
  for (BasicBlock &BB : F) {
    if (semanticInstCount(BB) < 5)
      continue;
    if (const char *Reason = unsafeReason(BB)) {
      YANSO_WARN_BLOCK("bb2func", F, BB, Reason);
      continue;
    }

    RegionCandidate Candidate;
    Candidate.Kind = CandidateKind::OriginalDenseBlock;
    Candidate.Blocks.push_back(&BB);
    Candidate.Score = candidateScore(Candidate.Blocks, Candidate.Kind);
    std::vector<BasicBlock *> Single{&BB};
    CodeExtractor CE(Single);
    if (CE.isEligible())
      Candidates.push_back(Candidate);
  }
}

void prioritizeCandidates(SmallVectorImpl<RegionCandidate> &Candidates,
                          YansoRNG &RNG) {
  RNG.shuffle(Candidates);
  llvm::stable_sort(Candidates, [](const RegionCandidate &A,
                                   const RegionCandidate &B) {
    return A.Score > B.Score;
  });
}
static bool runBB2FuncOnFunction(Function &F) {
  if (F.isDeclaration())
    return false;

  if (F.hasPersonalityFn()) {
    YANSO_WARN_SKIP_FUNCTION("bb2func", F, "EH/personality function");
    return false;
  }

  bool Modified = false;
  YansoRNG RNG(yanso_function_seed(F, "bb2func"));

  SmallVector<ShapedChain, 8> Shaped;
  DenseSet<BasicBlock *> ShapedBlocks;
  if (shapeLinearWindows(F, RNG, Shaped, ShapedBlocks))
    Modified = true;
  if (shapeDenseBlocks(F, RNG, Shaped, ShapedBlocks))
    Modified = true;

  DominatorTree DT(F);
  SmallVector<RegionCandidate, 64> Candidates;
  collectShapedCandidates(Shaped, Candidates);
  collectRegionCandidates(F, DT, RNG, Candidates);
  collectSingleBlockCandidates(F, Candidates);
  prioritizeCandidates(Candidates, RNG);

  DenseSet<BasicBlock *> Used;
  unsigned Extracted = 0;
  for (RegionCandidate &Candidate : Candidates) {
    if (Extracted >= MaxExtracts)
      break;
    if (overlaps(Candidate.Blocks, Used))
      continue;
    if (coversTooMuch(F, Candidate.Blocks))
      continue;

    if (extractRegion(F, Candidate.Blocks, Candidate.Kind)) {
      markUsed(Candidate.Blocks, Used);
      Modified = true;
      ++Extracted;
    }
  }

  return Modified;
}
} // namespace

PreservedAnalyses BB2FuncPass::run(Module &M, ModuleAnalysisManager &) {
  if (!Enabled)
    return PreservedAnalyses::all();

  SmallVector<Function *, 32> Worklist;
  for (Function &F : M)
    if (!F.isDeclaration())
      Worklist.push_back(&F);

  bool Modified = false;
  for (Function *F : Worklist) {
    if (!F->getParent())
      continue;
    Modified |= runBB2FuncOnFunction(*F);
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
