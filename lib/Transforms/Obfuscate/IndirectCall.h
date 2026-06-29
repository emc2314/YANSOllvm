#pragma once
// LLVM libs
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
// User libs
#include "CryptoUtils.h"
#include "ObfuscationOptions.h"
#include "Utils.h"
// System libs

namespace llvm {
class IndirectCallPass : public PassInfoMixin<IndirectCallPass> {
public:
  bool flag;
  std::vector<CallInst *> CallSites;
  ObfuscationOptions *Options;
  std::vector<Function *> Callees;
  std::map<Function *, unsigned> CalleeNumbering;
  IndirectCallPass(bool flag) {
    this->flag = flag;
    this->Options = new ObfuscationOptions;
  } // 携带flag的构造函数
  bool doIndirctCall(Function &F);
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  GlobalVariable *getIndirectCallees(Function &F, ConstantInt *EncKey);
  void NumberCallees(Function &F);
  static bool isRequired() { return true; }
};
} // namespace llvm