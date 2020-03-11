#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

#include "Util.h"

#include <algorithm>
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
static RegisterPass<BB2Func> X("bb2func", "Extract basic blocks to Function");
Pass *createBB2FuncPass() { return new BB2Func(); }

bool BB2Func::runOnFunction(Function &F) {
  bool modified = false;
  if(F.getEntryBlock().getName() == "newFuncRoot")
    return modified;
  std::vector<BasicBlock *> bblist;
  for(Function::iterator i = F.begin(); i != F.end(); ++i){
    BasicBlock *BB = &*i;
    if(BB->getInstList().size() > 2){
      std::vector<BasicBlock *> blocks;
      blocks.push_back(BB);
      CodeExtractor CE(blocks);
      if(CE.isEligible()){
        bblist.push_back(BB);
      }
    }
  }
  
  size_t sizeLimit = 32;
  if(bblist.size() > sizeLimit){
    std::sort(bblist.begin(), bblist.end(), [](const BasicBlock *a, const BasicBlock *b){
              return a->getInstList().size() < b->getInstList().size();});
    bblist.erase(bblist.begin()+sizeLimit, bblist.end());
  }

  for(BasicBlock *BB: bblist){
    std::vector<BasicBlock *> blocks;
    blocks.push_back(BB);
    CodeExtractor CE(blocks);
    Function *F = CE.extractCodeRegion();
    F->addFnAttr(Attribute::NoInline);
    F->addFnAttr(Attribute::OptimizeNone);
    modified = true;
  }
  return modified;
}