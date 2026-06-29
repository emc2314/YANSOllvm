#pragma once

#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace llvm {
class BasicBlock;
class Function;
class Module;

uint64_t yanso_module_seed(const Module &M, StringRef PassName);
uint64_t yanso_function_seed(const Function &F, StringRef PassName);
uint64_t yanso_basic_block_seed(const BasicBlock &BB, StringRef PassName);

} // namespace llvm

