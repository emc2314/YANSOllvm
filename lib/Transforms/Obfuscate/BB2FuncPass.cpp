#include "BB2FuncPass.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

#include <algorithm>
#include <list>
#include <vector>

using namespace llvm;

static bool hasDynamicStackState(BasicBlock &BB) {
  for (Instruction &I : BB) {
    if (auto *AI = dyn_cast<AllocaInst>(&I)) {
      if (AI->isArrayAllocation())
        return true;
    } else if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
      if (II->getIntrinsicID() == Intrinsic::stacksave ||
          II->getIntrinsicID() == Intrinsic::stackrestore)
        return true;
    }
  }
  return false;
}

PreservedAnalyses BB2FuncPass::run(Function &F, FunctionAnalysisManager &) {
  if (!Enabled || F.getEntryBlock().getName() == "newFuncRoot")
    return PreservedAnalyses::all();

  bool Modified = false;
  std::list<BasicBlock *> BBList;
  for (BasicBlock &BB : F) {
    if (BB.size() > 4 && !hasDynamicStackState(BB)) {
      std::vector<BasicBlock *> Blocks{&BB};
      CodeExtractor CE(Blocks);
      if (CE.isEligible())
        BBList.push_back(&BB);
    }
  }

  size_t SizeLimit = 16;
  if (BBList.size() > SizeLimit) {
    BBList.sort([](const BasicBlock *A, const BasicBlock *B) {
      return A->size() > B->size();
    });
    auto It = BBList.begin();
    std::advance(It, SizeLimit);
    BBList.erase(It, BBList.end());
  }

  for (auto It = BBList.begin(); It != BBList.end(); ++It) {
    BasicBlock *BB = *It;
    BasicBlock::iterator ItB = BB->getFirstInsertionPt();
    size_t BBSize = std::distance(ItB, BB->end());
    if (BBSize >= 8) {
      std::advance(ItB, BBSize / 2 > 8 ? 8 : BBSize / 2);
      BBList.push_back(BB->splitBasicBlock(ItB));
    }
  }

  for (BasicBlock *BB : BBList) {
    std::vector<BasicBlock *> Blocks{BB};
    CodeExtractor CE(Blocks);
    if (!CE.isEligible())
      continue;
    CodeExtractorAnalysisCache CEAC(F);
    if (Function *Extracted = CE.extractCodeRegion(CEAC)) {
      Extracted->removeFnAttr(Attribute::AlwaysInline);
      Extracted->addFnAttr(Attribute::NoInline);
      Modified = true;
    }
  }
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
