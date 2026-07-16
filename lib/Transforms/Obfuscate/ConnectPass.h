#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct ConnectPass : PassInfoMixin<ConnectPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm
