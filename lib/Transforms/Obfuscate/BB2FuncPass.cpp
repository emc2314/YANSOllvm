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

struct RegionCandidate {
  SmallVector<BasicBlock *, MaxRegionBlocks> Blocks;
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

bool isUnsafeBlock(BasicBlock &BB) {
  return unsafeReason(BB) != nullptr;
}

bool hasOutsidePred(BasicBlock *BB,
                    const SmallPtrSetImpl<BasicBlock *> &Region) {
  for (BasicBlock *Pred : predecessors(BB))
    if (!Region.contains(Pred))
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

  if (Candidate.Blocks.size() < 2 || !hasSingleEntry(Candidate.Blocks))
    return std::nullopt;
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
      !hasSingleEntry(Candidate.Blocks))
    return std::nullopt;
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

bool extractRegion(Function &F, ArrayRef<BasicBlock *> Blocks) {
  if (Blocks.empty())
    return false;
  for (BasicBlock *BB : Blocks)
    if (BB->getParent() != &F)
      return false;
  if (!hasSingleEntry(Blocks))
    return false;

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
                                  SmallVectorImpl<BasicBlock *> &Blocks) {
  for (BasicBlock &BB : F) {
    if (BB.size() <= 4)
      continue;
    if (const char *Reason = unsafeReason(BB)) {
      YANSO_WARN_BLOCK("bb2func", F, BB, Reason);
      continue;
    }

    std::vector<BasicBlock *> Single{&BB};
    CodeExtractor CE(Single);
    if (CE.isEligible())
      Blocks.push_back(&BB);
  }
}
} // namespace

PreservedAnalyses BB2FuncPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (!Enabled || F.getEntryBlock().getName() == "newFuncRoot")
    return PreservedAnalyses::all();

  if (F.hasPersonalityFn()) {
    YANSO_WARN_FUNCTION("bb2func", F, "EH/personality function");
    return PreservedAnalyses::all();
  }

  bool Modified = false;
  YansoRNG RNG(yanso_function_seed(F, "bb2func"));
  DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);

  SmallVector<RegionCandidate, 16> Candidates;
  collectRegionCandidates(F, DT, RNG, Candidates);
  RNG.shuffle(Candidates);

  DenseSet<BasicBlock *> Used;
  unsigned Extracted = 0;
  for (RegionCandidate &Candidate : Candidates) {
    if (Extracted >= MaxExtracts)
      break;
    if (overlaps(Candidate.Blocks, Used))
      continue;
    if (coversTooMuch(F, Candidate.Blocks))
      continue;

    if (extractRegion(F, Candidate.Blocks)) {
      markUsed(Candidate.Blocks, Used);
      Modified = true;
      ++Extracted;
    }
  }

  if (Extracted == 0) {
    SmallVector<BasicBlock *, 32> BBList;
    collectSingleBlockCandidates(F, BBList);
    RNG.shuffle(BBList);

    if (BBList.size() > MaxExtracts)
      BBList.resize(MaxExtracts);

    for (BasicBlock *BB : BBList) {
      if (Used.contains(BB) || BB->getParent() != &F)
        continue;
      std::vector<BasicBlock *> Blocks{BB};
      if (extractRegion(F, Blocks)) {
        Modified = true;
        ++Extracted;
      }
    }
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
