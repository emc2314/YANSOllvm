#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct ConnectPass : PassInfoMixin<ConnectPass> {
  bool Enabled;
  explicit ConnectPass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm
