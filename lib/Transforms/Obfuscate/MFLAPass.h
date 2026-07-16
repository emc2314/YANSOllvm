#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {
class MFLAPass : public PassInfoMixin<MFLAPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
} // namespace llvm
