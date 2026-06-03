#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct Func2ModPass : PassInfoMixin<Func2ModPass> {
  bool Enabled;
  unsigned NumOutputs;
  explicit Func2ModPass(bool Enabled = true, unsigned NumOutputs = 3)
      : Enabled(Enabled), NumOutputs(NumOutputs) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
