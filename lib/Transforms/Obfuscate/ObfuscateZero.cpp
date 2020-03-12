#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "Util.h"

#include <vector>
#include <random>

using namespace llvm;

namespace {
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
    ConstantInt *isValidCandidateOperand(Value *V) const;
    void registerInteger(Value &V);
    Value *replaceZero(Instruction &Inst, ConstantInt *VReplace);
    Value *createExpression(Value* x, const uint32_t p, IRBuilder<>& Builder);
  };
}

bool ObfuscateZero::runOnBasicBlock(BasicBlock &BB) {
  IntegerVect.clear();
  bool modified = false;

  for (BasicBlock::iterator I = BB.getFirstInsertionPt(),
      end = BB.end();
      I != end; ++I) {
    Instruction &Inst = *I;
    if (isValidCandidateInstruction(Inst)) {
      size_t opSize = Inst.getNumOperands();
      //Do not obfuscate switch cases
      if (isa<SwitchInst>(&Inst))
        opSize = 1;
      for (size_t i = 0; i < opSize; ++i) {
        if (ConstantInt *C = isValidCandidateOperand(Inst.getOperand(i))) {
          if (Value *New_val = replaceZero(Inst, C)) {
            Inst.setOperand(i, New_val);
            modified = true;
          }
        }
      }
    }
    registerInteger(Inst);
  }
  return modified;
}

bool ObfuscateZero::isValidCandidateInstruction(Instruction &Inst) const {
  if (isa<GetElementPtrInst>(&Inst)) {
    return false;
  //} else if (isa<SwitchInst>(&Inst)) {
  //  return false;
  } else if (isa<ReturnInst>(&Inst)) {
    return false;
  } else if (isa<CallInst>(&Inst)) {
    return false;
  } else {
    return true;
  }
}

ConstantInt* ObfuscateZero::isValidCandidateOperand(Value *V) const {
  if (ConstantInt *C = dyn_cast<ConstantInt>(V)) {
    if (C->isZero()) {
      return C;
    } else {
      return nullptr;
    }
  } else {
    return nullptr;
  }
}

void ObfuscateZero::registerInteger(Value &V) {
  if (V.getType()->isIntegerTy() && !dyn_cast<llvm::ConstantInt>(&V))
    IntegerVect.push_back(&V);
}

Value *ObfuscateZero::createExpression(Value* x, const uint32_t p, IRBuilder<>& Builder) {
  Type *IntermediaryType = x->getType();
  std::uniform_int_distribution<size_t> RandAny(1, 256);
  Constant *any = ConstantInt::get(IntermediaryType, RandAny(Generator)),
           *prime = ConstantInt::get(IntermediaryType, p),
           *OverflowMask = ConstantInt::get(IntermediaryType, 0xFF);

  Value *temp = Builder.CreateOr(x, any);
  temp = Builder.CreateAnd(OverflowMask, temp);
  temp = Builder.CreateMul(temp,temp);
  temp = Builder.CreateMul(prime, temp);
  registerInteger(*temp);

  return temp;
}

Value* ObfuscateZero::replaceZero(Instruction &Inst, ConstantInt *VReplace) {
  IntegerType *ReplacedType = VReplace->getType(),
       *i32 = IntegerType::get(Inst.getParent()->getContext(),
           sizeof(uint32_t) * 8);

  if (IntegerVect.size() < 1) {
    return nullptr;
  }

  IRBuilder<> Builder(&Inst);
  std::uniform_int_distribution<size_t> Rand(0, IntegerVect.size() - 1);
  Value *temp = IntegerVect[Rand(Generator)];
  Value *x = Builder.CreateCast(CastInst::getCastOpcode(temp, false, i32, false), temp, i32);

  uint32_t randp1 = randPrime(1<<8, 1<<16);
  uint32_t randp2 = randPrime(1<<8, 1<<16);
  while(randp1 == randp2)
    randp2 = randPrime(1<<8, 1<<16);
  Value* LhsTot = createExpression(x, randp1, Builder);
  Value* RhsTot = createExpression(x, randp2, Builder);
  Value *comp =
    Builder.CreateICmp(CmpInst::ICMP_EQ, LhsTot, RhsTot);
  Value *replaced = Builder.CreateSExt(comp, ReplacedType);
  registerInteger(*replaced);

  return replaced;
}

char ObfuscateZero::ID = 0;
static RegisterPass<ObfuscateZero> X("obfZero", "Obfuscates zeroes",
    false, false);

// register pass for clang use
static void registerObfuscateZeroPass(const PassManagerBuilder &,
    legacy::PassManagerBase &PM) {
  PM.add(new ObfuscateZero());
}

static RegisterStandardPasses
RegisterMBAPass(PassManagerBuilder::EP_EarlyAsPossible,
    registerObfuscateZeroPass);
