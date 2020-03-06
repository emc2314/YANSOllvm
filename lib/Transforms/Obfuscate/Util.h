#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"

void fixStack(llvm::Function *f);
llvm::InlineAsm *generateGarbage(llvm::Function *f);