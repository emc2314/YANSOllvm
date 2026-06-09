#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {
class MFLAPass : public PassInfoMixin<MFLAPass> {
  bool Enabled;

public:
  explicit MFLAPass(bool Enabled) : Enabled(Enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
} // namespace llvm
