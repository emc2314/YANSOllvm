#pragma once

#include "CryptoUtils.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

struct VMPass : PassInfoMixin<VMPass> {
  bool Enabled;
  explicit VMPass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct MergePass : PassInfoMixin<MergePass> {
  bool Enabled;
  explicit MergePass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct Func2ModPass : PassInfoMixin<Func2ModPass> {
  bool Enabled;
  unsigned NumOutputs;
  explicit Func2ModPass(bool Enabled = true, unsigned NumOutputs = 3)
      : Enabled(Enabled), NumOutputs(NumOutputs) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct BB2FuncPass : PassInfoMixin<BB2FuncPass> {
  bool Enabled;
  explicit BB2FuncPass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

struct ConnectPass : PassInfoMixin<ConnectPass> {
  bool Enabled;
  explicit ConnectPass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

struct ObfConPass : PassInfoMixin<ObfConPass> {
  bool Enabled;
  explicit ObfConPass(bool Enabled = true) : Enabled(Enabled) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

void yansollvm_fix_stack(Function *F);
void yansollvm_create_trap_block(Function *F, BasicBlock *BB);
uint32_t yansollvm_rand_prime(uint32_t Min, uint32_t Max, YansoRNG &RNG);
uint64_t yansollvm_mod_inv(uint64_t A);

} // namespace llvm
