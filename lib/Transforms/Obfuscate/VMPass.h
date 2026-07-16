#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct VMPass : PassInfoMixin<VMPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
