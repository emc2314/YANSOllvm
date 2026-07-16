#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct BB2FuncPass : PassInfoMixin<BB2FuncPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
