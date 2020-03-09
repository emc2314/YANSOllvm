#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <vector>
#include <random>

using namespace llvm;

namespace {
  struct Merge : public ModulePass {
    static char ID;
    Merge() : ModulePass(ID) {}

    bool runOnModule(Module &M) override;

    std::vector<Function *> mergeList;
  };
}

char Merge::ID = 0;
static RegisterPass<Merge> X("merge", "Merge static functions");

bool Merge::runOnModule(Module &M){
  for(Function &F: M){
    if(F.getLinkage() == GlobalValue::InternalLinkage && !F.isVarArg()
          && (F.getReturnType()->isIntOrPtrTy() || F.getReturnType()->isVoidTy())){
      mergeList.push_back(&F);
    }
  }

  if(mergeList.size() < 2)
    return false;

  size_t retBitLen = 64;
  std::string funcName = "";
  std::vector<uint32_t> funcID;
  std::random_device rd;
  std::mt19937 g(rd());
  std::uniform_int_distribution<uint32_t> rand(0, UINT32_MAX);
  std::vector<Type *> paramTy;
  paramTy.push_back(IntegerType::get(M.getContext(), 32));
  for(Function *f: mergeList){
    if(IntegerType *ty = dyn_cast<IntegerType>(f->getReturnType())){
      if(ty->getBitWidth() > retBitLen){
        retBitLen = ty->getBitWidth();
      }
    }
    for(Type *ty: f->getFunctionType()->params()){
      paramTy.push_back(ty);
    }
    funcName += std::string(f->getName()) + ".";
    funcID.push_back(rand(g));
  }
  IntegerType *retTy = IntegerType::get(M.getContext(), retBitLen);
  FunctionType *funcTy = FunctionType::get(retTy, paramTy, false);
  Function *newFunction = Function::Create(funcTy, GlobalValue::InternalLinkage, funcName + "merge", M);

  for(size_t i = 0; i < mergeList.size(); i++){
    std::vector<CallInst*> vecCall;
    for(Use &U: mergeList[i]->uses()){
      CallInst *call = dyn_cast<CallInst>(U.getUser());
      if(!call){
        errs() << "Not a call instruction use" << *(U.getUser()) << "\n";
      }else{
        vecCall.push_back(call);
      }
    }
    for(CallInst *call: vecCall){
      ConstantInt *numCase = cast<ConstantInt>(ConstantInt::get(
          IntegerType::get(M.getContext(), 32),
          funcID[i]));
      std::vector<Value*> callArgs;
      callArgs.push_back(numCase);
      for(Function *f: mergeList){
        if(f != mergeList[i]){
          for(Type *ty: f->getFunctionType()->params()){
            callArgs.push_back(Constant::getNullValue(ty));
          }
        }else{
          for(const auto &arg: call->args()){
            callArgs.push_back(arg);
          }
        }
      }
      CallInst *newCall = CallInst::Create(newFunction, callArgs, "", call);
      //errs() << "Replacing" << *call << " with" << *newCall << "\n";
      if(mergeList[i]->getReturnType()->isVoidTy()){
      }else if(mergeList[i]->getReturnType()->isPointerTy()){
        Value *replaced = new IntToPtrInst(newCall, mergeList[i]->getReturnType(), "", call);
        call->replaceAllUsesWith(replaced);
      }else if(cast<IntegerType>(mergeList[i]->getReturnType())->getBitWidth() < retBitLen){
        Value *replaced = new TruncInst(newCall, mergeList[i]->getReturnType(), "", call);
        call->replaceAllUsesWith(replaced);
      }else{
        call->replaceAllUsesWith(newCall);
      }
      call->eraseFromParent();
    }
  }

  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", newFunction);
  BasicBlock *switchB = BasicBlock::Create(M.getContext(), "switch", newFunction);
  BranchInst::Create(switchB, entry);
  Function::arg_iterator itArgs = newFunction->arg_begin();
  SwitchInst *switchI = SwitchInst::Create(itArgs++, switchB, 0, switchB);
  for(size_t i = 0; i < mergeList.size(); i++){
    std::vector<Value*> callArgs;
    for(__attribute__((unused)) const auto &j: mergeList[i]->args()){
      callArgs.push_back(itArgs++);
    }
    BasicBlock *callFunc = BasicBlock::Create(M.getContext(), "", newFunction, switchB);
    CallInst *callI = CallInst::Create(mergeList[i], callArgs, "", callFunc);
    if(mergeList[i]->getReturnType()->isVoidTy()){
      ReturnInst::Create(M.getContext(), ConstantInt::get(retTy, 0), callFunc);
    }else if(mergeList[i]->getReturnType()->isPointerTy()){
      ReturnInst::Create(M.getContext(),
            new PtrToIntInst(callI, retTy, "", callFunc), callFunc);
    }else if(cast<IntegerType>(mergeList[i]->getReturnType())->getBitWidth() < retBitLen){
      ReturnInst::Create(M.getContext(),
            new ZExtInst(callI, retTy, "", callFunc), callFunc);
    }else{
      ReturnInst::Create(M.getContext(), callI, callFunc);
    }
    ConstantInt *numCase = cast<ConstantInt>(ConstantInt::get(
        switchI->getCondition()->getType(),
        funcID[i]));
    switchI->addCase(numCase, callFunc);
    InlineFunctionInfo IFI;
    InlineFunction(callI, IFI);
    if(mergeList[i]->isDefTriviallyDead()){
      mergeList[i]->eraseFromParent();
    }else{
      for (const User *U : mergeList[i]->users())
        if (!isa<BlockAddress>(U))
          errs() << *U << "\n";
      errs() << mergeList[i]->getName() << " Not Dead Yet\n";
    }
  }

  return true;
}