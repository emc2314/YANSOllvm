//===--------- ObfuscateZero.h - How to use the LoopInfo analysis ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass implements a simple instruction obfuscator, which replaces
// occurrences of "zero" as a constant with inequalities that always
// return false.
//
// Author: Merlini Adrien.
// Date: January 14th, 2015.
// Usage: opt -load Obfuscator.dylib -obfZero input.bc -o output.bc
//
//===----------------------------------------------------------------------===//

#ifndef _OBFUSCATE_ZERO_H_
#define _OBFUSCATE_ZERO_H_

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/IR/IRBuilder.h"

#include <vector>
#include <random>

using namespace llvm;

namespace {

  class vector;
  class random;

  class ObfuscateZero : public BasicBlockPass {
    private:
    std::vector<Value *> IntegerVect;
    std::default_random_engine Generator;

    public:
    static char ID;

    ObfuscateZero() : BasicBlockPass(ID) {}

    bool runOnBasicBlock(BasicBlock &BB) override;

    private:

    bool isValidCandidateInstruction(Instruction &Inst) const;

    Constant *isValidCandidateOperand(Value *V) const;

    void registerInteger(Value &V);

    // We replace 0 with:
    // prime1 * ((x | any1)**2) != prime2 * ((y | any2)**2)
    // with prime1 != prime2 and any1 != 0 and any2 != 0
    Value *replaceZero(Instruction &Inst, Value *VReplace);

    Value *createExpression(Type* IntermediaryType, const uint32_t p, IRBuilder<>& Builder);
  };
}

#endif
