#include "YANSOllvmCommon.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <vector>

using namespace llvm;

namespace {
class VirtualizeImpl {
  Function *Add = nullptr;
  Function *Sub = nullptr;
  Function *Shl = nullptr;
  Function *AShr = nullptr;
  Function *LShr = nullptr;
  Function *And = nullptr;
  Function *Or = nullptr;
  Function *Xor = nullptr;

  static void attrs(Function *F) {
    F->addFnAttr(Attribute::NoInline);
    F->addFnAttr(Attribute::OptimizeNone);
  }

  Function *createAdd(FunctionType *FuncTy, Module &M) {
    Function *F = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                   "__yansollvm_vm_Add", M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    Value *A = B.CreateNot(Y);
    A = B.CreateOr(A, X);
    Value *C = B.CreateNot(X);
    C = B.CreateAnd(C, Y);
    Value *D = B.CreateAnd(X, Y);
    D = B.CreateNot(D);
    Value *E = B.CreateOr(X, Y);
    Value *R = B.CreateAdd(A, C);
    R = B.CreateSub(R, D);
    R = B.CreateAdd(R, E);
    B.CreateRet(R);
    attrs(F);
    return F;
  }

  Function *createSub(FunctionType *FuncTy, Module &M) {
    if (!Add)
      Add = createAdd(FuncTy, M);
    Function *F = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                   "__yansollvm_vm_Sub", M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    Value *NY = B.CreateNot(Y);
    Value *R = B.CreateCall(Add, {X, NY});
    R = B.CreateAdd(R, ConstantInt::get(cast<IntegerType>(X->getType()), 1));
    B.CreateRet(R);
    attrs(F);
    return F;
  }

  Function *createSimple(FunctionType *FuncTy, Module &M, StringRef Name,
                         Instruction::BinaryOps Op) {
    Function *F =
        Function::Create(FuncTy, GlobalValue::InternalLinkage, Name, M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    B.CreateRet(B.CreateBinOp(Op, X, Y));
    attrs(F);
    return F;
  }

  Function *createAnd(FunctionType *FuncTy, Module &M) {
    Function *F = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                   "__yansollvm_vm_And", M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    Value *A = B.CreateAnd(X, Y);
    A = B.CreateNot(A);
    Value *C = B.CreateNot(X);
    C = B.CreateOr(C, Y);
    Value *D = B.CreateNot(Y);
    D = B.CreateAnd(X, D);
    Value *R = B.CreateAdd(C, D);
    R = B.CreateSub(R, A);
    B.CreateRet(R);
    attrs(F);
    return F;
  }

  Function *createOr(FunctionType *FuncTy, Module &M) {
    Function *F = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                   "__yansollvm_vm_Or", M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    Value *A = B.CreateXor(X, Y);
    Value *C = B.CreateNot(X);
    C = B.CreateAnd(C, Y);
    Value *R = B.CreateAdd(A, Y);
    R = B.CreateSub(R, C);
    B.CreateRet(R);
    attrs(F);
    return F;
  }

  Function *createXor(FunctionType *FuncTy, Module &M) {
    if (!Shl)
      Shl = createSimple(FuncTy, M, "__yansollvm_vm_Shl", BinaryOperator::Shl);
    Function *F = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                   "__yansollvm_vm_Xor", M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    Value *A = B.CreateAdd(X, Y);
    Value *C = B.CreateAnd(X, Y);
    Value *R = B.CreateCall(
        Shl, {C, ConstantInt::get(cast<IntegerType>(X->getType()), 1)});
    R = B.CreateSub(A, R);
    B.CreateRet(R);
    attrs(F);
    return F;
  }

public:
  bool run(Module &M) {
    bool Modified = false;
    IntegerType *I64 = IntegerType::get(M.getContext(), 64);
    FunctionType *FuncTy = FunctionType::get(I64, {I64, I64}, false);
    std::vector<BinaryOperator *> BinOps;
    for (Function &F : M) {
      if (F.getName().starts_with("__yansollvm_vm_"))
        continue;
      for (Instruction &I : instructions(F)) {
        auto *BO = dyn_cast<BinaryOperator>(&I);
        if (!BO)
          continue;
        auto *OpTy = dyn_cast<IntegerType>(BO->getOperand(0)->getType());
        if (!OpTy || OpTy->getBitWidth() > 64)
          continue;
        switch (BO->getOpcode()) {
        case BinaryOperator::Add:
        case BinaryOperator::Sub:
        case BinaryOperator::Shl:
        case BinaryOperator::AShr:
        case BinaryOperator::LShr:
        case BinaryOperator::And:
        case BinaryOperator::Or:
        case BinaryOperator::Xor:
          BinOps.push_back(BO);
          break;
        default:
          break;
        }
      }
    }
    for (BinaryOperator *BO : BinOps) {
      auto *OpTy = cast<IntegerType>(BO->getOperand(0)->getType());
      Function *Func = nullptr;
      bool IsSigned = false;
      switch (BO->getOpcode()) {
      case BinaryOperator::Add:
        if (!Add)
          Add = createAdd(FuncTy, M);
        Func = Add;
        break;
      case BinaryOperator::Sub:
        if (!Sub)
          Sub = createSub(FuncTy, M);
        Func = Sub;
        break;
      case BinaryOperator::Shl:
        if (!Shl)
          Shl = createSimple(FuncTy, M, "__yansollvm_vm_Shl",
                             BinaryOperator::Shl);
        Func = Shl;
        break;
      case BinaryOperator::AShr:
        if (!AShr)
          AShr = createSimple(FuncTy, M, "__yansollvm_vm_AShr",
                              BinaryOperator::AShr);
        Func = AShr;
        IsSigned = true;
        break;
      case BinaryOperator::LShr:
        if (!LShr)
          LShr = createSimple(FuncTy, M, "__yansollvm_vm_LShr",
                              BinaryOperator::LShr);
        Func = LShr;
        break;
      case BinaryOperator::And:
        if (!And)
          And = createAnd(FuncTy, M);
        Func = And;
        break;
      case BinaryOperator::Or:
        if (!Or)
          Or = createOr(FuncTy, M);
        Func = Or;
        break;
      case BinaryOperator::Xor:
        if (!Xor)
          Xor = createXor(FuncTy, M);
        Func = Xor;
        break;
      default:
        break;
      }
      if (!Func)
        continue;
      IRBuilder<> B(BO);
      Value *A0 = B.CreateIntCast(BO->getOperand(0), I64, IsSigned);
      Value *A1 = B.CreateIntCast(BO->getOperand(1), I64, IsSigned);
      Value *R = B.CreateCall(Func, {A0, A1});
      R = B.CreateIntCast(R, OpTy, false);
      BO->replaceAllUsesWith(R);
      BO->eraseFromParent();
      Modified = true;
    }
    return Modified;
  }
};
} // namespace

PreservedAnalyses VMPass::run(Module &M, ModuleAnalysisManager &) {
  if (!Enabled)
    return PreservedAnalyses::all();
  VirtualizeImpl Impl;
  return Impl.run(M) ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
