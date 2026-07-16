#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct ObfConPass : PassInfoMixin<ObfConPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm
