#pragma once

#include "CryptoUtils.h"
#include "llvm/IR/Function.h"

#include <set>

namespace llvm {

// Repair PHI nodes and escaped values invalidated by CFG rewriting.
void yansollvm_fix_stack(Function *F, const std::set<BasicBlock *> *SkipPhiBlocks = nullptr,
                         const std::set<Instruction *> *SkipRegs = nullptr);
bool yansollvm_has_dynamic_stack_state(BasicBlock &BB);
bool yansollvm_has_dynamic_stack_state(Function &F);
void yansollvm_create_trap_block(Function *F, BasicBlock *BB);
uint32_t yansollvm_rand_prime(uint32_t Min, uint32_t Max, YansoRNG &RNG);

} // namespace llvm
