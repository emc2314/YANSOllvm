#ifndef LLVM_INDIRECTBRANCH_H
#define LLVM_INDIRECTBRANCH_H
// LLVM libs
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
// User libs
#include "CryptoUtils.h"
#include "ObfuscationOptions.h"
#include "Utils.h"
using namespace llvm;
using namespace std;
namespace llvm { // 间接跳转
class IndirectBranchPass : public PassInfoMixin<IndirectBranchPass> {
public:
  bool flag;
  ObfuscationOptions *Options;
  std::map<BasicBlock *, unsigned> BBNumbering;
  std::vector<BasicBlock *> BBTargets; // all conditional branch targets
  IndirectBranchPass(bool flag) {
    this->flag = flag;
    this->Options = new ObfuscationOptions;
  } // 携带flag的构造函数
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM); // Pass实现函数
  void NumberBasicBlock(Function &F, YansoRNG &RNG);
  GlobalVariable *getIndirectTargets(Function &F, ConstantInt *EncKey);
  static bool isRequired() { return true; } // 直接返回true即可
};
} // namespace llvm
#endif // LLVM_INDIRECTBRANCH_H