#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;

class FlatteningPass : public PassInfoMixin<FlatteningPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  bool flattenImpl(Function &F);
  static bool isRequired() { return true; }
};
} // namespace llvm