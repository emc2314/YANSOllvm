#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <random>
#include <vector>

using namespace llvm;

namespace {
struct Merge : public ModulePass {
  static char ID;
  Merge() : ModulePass(ID) {}

  bool runOnModule(Module &M) override;

  std::vector<Function *> mergeList;
};
} // namespace

char Merge::ID = 0;
static RegisterPass<Merge> X("merge", "Merge static functions");

bool Merge::runOnModule(Module &M) {
  for (Function &F : M) {
    if (F.getLinkage() == GlobalValue::InternalLinkage && !F.isVarArg() &&
        (F.getReturnType()->isIntOrPtrTy() || F.getReturnType()->isVoidTy())) {
      mergeList.push_back(&F);
    }
  }

  if (mergeList.size() < 2)
    return false;

  size_t retBitLen = 64;
  std::string funcName = "";
  std::vector<uint32_t> funcID;
  std::random_device rd;
  std::mt19937 g(rd());
  std::uniform_int_distribution<uint32_t> rand(0, UINT32_MAX);
  std::vector<Type *> paramTy;
  int ni32 = 0, ni64 = 0;
  std::vector<Type *> OtherTypes;
  IntegerType *i32 = IntegerType::get(M.getContext(), 32);
  IntegerType *i64 = IntegerType::get(M.getContext(), 64);
  paramTy.push_back(i32);
  for (Function *f : mergeList) {
    if (IntegerType *ty = dyn_cast<IntegerType>(f->getReturnType())) {
      if (ty->getBitWidth() > retBitLen) {
        retBitLen = ty->getBitWidth();
      }
    }
    int nfi32 = 0, nfi64 = 0;
    for (Type *ty : f->getFunctionType()->params()) {
      if (IntegerType *ti = dyn_cast<IntegerType>(ty)) {
        if (ti->getBitWidth() == 32) {
          nfi32++;
        } else if (ti->getBitWidth() == 64) {
          nfi64++;
        } else {
          OtherTypes.push_back(ty);
        }
      } else if (isa<PointerType>(ty)) {
        nfi64++;
      } else {
        OtherTypes.push_back(ty);
      }
    }
    if (nfi32 > ni32)
      ni32 = nfi32;
    if (nfi64 > ni64)
      ni64 = nfi64;
    funcName += std::string(f->getName()) + ".";
    funcID.push_back(rand(g));
  }
  for (int i = 0; i < ni32; i++) {
    paramTy.push_back(i32);
  }
  for (int i = 0; i < ni64; i++) {
    paramTy.push_back(i64);
  }
  for (Type *ty : OtherTypes) {
    paramTy.push_back(ty);
  }
  IntegerType *retTy = IntegerType::get(M.getContext(), retBitLen);
  FunctionType *funcTy = FunctionType::get(retTy, paramTy, false);
  Function *newFunction = Function::Create(funcTy, GlobalValue::InternalLinkage,
                                           funcName + "merge", M);
  newFunction->addFnAttr(Attribute::NoInline);

  for (size_t i = 0; i < mergeList.size(); i++) {
    std::vector<CallInst *> vecCall;
    for (Use &U : mergeList[i]->uses()) {
      CallInst *call = dyn_cast<CallInst>(U.getUser());
      if (!call) {
        errs() << "Not a call instruction use" << *(U.getUser()) << "\n";
      } else {
        vecCall.push_back(call);
      }
    }
    for (CallInst *call : vecCall) {
      ConstantInt *numCase = ConstantInt::get(i32, funcID[i]);
      std::vector<Value *> callArgs;
      std::vector<Value *> i32Args;
      std::vector<Value *> i64Args;
      std::vector<Value *> otherArgs;
      callArgs.push_back(numCase);
      for (Value *arg : call->args()) {
        Type *ty = arg->getType();
        if (IntegerType *ti = dyn_cast<IntegerType>(ty)) {
          if (ti->getBitWidth() == 32) {
            i32Args.push_back(arg);
          } else if (ti->getBitWidth() == 64) {
            i64Args.push_back(arg);
          } else {
            otherArgs.push_back(arg);
          }
        } else if (isa<PointerType>(ty)) {
          i64Args.push_back(new PtrToIntInst(arg, i64, "", call));
        } else {
          otherArgs.push_back(arg);
        }
      }
      for (int i = 0; i < ni32; i++) {
        if (i < (int)i32Args.size()) {
          callArgs.push_back(i32Args[i]);
        } else {
          callArgs.push_back(Constant::getNullValue(i32));
        }
      }
      for (int i = 0; i < ni64; i++) {
        if (i < (int)i64Args.size()) {
          callArgs.push_back(i64Args[i]);
        } else {
          callArgs.push_back(Constant::getNullValue(i64));
        }
      }
      for (Function *f : mergeList) {
        if (f != mergeList[i]) {
          for (Type *ty : f->getFunctionType()->params()) {
            if (IntegerType *ti = dyn_cast<IntegerType>(ty)) {
              if (ti->getBitWidth() == 32 || ti->getBitWidth() == 64) {
                continue;
              }
            } else if (isa<PointerType>(ty)) {
              continue;
            }
            callArgs.push_back(Constant::getNullValue(ty));
          }
        } else {
          for (Value *arg : otherArgs) {
            callArgs.push_back(arg);
          }
        }
      }
      CallInst *newCall = CallInst::Create(newFunction, callArgs, "", call);
      // errs() << "Replacing" << *call << " with" << *newCall << "\n";
      if (mergeList[i]->getReturnType()->isVoidTy()) {
      } else if (mergeList[i]->getReturnType()->isPointerTy()) {
        Value *replaced =
            new IntToPtrInst(newCall, mergeList[i]->getReturnType(), "", call);
        call->replaceAllUsesWith(replaced);
      } else if (cast<IntegerType>(mergeList[i]->getReturnType())
                     ->getBitWidth() < retBitLen) {
        Value *replaced =
            new TruncInst(newCall, mergeList[i]->getReturnType(), "", call);
        call->replaceAllUsesWith(replaced);
      } else {
        call->replaceAllUsesWith(newCall);
      }
      call->eraseFromParent();
    }
  }

  BasicBlock *entry = BasicBlock::Create(M.getContext(), "entry", newFunction);
  BasicBlock *switchB =
      BasicBlock::Create(M.getContext(), "switch", newFunction);
  BranchInst::Create(switchB, entry);
  SwitchInst *switchI =
      SwitchInst::Create(newFunction->arg_begin(), switchB, 0, switchB);
  for (size_t i = 0; i < mergeList.size(); i++) {
    BasicBlock *callFunc =
        BasicBlock::Create(M.getContext(), "", newFunction, switchB);
    Function::arg_iterator iti32Args = newFunction->arg_begin();
    std::advance(iti32Args, 1);
    Function::arg_iterator iti64Args = newFunction->arg_begin();
    std::advance(iti64Args, 1 + ni32);
    Function::arg_iterator itotherArgs = newFunction->arg_begin();
    std::advance(itotherArgs, 1 + ni32 + ni64);
    for (Function *f : mergeList) {
      if (f != mergeList[i]) {
        for (Type *ty : f->getFunctionType()->params()) {
          if (IntegerType *ti = dyn_cast<IntegerType>(ty)) {
            if (ti->getBitWidth() == 32 || ti->getBitWidth() == 64) {
              continue;
            }
          } else if (isa<PointerType>(ty)) {
            continue;
          }
          itotherArgs++;
        }
      } else {
        break;
      }
    }
    std::vector<Value *> callArgs;
    for (Argument &argument : mergeList[i]->args()) {
      Value *arg = &argument;
      Type *ty = arg->getType();
      if (IntegerType *ti = dyn_cast<IntegerType>(ty)) {
        if (ti->getBitWidth() == 32) {
          callArgs.push_back(iti32Args++);
        } else if (ti->getBitWidth() == 64) {
          callArgs.push_back(iti64Args++);
        } else {
          callArgs.push_back(itotherArgs++);
        }
      } else if (isa<PointerType>(ty)) {
        callArgs.push_back(new IntToPtrInst(iti64Args++, ty, "", callFunc));
      } else {
        callArgs.push_back(itotherArgs++);
      }
    }
    CallInst *callI = CallInst::Create(mergeList[i], callArgs, "", callFunc);
    if (mergeList[i]->getReturnType()->isVoidTy()) {
      ReturnInst::Create(M.getContext(), ConstantInt::get(retTy, 0), callFunc);
    } else if (mergeList[i]->getReturnType()->isPointerTy()) {
      ReturnInst::Create(M.getContext(),
                         new PtrToIntInst(callI, retTy, "", callFunc),
                         callFunc);
    } else if (cast<IntegerType>(mergeList[i]->getReturnType())->getBitWidth() <
               retBitLen) {
      ReturnInst::Create(M.getContext(),
                         new ZExtInst(callI, retTy, "", callFunc), callFunc);
    } else {
      ReturnInst::Create(M.getContext(), callI, callFunc);
    }
    ConstantInt *numCase = cast<ConstantInt>(
        ConstantInt::get(switchI->getCondition()->getType(), funcID[i]));
    switchI->addCase(numCase, callFunc);
    InlineFunctionInfo IFI;
    InlineFunction(callI, IFI);
  }

  for (size_t i = 0; i < mergeList.size(); i++) {
    if (mergeList[i]->isDefTriviallyDead()) {
      mergeList[i]->eraseFromParent();
    } else {
      for (const User *U : mergeList[i]->users())
        if (!isa<BlockAddress>(U))
          errs() << *U << "\n";
      errs() << mergeList[i]->getName() << " Not Dead Yet\n";
    }
  }

  return true;
}