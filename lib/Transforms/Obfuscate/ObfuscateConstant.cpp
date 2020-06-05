#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"

#include "Util.h"

#include <vector>
#include <unordered_set>
#include <random>

using namespace llvm;

namespace {
  class ObfuscateConstant : public FunctionPass {
    private:
    std::vector<Value *> IntegerVect;
    std::unordered_set<Value *> OriginalInst;
    std::default_random_engine Generator;

    public:
    static char ID;
    ObfuscateConstant() : FunctionPass(ID) {}
    bool runOnFunction(Function &F) override;

    private:
    bool isValidCandidateInstruction(Instruction &Inst) const;
    ConstantInt *isSplitCandidateOperand(Value *V) const;
    ConstantInt *isObfCandidateOperand(Value *V) const;
    void registerInteger(Value &V, bool original=false);
    Value *replaceZero(Instruction &Inst, ConstantInt *VReplace);
    Value *createExpression(Value* x, const uint32_t p, IRBuilder<>& Builder);
    Value *splitConst(Instruction &Inst, ConstantInt *VReplace);
  };
}

char ObfuscateConstant::ID = 0;
static RegisterPass<ObfuscateConstant> X("obfCon", "Split and obfuscate constants");
Pass *createObfuscateConstantPass() { return new ObfuscateConstant(); }

bool ObfuscateConstant::runOnFunction(Function &F) {
  bool modified = false;

  OriginalInst.clear();
  for (auto &BB:F.getBasicBlockList()){
    for (BasicBlock::iterator I = BB.getFirstInsertionPt(),
        end = BB.end();
        I != end; ++I) {
      Instruction &Inst = *I;
      registerInteger(Inst, true);
    }
  }

  for (auto &BB:F.getBasicBlockList()){
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
          if (ConstantInt *C = isSplitCandidateOperand(Inst.getOperand(i))) {
            if (CallInst *CI = dyn_cast<CallInst>(&Inst))
              if(CI->paramHasAttr(i, Attribute::ImmArg))
                break;
            if (Value *New_val = splitConst(Inst, C)) {
              Inst.setOperand(i, New_val);
              modified = true;
            }
          }
        }
      }
    }

    IntegerVect.clear();
    BasicBlock *PBB = BB.getSinglePredecessor();
    while(PBB){
      for (BasicBlock::iterator I = PBB->getFirstInsertionPt(),
          end = PBB->end();
          I != end; ++I) {
        Instruction &Inst = *I;
        if(std::count(OriginalInst.begin(), OriginalInst.end(), &Inst))
          registerInteger(Inst);
      }
      PBB = PBB->getSinglePredecessor();
    }
    for(Argument &argument: F.args()){
      Value *arg = &argument;
      registerInteger(*arg);
    }
    for (BasicBlock::iterator I = BB.getFirstInsertionPt(),
        end = BB.end();
        I != end; ++I) {
      Instruction &Inst = *I;
      if (isValidCandidateInstruction(Inst)) {
        size_t opSize = Inst.getNumOperands();
        //Do not obfuscate switch cases
        if (isa<SwitchInst>(&Inst))
          opSize = 1;
        //Do not obfzero function args
        if (isa<CallInst>(&Inst))
          opSize = 0;
        for (size_t i = 0; i < opSize; ++i) {
          if (ConstantInt *C = isObfCandidateOperand(Inst.getOperand(i))) {
            if (Value *New_val = replaceZero(Inst, C)) {
              Inst.setOperand(i, New_val);
              modified = true;
            }
          }
        }
      }
      if(std::count(OriginalInst.begin(), OriginalInst.end(), &Inst))
        registerInteger(Inst);
    }
  }
  return modified;
}

bool ObfuscateConstant::isValidCandidateInstruction(Instruction &Inst) const {
  if (isa<GetElementPtrInst>(&Inst)) {
    return false;
  //} else if (isa<SwitchInst>(&Inst)) {
  //  return false;
  } else if (isa<ReturnInst>(&Inst)) {
    return false;
  //} else if (isa<CallInst>(&Inst)) {
  //  return false;
  } else {
    return true;
  }
}

ConstantInt* ObfuscateConstant::isSplitCandidateOperand(Value *V) const {
  if (ConstantInt *C = dyn_cast<ConstantInt>(V)) {
    uint64_t v = C->getValue().getLimitedValue();
    if (v && v != UINT64_MAX) {
      return C;
    } else {
      return nullptr;
    }
  } else {
    return nullptr;
  }
}

ConstantInt* ObfuscateConstant::isObfCandidateOperand(Value *V) const {
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

Value* ObfuscateConstant::splitConst(Instruction &Inst, ConstantInt *VReplace) {
  IntegerType *ReplacedType = VReplace->getType(),
       *i64 = IntegerType::get(Inst.getParent()->getContext(),
           sizeof(uint64_t) * 8);

  std::uniform_int_distribution<uint64_t> urand64(0, (UINT64_MAX >> 1) - 1);
  IRBuilder<> Builder(&Inst);
  Value *replaced = Builder.CreateIntCast(VReplace, i64, true);
  uint64_t v = VReplace->getValue().getLimitedValue();
  uint64_t randv = urand64(Generator)*2 + 1;
  BinaryOperator *rv1 = BinaryOperator::Create(BinaryOperator::Add, ConstantInt::get(i64, randv), ConstantInt::get(i64, 0), "", &Inst);
  BinaryOperator *rv2 = BinaryOperator::Create(BinaryOperator::Xor, ConstantInt::get(i64, modinv(randv)*v), ConstantInt::get(i64, 0), "", &Inst);
  replaced = Builder.CreateMul(rv1, rv2);
  replaced = Builder.CreateIntCast(replaced, ReplacedType, true);

  return replaced;
}

void ObfuscateConstant::registerInteger(Value &V, bool original) {
  if (V.getType()->isIntegerTy() && !dyn_cast<llvm::ConstantInt>(&V)){
    if(original)
      OriginalInst.insert(&V);
    else
      IntegerVect.push_back(&V);
  }
}

Value *ObfuscateConstant::createExpression(Value* x, const uint32_t p, IRBuilder<>& Builder) {
  Type *IntermediaryType = x->getType();
  std::uniform_int_distribution<size_t> RandAny(1, 255);
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

Value* ObfuscateConstant::replaceZero(Instruction &Inst, ConstantInt *VReplace) {
  IntegerType *ReplacedType = VReplace->getType(),
       *i32 = IntegerType::get(Inst.getParent()->getContext(),
           sizeof(uint32_t) * 8);

  Value *replaced = nullptr;

  if(IntegerVect.size() > 0){
    IRBuilder<> Builder(&Inst);
    std::uniform_int_distribution<size_t> Rand(0, IntegerVect.size() - 1);
    std::uniform_int_distribution<uint32_t> randswitch(0, 2);
    size_t ix = Rand(Generator);
    Value *temp = IntegerVect[ix];
    Value *x = Builder.CreateIntCast(temp, i32, false);
    if(IntegerVect.size() == 1){
      // ((~x | 0x7AFAFA69) & 0xA061440) + ((x & 0x1050504) | 0x1010104) == 185013572
      temp = Builder.CreateNot(x);
      temp = Builder.CreateOr(temp, ConstantInt::get(i32, 0x7AFAFA69));
      temp = Builder.CreateAnd(temp, ConstantInt::get(i32, 0xA061440));
      replaced = Builder.CreateAnd(x, ConstantInt::get(i32, 0x1050504));
      replaced = Builder.CreateOr(replaced, ConstantInt::get(i32, 0x1010104));
      replaced = Builder.CreateAdd(replaced, temp);
      replaced = Builder.CreateXor(replaced, ConstantInt::get(i32, 185013572));
      replaced = Builder.CreateIntCast(replaced, ReplacedType, false);
    }else{
      size_t iy = Rand(Generator);
      while(ix == iy)
        iy = Rand(Generator);
      temp = IntegerVect[iy];
      Value *y = Builder.CreateIntCast(temp, i32, false);

      switch(randswitch(Generator)){
        case 0:{
          // p1*(x|any)**2 != p2*(y|any)**2
          uint32_t randp1 = randPrime(1<<8, 1<<16);
          uint32_t randp2 = randPrime(1<<8, 1<<16);
          while(randp1 == randp2)
            randp2 = randPrime(1<<8, 1<<16);
          Value* LhsTot = createExpression(x, randp1, Builder);
          Value* RhsTot = createExpression(y, randp2, Builder);
          Value *comp =
            Builder.CreateICmp(CmpInst::ICMP_EQ, LhsTot, RhsTot);
          replaced = Builder.CreateSExt(comp, ReplacedType);
          break;
        }
        case 1:{
          // x + y = x^y + 2*(x&y)
          replaced = Builder.CreateAdd(x,y);
          temp = Builder.CreateXor(x,y);
          replaced = Builder.CreateSub(replaced, temp);
          temp = Builder.CreateAnd(x,y);
          temp = Builder.CreateShl(temp, ConstantInt::get(i32, 1));
          replaced = Builder.CreateXor(replaced, temp);
          replaced = Builder.CreateIntCast(replaced, ReplacedType, false);
          break;
        }
        case 2:{
          // x ^ y == (x|~y) - 3*(~(x|y)) + 2*(~x) - y
          Value *a = Builder.CreateNot(y);
          a = Builder.CreateOr(x, a);
          Value *b = Builder.CreateOr(x,y);
          b = Builder.CreateNot(b);
          b = Builder.CreateMul(b, ConstantInt::get(i32, -3));
          Value *c = Builder.CreateNot(x);
          c = Builder.CreateMul(c, ConstantInt::get(i32, 2));
          c = Builder.CreateSub(c, y);
          replaced = Builder.CreateXor(x,y);
          replaced = Builder.CreateSub(replaced, a);
          replaced = Builder.CreateSub(replaced, b);
          replaced = Builder.CreateXor(replaced, c);
          replaced = Builder.CreateIntCast(replaced, ReplacedType, false);
          break;
        }
        default:
          errs() << "Impossible\n";
      }
    }
    registerInteger(*replaced);
  }

  return replaced;
}
