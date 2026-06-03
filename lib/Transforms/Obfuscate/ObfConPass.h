#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct ObfConPass : PassInfoMixin<ObfConPass> {
  bool Enabled;
  explicit ObfConPass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm
