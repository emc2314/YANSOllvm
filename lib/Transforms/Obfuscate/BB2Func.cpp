#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

#include "Util.h"

#include <algorithm>
#include <list>
#include <vector>

using namespace llvm;

// Stats

namespace {
struct BB2Func : public FunctionPass {
  static char ID;

  BB2Func() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override;
};
} // namespace

char BB2Func::ID = 0;
static RegisterPass<BB2Func> X("bb2func",
                               "Split & extract basic blocks to functions");
Pass *createBB2FuncPass() { return new BB2Func(); }

bool BB2Func::runOnFunction(Function &F) {
  bool modified = false;
  if (F.getEntryBlock().getName() == "newFuncRoot")
    return modified;
  std::list<BasicBlock *> bblist;
  for (Function::iterator i = F.begin(); i != F.end(); ++i) {
    BasicBlock *BB = &*i;
    if (BB->getInstList().size() > 4) {
      std::vector<BasicBlock *> blocks;
      blocks.push_back(BB);
      CodeExtractor CE(blocks);
      if (CE.isEligible()) {
        bblist.push_back(BB);
      }
    }
  }

  size_t sizeLimit = 16;
  if (bblist.size() > sizeLimit) {
    bblist.sort([](const BasicBlock *a, const BasicBlock *b) {
      return a->getInstList().size() > b->getInstList().size();
    });
    auto it = bblist.begin();
    std::advance(it, sizeLimit);
    bblist.erase(it, bblist.end());
  }

  for (auto it = bblist.begin(); it != bblist.end(); it++) {
    BasicBlock *BB = *it;
    BasicBlock::iterator itb = BB->getFirstInsertionPt();
    size_t bbSize = std::distance(itb, BB->end());
    if (bbSize >= 8) {
      std::advance(itb, bbSize / 2 > 8 ? 8 : bbSize / 2);
      bblist.push_back(BB->splitBasicBlock(itb));
    }
  }

  for (BasicBlock *BB : bblist) {
    std::vector<BasicBlock *> blocks;
    blocks.push_back(BB);
    CodeExtractor CE(blocks);
    assert(CE.isEligible());
    Function *F = CE.extractCodeRegion();
    F->addFnAttr(Attribute::NoInline);
    modified = true;
  }
  return modified;
}