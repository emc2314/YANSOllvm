#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct ObfConPass : PassInfoMixin<ObfConPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
