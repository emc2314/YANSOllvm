#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"

#include <vector>
#include <random>

using namespace llvm;

namespace {
  struct Virtualize : public ModulePass {
    static char ID;
    Virtualize() : ModulePass(ID) {}

    bool runOnModule(Module &M) override;

    private:
    Function *Add;
    Function *Sub;
    Function *Shl;
    Function *AShr;
    Function *LShr;
    Function *And;
    Function *Or;
    Function *Xor;
  };
}

char Virtualize::ID = 0;
static RegisterPass<Virtualize> X("vm", "Use functions to do simple arithmetic");

Function *CreateAdd(FunctionType *funcTy, Module &M){
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage, "__YANSOLLVM_VM_Add", M);
  Function::arg_iterator itArgs = f->arg_begin(); Value *op0 = itArgs; Value *op1 = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Add, op0, op1, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  return f;
}

Function *CreateSub(FunctionType *funcTy, Module &M){
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage, "__YANSOLLVM_VM_Sub", M);
  Function::arg_iterator itArgs = f->arg_begin(); Value *op0 = itArgs; Value *op1 = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Sub, op0, op1, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  return f;
}

Function *CreateShl(FunctionType *funcTy, Module &M){
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage, "__YANSOLLVM_VM_Shl", M);
  Function::arg_iterator itArgs = f->arg_begin(); Value *op0 = itArgs; Value *op1 = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Shl, op0, op1, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  return f;
}

Function *CreateAShr(FunctionType *funcTy, Module &M){
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage, "__YANSOLLVM_VM_AShr", M);
  Function::arg_iterator itArgs = f->arg_begin(); Value *op0 = itArgs; Value *op1 = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::AShr, op0, op1, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  return f;
}

Function *CreateLShr(FunctionType *funcTy, Module &M){
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage, "__YANSOLLVM_VM_LShr", M);
  Function::arg_iterator itArgs = f->arg_begin(); Value *op0 = itArgs; Value *op1 = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::LShr, op0, op1, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  return f;
}

Function *CreateAnd(FunctionType *funcTy, Module &M){
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage, "__YANSOLLVM_VM_And", M);
  Function::arg_iterator itArgs = f->arg_begin(); Value *op0 = itArgs; Value *op1 = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::And, op0, op1, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  return f;
}

Function *CreateOr(FunctionType *funcTy, Module &M){
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage, "__YANSOLLVM_VM_Or", M);
  Function::arg_iterator itArgs = f->arg_begin(); Value *op0 = itArgs; Value *op1 = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Or, op0, op1, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  return f;
}

Function *CreateXor(FunctionType *funcTy, Module &M){
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage, "__YANSOLLVM_VM_Xor", M);
  Function::arg_iterator itArgs = f->arg_begin(); Value *op0 = itArgs; Value *op1 = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Xor, op0, op1, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  return f;
}

bool Virtualize::runOnModule(Module &M){
  bool modified = false;
  IntegerType *i64 = IntegerType::get(M.getContext(), 64);
  std::vector<Type *> paramTy = {i64, i64};
  FunctionType *funcTy = FunctionType::get(i64, paramTy, false);
  std::vector<BinaryOperator *> binOpIns;
  for(Function &F: M){
    for(inst_iterator I = inst_begin(&F), E = inst_end(&F); I != E; ++I){
      if(BinaryOperator *II = dyn_cast<BinaryOperator>(&*I)){
        IntegerType *opType = dyn_cast<IntegerType>(II->getOperand(0)->getType());
        if(opType->getBitWidth() > 64)
          continue; 
        switch(II->getOpcode()){
          case BinaryOperator::Add:
          case BinaryOperator::Sub:
          case BinaryOperator::Shl:
          case BinaryOperator::AShr:
          case BinaryOperator::LShr:
          case BinaryOperator::And:
          case BinaryOperator::Or:
          case BinaryOperator::Xor:
            binOpIns.push_back(II);
            break;
          default:
            break;
        }
      }
    }
  }
  for(BinaryOperator *II: binOpIns){
    IntegerType *opType = dyn_cast<IntegerType>(II->getOperand(0)->getType());
    Value *replaced = nullptr;
    std::vector<Value*> callArgs;
    switch(II->getOpcode()){
      case BinaryOperator::Add:{
        if(!Virtualize::Add)
          Virtualize::Add = CreateAdd(funcTy, M);
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(0), i64, false, "", II));
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(1), i64, false, "", II));
        replaced = CallInst::Create(Virtualize::Add, callArgs, "", II);
        break;
      }
      case BinaryOperator::Sub:{
        if(!Virtualize::Sub)
          Virtualize::Sub = CreateSub(funcTy, M);
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(0), i64, false, "", II));
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(1), i64, false, "", II));
        replaced = CallInst::Create(Virtualize::Sub, callArgs, "", II);
        break;
      }
      case BinaryOperator::Shl:{
        if(!Virtualize::Shl)
          Virtualize::Shl = CreateShl(funcTy, M);
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(0), i64, false, "", II));
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(1), i64, false, "", II));
        replaced = CallInst::Create(Virtualize::Shl, callArgs, "", II);
        break;
      }
      case BinaryOperator::AShr:{
        if(!Virtualize::AShr)
          Virtualize::AShr = CreateAShr(funcTy, M);
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(0), i64, true, "", II));
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(1), i64, true, "", II));
        replaced = CallInst::Create(Virtualize::AShr, callArgs, "", II);
        break;
      }
      case BinaryOperator::LShr:{
        if(!Virtualize::LShr)
          Virtualize::LShr = CreateLShr(funcTy, M);
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(0), i64, false, "", II));
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(1), i64, false, "", II));
        replaced = CallInst::Create(Virtualize::LShr, callArgs, "", II);
        break;
      }
      case BinaryOperator::And:{
        if(!Virtualize::And)
          Virtualize::And = CreateAnd(funcTy, M);
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(0), i64, false, "", II));
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(1), i64, false, "", II));
        replaced = CallInst::Create(Virtualize::And, callArgs, "", II);
        break;
      }
      case BinaryOperator::Or:{
        if(!Virtualize::Or)
          Virtualize::Or = CreateOr(funcTy, M);
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(0), i64, false, "", II));
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(1), i64, false, "", II));
        replaced = CallInst::Create(Virtualize::Or, callArgs, "", II);
        break;
      }
      case BinaryOperator::Xor:{
        if(!Virtualize::Xor)
          Virtualize::Xor = CreateXor(funcTy, M);
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(0), i64, false, "", II));
        callArgs.push_back(CastInst::CreateIntegerCast(
                           II->getOperand(1), i64, false, "", II));
        replaced = CallInst::Create(Virtualize::Xor, callArgs, "", II);
        break;
      }
      default:
        break;
    }
    if(replaced){
      modified = true;
      replaced = CastInst::CreateIntegerCast(replaced, opType, false, "", II);
      II->replaceAllUsesWith(replaced);
      II->eraseFromParent();
    }
  }
  return modified;
}