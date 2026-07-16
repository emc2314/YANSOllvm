#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct MergePass : PassInfoMixin<MergePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
