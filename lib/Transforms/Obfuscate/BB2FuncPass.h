#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct BB2FuncPass : PassInfoMixin<BB2FuncPass> {
  bool Enabled;
  explicit BB2FuncPass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
