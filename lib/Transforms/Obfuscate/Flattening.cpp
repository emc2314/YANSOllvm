//===- Flattening.cpp - Flattening Obfuscation pass------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the flattening pass
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils.h"

#include "Util.h"

#include <algorithm>
#include <random>
#include <numeric>

using namespace llvm;

// Stats

namespace {
struct Flattening : public FunctionPass {
  static char ID;

  Flattening() : FunctionPass(ID) {
    initializeLowerSwitchPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override{
    AU.addRequiredID(LowerSwitchID);
  }

  bool runOnFunction(Function &F) override;
  bool flatten(Function *f);
};
} // namespace

char Flattening::ID = 0;
static RegisterPass<Flattening> X("flattening", "Call graph flattening");
Pass *createFlatteningPass() { return new Flattening(); }

bool Flattening::runOnFunction(Function &F) {
  Function *tmp = &F;
  return flatten(tmp);
}

bool Flattening::flatten(Function *f) {
  std::vector<BasicBlock *> origBB;
  std::vector<uint32_t> bbIndex, bbHash;
  BasicBlock *loopEntry;
  LoadInst *load;
  SwitchInst *switchI;
  AllocaInst *switchVar, *hashVar;
  std::random_device rd;
  std::mt19937 g(rd());
  IntegerType *i32 = Type::getInt32Ty(f->getContext());
  ConstantInt *byteConst = ConstantInt::get(i32, 0xFF);
  ConstantInt *primeConst = ConstantInt::get(i32, fnvPrime);
  ConstantInt *basisConst = ConstantInt::get(i32, fnvBasis);

  // Save all original BB
  for (Function::iterator i = f->begin(); i != f->end(); ++i) {
    BasicBlock *tmp = &*i;

    if (isa<InvokeInst>(tmp->getTerminator())) {
      return false;
    }
    origBB.push_back(tmp);
  }

  // Nothing to flatten
  if (origBB.size() <= 1) {
    return false;
  }

  // Remove first BB
  origBB.erase(origBB.begin());

  // Get a pointer on the first BB
  Function::iterator tmp = f->begin();
  BasicBlock *insert = &*tmp;

  // If main begin with an if
  BranchInst *br = NULL;
  if (isa<BranchInst>(insert->getTerminator())) {
    br = cast<BranchInst>(insert->getTerminator());
  }

  if ((br != NULL && br->isConditional()) ||
      insert->getTerminator()->getNumSuccessors() > 1) {
    BasicBlock::iterator i = insert->end();
    --i;

    if (insert->size() > 1) {
      --i;
    }

    BasicBlock *tmpBB = insert->splitBasicBlock(i, "first");
    origBB.insert(origBB.begin(), tmpBB);
  }

  std::uniform_int_distribution<uint32_t> randomBBIndex(0, UINT32_MAX);
  for (size_t i = 0; i < origBB.size(); i++){
    uint32_t bbi = randomBBIndex(g);
    bbIndex.push_back(bbi);
    uint32_t bbh = fnvHash(bbi, fnvBasis);
    for (size_t j = 0; j < randomBBIndex(g)%1001; j++){
      assert(std::count(bbHash.begin(), bbHash.end(), bbh) == 0);
      bbh = fnvHash(bbi, bbh);
    }
    bbHash.push_back(bbh);

  }

  // Remove jump
  insert->getTerminator()->eraseFromParent();

  // Create switch variable and set as it
  switchVar = new AllocaInst(i32, 0, "switchVar", insert);
  new StoreInst(ConstantInt::get(i32, bbIndex[0]), switchVar, insert);
  hashVar = new AllocaInst(i32, 0, "hashVar", insert);
  new StoreInst(basisConst, hashVar, insert);

  // Create main loop
  loopEntry = BasicBlock::Create(f->getContext(), "loopEntry", f, insert);

  // Move first BB on top
  insert->moveBefore(loopEntry);
  BranchInst::Create(loopEntry, insert);

  //Calculate hash
  load = new LoadInst(switchVar, "switchVar", loopEntry);
  BinaryOperator *dataVal = BinaryOperator::Create(BinaryOperator::And, load,
                          ConstantInt::get(i32, 0xFFFFFFFF), "", loopEntry);
  BinaryOperator *hashVal = BinaryOperator::Create(BinaryOperator::And, new LoadInst(hashVar, "hashVar", loopEntry),
                          ConstantInt::get(i32, 0xFFFFFFFF), "", loopEntry);
  for(int i = 0; i < 4; i++){
    BinaryOperator *t = BinaryOperator::Create(BinaryOperator::And, dataVal, byteConst, "", loopEntry);
    hashVal = BinaryOperator::Create(BinaryOperator::Xor, hashVal, t, "", loopEntry);
    hashVal = BinaryOperator::Create(BinaryOperator::Mul, hashVal, primeConst, "", loopEntry);
    dataVal = BinaryOperator::Create(BinaryOperator::AShr, dataVal,
              ConstantInt::get(i32, 8), "", loopEntry);
  }
  new StoreInst(hashVal, hashVar, loopEntry);

  // Create switch instruction itself and set condition
  switchI = SwitchInst::Create(hashVal, loopEntry, 0, loopEntry);

  std::vector<size_t> bbSeq(origBB.size());
  std::iota(bbSeq.begin(), bbSeq.end(), 0);
  std::shuffle(bbSeq.begin(), bbSeq.end(), g);

  // Put all BB in the switch
  for (size_t b: bbSeq) {
    BasicBlock *i = origBB[b];
    ConstantInt *numCase = NULL;

    // Move the BB inside the switch (only visual, no code logic)
    i->moveBefore(loopEntry);

    // Add case to switch
    numCase = cast<ConstantInt>(ConstantInt::get(
        switchI->getCondition()->getType(),
        bbHash[b]));
    switchI->addCase(numCase, i);
  }

  // Recalculate switchVar
  for (size_t b: bbSeq) {
    BasicBlock *i = origBB[b];
    size_t succIndexTrue, succIndexFalse;
    SExtInst *cond = nullptr;

    // Ret BB
    if (i->getTerminator()->getNumSuccessors() == 0) {
      continue;
    }

    // If it's a non-conditional jump
    if (i->getTerminator()->getNumSuccessors() == 1) {
      cond = new SExtInst(ConstantInt::get(Type::getInt1Ty(f->getContext()), 0), i32, "", i->getTerminator());
      succIndexFalse = std::distance(origBB.begin(), std::find(origBB.begin(), origBB.end(), i->getTerminator()->getSuccessor(0)));
      succIndexTrue = std::distance(bbSeq.begin(), std::find(bbSeq.begin(), bbSeq.end(), b));

    } else {
      // If it's a conditional jump
      assert (i->getTerminator()->getNumSuccessors() == 2);
      cond = new SExtInst(cast<BranchInst>(i->getTerminator())->getCondition(), i32, "", i->getTerminator());
      succIndexFalse = std::distance(origBB.begin(), std::find(origBB.begin(), origBB.end(), i->getTerminator()->getSuccessor(1)));
      succIndexTrue = std::distance(origBB.begin(), std::find(origBB.begin(), origBB.end(), i->getTerminator()->getSuccessor(0)));
    }

    BinaryOperator *tempValFalse = BinaryOperator::Create(BinaryOperator::Xor,
                 ConstantInt::get(i32, bbIndex[b] ^ bbIndex[succIndexFalse]),
                  load, "", i->getTerminator());
    BinaryOperator *maskVal = BinaryOperator::Create(BinaryOperator::And, cond,
                 ConstantInt::get(i32, bbIndex[succIndexTrue] ^ bbIndex[succIndexFalse]), "", i->getTerminator());
    BinaryOperator *finalVal = BinaryOperator::Create(BinaryOperator::Xor, maskVal, tempValFalse, "", i->getTerminator());

    // Erase terminator
    i->getTerminator()->eraseFromParent();

    // Update switchVar and jump to the end of loop
    new StoreInst(finalVal, load->getPointerOperand(), i);
    new StoreInst(basisConst, hashVar, i);

    BranchInst::Create(loopEntry, i);
  }

  fixStack(f);

  return true;
}
