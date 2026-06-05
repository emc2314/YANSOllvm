#ifndef LLVM_FLATTENING_H
#define LLVM_FLATTENING_H
// LLVM libs
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Local.h"
// #include "llvm/Transforms/IPO/PassManagerBuilder.h"
//  System libs
#include <cstdlib>
#include <ctime>
#include <vector>
namespace llvm {
class FlatteningPass : public PassInfoMixin<FlatteningPass> {
public:
  bool flag;
  FlatteningPass(bool flag) { this->flag = flag; } // 携带flag的构造函数
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  bool flattenImpl(Function &F);
  static bool isRequired() { return true; }
};
} // namespace llvm
#endif // LLVM_FLATTENING_H