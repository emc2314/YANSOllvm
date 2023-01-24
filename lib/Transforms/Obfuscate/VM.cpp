#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <random>
#include <vector>

using namespace llvm;

namespace {
struct Virtualize : public ModulePass {
  static char ID;
  Virtualize() : ModulePass(ID) {}

  bool runOnModule(Module &M) override;

private:
  Function *Add = nullptr;
  Function *CreateAdd(FunctionType *funcTy, Module &M);
  Function *Sub = nullptr;
  Function *CreateSub(FunctionType *funcTy, Module &M);
  Function *Shl = nullptr;
  Function *CreateShl(FunctionType *funcTy, Module &M);
  Function *AShr = nullptr;
  Function *CreateAShr(FunctionType *funcTy, Module &M);
  Function *LShr = nullptr;
  Function *CreateLShr(FunctionType *funcTy, Module &M);
  Function *And = nullptr;
  Function *CreateAnd(FunctionType *funcTy, Module &M);
  Function *Or = nullptr;
  Function *CreateOr(FunctionType *funcTy, Module &M);
  Function *Xor = nullptr;
  Function *CreateXor(FunctionType *funcTy, Module &M);
};
} // namespace

char Virtualize::ID = 0;
static RegisterPass<Virtualize> X("vm",
                                  "Use functions to do simple arithmetic");

Function *Virtualize::CreateAdd(FunctionType *funcTy, Module &M) {
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                 "__YANSOLLVM_VM_Add", M);
  Function::arg_iterator itArgs = f->arg_begin();
  Value *x = itArgs;
  Value *y = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  IRBuilder<> Builder(entry);
  // BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Add, x, y,
  // "", entry); x + y == (x|~y) + (~x&y) - (~(x&y)) + (x|y)
  Value *a = Builder.CreateNot(y);
  a = Builder.CreateOr(a, x);
  Value *b = Builder.CreateNot(x);
  b = Builder.CreateAnd(b, y);
  Value *c = Builder.CreateAnd(x, y);
  c = Builder.CreateNot(c);
  Value *d = Builder.CreateOr(x, y);
  Value *binOp = Builder.CreateAdd(a, b);
  binOp = Builder.CreateSub(binOp, c);
  binOp = Builder.CreateAdd(binOp, d);
  ReturnInst::Create(M.getContext(), binOp, entry);
  f->addFnAttr(Attribute::NoInline);
  f->addFnAttr(Attribute::OptimizeNone);
  return f;
}

Function *Virtualize::CreateSub(FunctionType *funcTy, Module &M) {
  if (!Virtualize::Add)
    Virtualize::Add = CreateAdd(funcTy, M);
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                 "__YANSOLLVM_VM_Sub", M);
  Function::arg_iterator itArgs = f->arg_begin();
  Value *x = itArgs;
  Value *y = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  IRBuilder<> Builder(entry);
  // BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Sub, x, y,
  // "", entry);
  // x - y == x + ~y + 1
  Value *ny = Builder.CreateNot(y);
  std::vector<Value *> callArgs;
  callArgs.push_back(x);
  callArgs.push_back(ny);
  Value *binOp = CallInst::Create(Virtualize::Add, callArgs, "", entry);
  binOp = Builder.CreateAdd(
      binOp, ConstantInt::get(cast<IntegerType>(x->getType()), 1));
  ReturnInst::Create(M.getContext(), binOp, entry);
  f->addFnAttr(Attribute::NoInline);
  f->addFnAttr(Attribute::OptimizeNone);
  return f;
}

Function *Virtualize::CreateShl(FunctionType *funcTy, Module &M) {
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                 "__YANSOLLVM_VM_Shl", M);
  Function::arg_iterator itArgs = f->arg_begin();
  Value *x = itArgs;
  Value *y = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp =
      BinaryOperator::Create(BinaryOperator::Shl, x, y, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  f->addFnAttr(Attribute::NoInline);
  f->addFnAttr(Attribute::OptimizeNone);
  return f;
}

Function *Virtualize::CreateAShr(FunctionType *funcTy, Module &M) {
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                 "__YANSOLLVM_VM_AShr", M);
  Function::arg_iterator itArgs = f->arg_begin();
  Value *x = itArgs;
  Value *y = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp =
      BinaryOperator::Create(BinaryOperator::AShr, x, y, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  f->addFnAttr(Attribute::NoInline);
  f->addFnAttr(Attribute::OptimizeNone);
  return f;
}

Function *Virtualize::CreateLShr(FunctionType *funcTy, Module &M) {
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                 "__YANSOLLVM_VM_LShr", M);
  Function::arg_iterator itArgs = f->arg_begin();
  Value *x = itArgs;
  Value *y = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  BinaryOperator *binOp =
      BinaryOperator::Create(BinaryOperator::LShr, x, y, "", entry);
  ReturnInst::Create(M.getContext(), binOp, entry);
  f->addFnAttr(Attribute::NoInline);
  f->addFnAttr(Attribute::OptimizeNone);
  return f;
}

Function *Virtualize::CreateAnd(FunctionType *funcTy, Module &M) {
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                 "__YANSOLLVM_VM_And", M);
  Function::arg_iterator itArgs = f->arg_begin();
  Value *x = itArgs;
  Value *y = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  IRBuilder<> Builder(entry);
  // BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::And, x, y,
  // "", entry); x & y == -(~(x&y)) + (~x|y) + (x&~y)
  Value *a = Builder.CreateAnd(x, y);
  a = Builder.CreateNot(a);
  Value *b = Builder.CreateNot(x);
  b = Builder.CreateOr(b, y);
  Value *c = Builder.CreateNot(y);
  c = Builder.CreateAnd(x, c);
  Value *binOp = Builder.CreateAdd(b, c);
  binOp = Builder.CreateSub(binOp, a);
  ReturnInst::Create(M.getContext(), binOp, entry);
  f->addFnAttr(Attribute::NoInline);
  f->addFnAttr(Attribute::OptimizeNone);
  return f;
}

Function *Virtualize::CreateOr(FunctionType *funcTy, Module &M) {
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                 "__YANSOLLVM_VM_Or", M);
  Function::arg_iterator itArgs = f->arg_begin();
  Value *x = itArgs;
  Value *y = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  IRBuilder<> Builder(entry);
  // BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Or, x, y,
  // "", entry); x | y == (x^y) + y - (~x&y)
  Value *a = Builder.CreateXor(x, y);
  Value *b = Builder.CreateNot(x);
  b = Builder.CreateAnd(b, y);
  Value *binOp = Builder.CreateAdd(a, y);
  binOp = Builder.CreateSub(binOp, b);
  ReturnInst::Create(M.getContext(), binOp, entry);
  f->addFnAttr(Attribute::NoInline);
  f->addFnAttr(Attribute::OptimizeNone);
  return f;
}

Function *Virtualize::CreateXor(FunctionType *funcTy, Module &M) {
  if (!Virtualize::Shl)
    Virtualize::Shl = CreateShl(funcTy, M);
  Function *f = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                 "__YANSOLLVM_VM_Xor", M);
  Function::arg_iterator itArgs = f->arg_begin();
  Value *x = itArgs;
  Value *y = ++itArgs;
  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", f);
  IRBuilder<> Builder(entry);
  // BinaryOperator *binOp = BinaryOperator::Create(BinaryOperator::Xor, x, y,
  // "", entry); x ^ y == x + y - ((x&y)<<1)
  Value *a = Builder.CreateAdd(x, y);
  Value *b = Builder.CreateAnd(x, y);
  std::vector<Value *> callArgs;
  callArgs.push_back(b);
  callArgs.push_back(ConstantInt::get(cast<IntegerType>(x->getType()), 1));
  Value *binOp = CallInst::Create(Virtualize::Shl, callArgs, "", entry);
  binOp = Builder.CreateSub(a, binOp);
  ReturnInst::Create(M.getContext(), binOp, entry);
  f->addFnAttr(Attribute::NoInline);
  f->addFnAttr(Attribute::OptimizeNone);
  return f;
}

bool Virtualize::runOnModule(Module &M) {
  bool modified = false;
  IntegerType *i64 = IntegerType::get(M.getContext(), 64);
  std::vector<Type *> paramTy = {i64, i64};
  FunctionType *funcTy = FunctionType::get(i64, paramTy, false);
  std::vector<BinaryOperator *> binOpIns;
  for (Function &F : M) {
    for (inst_iterator I = inst_begin(&F), E = inst_end(&F); I != E; ++I) {
      if (BinaryOperator *II = dyn_cast<BinaryOperator>(&*I)) {
        IntegerType *opType = cast<IntegerType>(II->getOperand(0)->getType());
        if (opType->getBitWidth() > 64)
          continue;
        switch (II->getOpcode()) {
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
  for (BinaryOperator *II : binOpIns) {
    IntegerType *opType = cast<IntegerType>(II->getOperand(0)->getType());
    Function *func = nullptr;
    bool isSigned = false;
    switch (II->getOpcode()) {
    case BinaryOperator::Add: {
      if (!Virtualize::Add)
        Virtualize::Add = CreateAdd(funcTy, M);
      func = Virtualize::Add;
      break;
    }
    case BinaryOperator::Sub: {
      if (!Virtualize::Sub)
        Virtualize::Sub = CreateSub(funcTy, M);
      func = Virtualize::Sub;
      break;
    }
    case BinaryOperator::Shl: {
      if (!Virtualize::Shl)
        Virtualize::Shl = CreateShl(funcTy, M);
      func = Virtualize::Shl;
      break;
    }
    case BinaryOperator::AShr: {
      if (!Virtualize::AShr)
        Virtualize::AShr = CreateAShr(funcTy, M);
      func = Virtualize::AShr;
      isSigned = true;
      break;
    }
    case BinaryOperator::LShr: {
      if (!Virtualize::LShr)
        Virtualize::LShr = CreateLShr(funcTy, M);
      func = Virtualize::LShr;
      break;
    }
    case BinaryOperator::And: {
      if (!Virtualize::And)
        Virtualize::And = CreateAnd(funcTy, M);
      func = Virtualize::And;
      break;
    }
    case BinaryOperator::Or: {
      if (!Virtualize::Or)
        Virtualize::Or = CreateOr(funcTy, M);
      func = Virtualize::Or;
      break;
    }
    case BinaryOperator::Xor: {
      if (!Virtualize::Xor)
        Virtualize::Xor = CreateXor(funcTy, M);
      func = Virtualize::Xor;
      break;
    }
    default:
      break;
    }
    if (func) {
      std::vector<Value *> callArgs;
      callArgs.push_back(CastInst::CreateIntegerCast(II->getOperand(0), i64,
                                                     isSigned, "", II));
      callArgs.push_back(CastInst::CreateIntegerCast(II->getOperand(1), i64,
                                                     isSigned, "", II));
      Value *replaced = CallInst::Create(func, callArgs, "", II);
      replaced = CastInst::CreateIntegerCast(replaced, opType, false, "", II);
      II->replaceAllUsesWith(replaced);
      II->eraseFromParent();
      modified = true;
    }
  }
  return modified;
}