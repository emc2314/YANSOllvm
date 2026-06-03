#include "CryptoUtils.h"
#include "YANSOllvmCommon.h"
#include "Utils.h"

#include "YANSOllvmSeed.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <vector>

using namespace llvm;

PreservedAnalyses ConnectPass::run(Function &F, FunctionAnalysisManager &) {
  if (!Enabled)
    return PreservedAnalyses::all();
  Function *Func = &F;
  std::vector<BasicBlock *> OrigBB, DownBB, AllBB;
  YansoRNG RNG(yanso_function_seed(F, "connect"));

  auto I = Func->begin();
  for (++I; I != Func->end(); ++I)
    OrigBB.push_back(&*I);

  for (auto B = OrigBB.begin(); B != OrigBB.end();) {
    BasicBlock *BB = *B;
    BasicBlock::iterator It = BB->getFirstInsertionPt();
    size_t BBSize = std::distance(It, BB->end());
    if (BBSize < 4) {
      B = OrigBB.erase(B);
      continue;
    }
    std::advance(It, BBSize / 2);
    BasicBlock *NewBB = BB->splitBasicBlock(It);
    DownBB.push_back(NewBB);
    AllBB.push_back(BB);
    AllBB.push_back(NewBB);
    ++B;
  }

  if (OrigBB.empty())
    return PreservedAnalyses::all();
  if (OrigBB.size() == 1)
    return PreservedAnalyses::none();

  std::vector<BasicBlock *> ShuffleBB = AllBB;
  RNG.shuffle(ShuffleBB);
  for (size_t Num = 0; Num < AllBB.size(); Num++)
    if (AllBB[Num] != ShuffleBB[Num])
      AllBB[Num]->moveBefore(ShuffleBB[Num]);

  for (size_t Num = 0; Num < OrigBB.size(); Num++) {
    RNG.shuffle(DownBB);
    BasicBlock *BB = OrigBB[Num];
    if (BB->getTerminator()->getNumSuccessors() == 0)
      continue;
    BasicBlock *DestBB = BB->getTerminator()->getSuccessor(0);
    BB->getTerminator()->eraseFromParent();
    BasicBlock *DefaultBB =
        BasicBlock::Create(Func->getContext(), "", Func, ShuffleBB[Num]);
    yansollvm_create_trap_block(Func, DefaultBB);

    ConstantInt *C0 =
        ConstantInt::get(IntegerType::get(BB->getContext(), 32), 0);
    ConstantInt *C1 =
        ConstantInt::get(IntegerType::get(BB->getContext(), 32), 1);
    SwitchInst *SwitchII = SwitchInst::Create(C0, DefaultBB, 0, BB);
    int GarbageCap = DownBB.size() / 4;
    GarbageCap = GarbageCap > 1 ? GarbageCap : 1;
    for (BasicBlock *J : DownBB) {
      ConstantInt *NumCase = ConstantInt::get(
          cast<IntegerType>(SwitchII->getCondition()->getType()), RNG.next32());
      if (J == DestBB) {
        BinaryOperator *TempVal = nullptr;
        std::vector<Instruction::BinaryOps> VecBin{
            BinaryOperator::Xor, BinaryOperator::Add, BinaryOperator::Or};
        if (RNG.range(2)) {
          std::vector<Instruction::BinaryOps> Vec1Bin{
              BinaryOperator::UDiv, BinaryOperator::Mul, BinaryOperator::SDiv};
          TempVal = BinaryOperator::Create(VecBin[RNG.range(VecBin.size())], C0,
                                           C0, "", it(SwitchII));
          TempVal->setOperand(RNG.range(2), C1);
          TempVal = BinaryOperator::Create(Vec1Bin[RNG.range(Vec1Bin.size())],
                                           NumCase, TempVal, "",
                                           it(SwitchII));
        } else {
          TempVal = BinaryOperator::Create(VecBin[RNG.range(VecBin.size())], C0,
                                           C0, "", it(SwitchII));
          TempVal->setOperand(RNG.range(2), NumCase);
        }
        SwitchII->setCondition(TempVal);
        SwitchII->addCase(NumCase, J);
      } else if (RNG.range(GarbageCap) == 0) {
        SwitchII->addCase(NumCase, J);
      }
    }
  }

  yansollvm_fix_stack(Func);
  return PreservedAnalyses::none();
}
