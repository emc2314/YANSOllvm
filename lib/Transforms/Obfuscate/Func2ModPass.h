#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct Func2ModPass : PassInfoMixin<Func2ModPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
