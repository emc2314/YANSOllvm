#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct MergePass : PassInfoMixin<MergePass> {
  bool Enabled;
  explicit MergePass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
