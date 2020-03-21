#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"

#include "Util.h"

#include <algorithm>
#include <random>
#include <vector>

using namespace llvm;

// Stats

namespace {
struct Connect : public FunctionPass {
  static char ID;

  Connect() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override;
};
} // namespace

char Connect::ID = 0;
static RegisterPass<Connect> X("connect", "Split & connect basic blocks & add garbage blocks");
Pass *createConnectPass() { return new Connect(); }

bool Connect::runOnFunction(Function &F) {
  Function *f = &F;
  std::vector<BasicBlock *> origBB, downBB, allBB;
  std::random_device rd;
  std::mt19937 g(rd());

  Function::iterator i = f->begin();
  for (++i; i != f->end(); ++i) {
    BasicBlock *tmp = &*i;
    origBB.push_back(tmp);
  }

  for (std::vector<BasicBlock *>::iterator b = origBB.begin();
       b != origBB.end();) {
    BasicBlock *i = *b;
    BasicBlock::iterator it = i->getFirstInsertionPt();
    size_t bbSize = std::distance(it, i->end());
    if(bbSize < 4 ){
      b=origBB.erase(b);
      continue;
    }
    std::advance(it, bbSize / 2);
    BasicBlock *newBB = i->splitBasicBlock(it);
    downBB.push_back(newBB);
    allBB.push_back(i);
    allBB.push_back(newBB);
    ++b;
  }

  if(origBB.size() == 0){
    return false;
  }else if(origBB.size() == 1){
    return true;
  }

  std::vector<BasicBlock *> shuffleBB = allBB;
  std::shuffle(shuffleBB.begin(), shuffleBB.end(), g);
  for (size_t num = 0; num < allBB.size(); num++) {
    if(allBB[num] != shuffleBB[num])
      allBB[num]->moveBefore(shuffleBB[num]);
  }

  for (size_t num = 0; num < origBB.size(); num++) {
    std::shuffle(downBB.begin(), downBB.end(), g);
    BasicBlock *i = origBB[num];
    BasicBlock *destBB = i->getTerminator()->getSuccessor(0);
    i->getTerminator()->eraseFromParent();
    BasicBlock *defaultBB = BasicBlock::Create(f->getContext(), "", f, shuffleBB[num]);
    CallInst::Create(generateGarbage(f), "", defaultBB);
    new UnreachableInst(f->getContext(), defaultBB);

    std::uniform_int_distribution<uint32_t> rand(0, UINT32_MAX);
    ConstantInt *c0 = ConstantInt::get(IntegerType::get(i->getContext(), 32), 0);
    ConstantInt *c1 = ConstantInt::get(IntegerType::get(i->getContext(), 32), 1);
    SwitchInst *switchII = SwitchInst::Create(c0, defaultBB, 0, i);
    int garbageCap = downBB.size()/4;
    garbageCap = garbageCap > 1 ? garbageCap : 1;
    for (BasicBlock *j: downBB) {
      ConstantInt *numCase = cast<ConstantInt>(ConstantInt::get(
          switchII->getCondition()->getType(),
          rand(g)));
      if(j == destBB){
        BinaryOperator *tempVal = nullptr;
        std::vector<Instruction::BinaryOps> vecBin{BinaryOperator::Xor, BinaryOperator::Add, BinaryOperator::Or};
        if(rand(g)%2){
          std::vector<Instruction::BinaryOps> vec1Bin{BinaryOperator::UDiv, BinaryOperator::Mul, BinaryOperator::SDiv};
          tempVal = BinaryOperator::Create(vecBin[rand(g)%(vecBin.size())], c0, c0, "", switchII);
          tempVal->setOperand(rand(g)%2, c1);
          tempVal = BinaryOperator::Create(vec1Bin[rand(g)%(vec1Bin.size())], numCase, tempVal, "", switchII);
        }else{
          tempVal = BinaryOperator::Create(vecBin[rand(g)%(vecBin.size())], c0, c0, "", switchII);
          tempVal->setOperand(rand(g)%2, numCase);
        }
        switchII->setCondition(tempVal);
        switchII->addCase(numCase, j);
      }else if(rand(g)%garbageCap == 0){
        switchII->addCase(numCase, j);
      }
    }
  }

  fixStack(f);

  return true;
}
