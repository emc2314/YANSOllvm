#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct VMPass : PassInfoMixin<VMPass> {
  bool Enabled;
  explicit VMPass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
