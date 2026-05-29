#ifndef LLVM_SPLIT_BASIC_BLOCK_H
#define LLVM_SPLIT_BASIC_BLOCK_H
// LLVM libs
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h" //new Pass
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
// System libs
#include <vector>
namespace llvm { // 基本块分割
class SplitBasicBlockPass : public PassInfoMixin<SplitBasicBlockPass> {
public:
  bool flag;
  SplitBasicBlockPass(bool flag) { this->flag = flag; } // 携带flag的构造函数
  PreservedAnalyses run(Function &F,
                        FunctionAnalysisManager &AM); // Pass实现函数
  bool split(Function *f);          // 对单个基本块执行分裂操作
  bool containsPHI(BasicBlock *BB); // 判断一个基本块中是否包含 PHI指令(PHINode)
  static bool isRequired() { return true; } // 直接返回true即可
};
SplitBasicBlockPass *createSplitBasicBlock(bool flag); // 创建基本块分割
} // namespace llvm
#endif // LLVM_SPLIT_BASIC_BLOCK_H