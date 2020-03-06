#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"

void fixStack(llvm::Function *f);

const uint32_t fnvPrime = 16777633;
const uint32_t fnvBasis = 0x114514;
uint32_t fnvHash(const uint32_t data, uint32_t b);
llvm::InlineAsm *generateGarbage(llvm::Function *f);