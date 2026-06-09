#include "CryptoUtils.h"
#include "MergePass.h"
#include "Utils.h"

#include "YANSOllvmSeed.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <vector>

using namespace llvm;

PreservedAnalyses MergePass::run(Module &M, ModuleAnalysisManager &) {
  if (!Enabled)
    return PreservedAnalyses::all();

  std::vector<Function *> MergeList;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.getLinkage() == GlobalValue::InternalLinkage && !F.isVarArg() &&
        (F.getReturnType()->isIntOrPtrTy() || F.getReturnType()->isVoidTy()))
      MergeList.push_back(&F);
  }
  if (MergeList.size() < 2) {
    YANSO_WARN_MODULE(
        "merge", M,
        "fewer than two eligible internal non-vararg int/pointer/void-return functions");
    return PreservedAnalyses::all();
  }

  size_t RetBitLen = 64;
  std::string FuncName;
  std::vector<uint32_t> FuncID;
  YansoRNG RNG(yanso_module_seed(M, "merge"));
  std::vector<Type *> ParamTy;
  int NI32 = 0, NI64 = 0;
  std::vector<Type *> OtherTypes;
  IntegerType *I32 = IntegerType::get(M.getContext(), 32);
  IntegerType *I64 = IntegerType::get(M.getContext(), 64);
  ParamTy.push_back(I32);

  for (Function *F : MergeList) {
    if (auto *Ty = dyn_cast<IntegerType>(F->getReturnType()))
      RetBitLen = std::max<size_t>(RetBitLen, Ty->getBitWidth());
    int NFI32 = 0, NFI64 = 0;
    for (Type *Ty : F->getFunctionType()->params()) {
      if (auto *TI = dyn_cast<IntegerType>(Ty)) {
        if (TI->getBitWidth() == 32)
          NFI32++;
        else if (TI->getBitWidth() == 64)
          NFI64++;
        else
          OtherTypes.push_back(Ty);
      } else if (isa<PointerType>(Ty)) {
        NFI64++;
      } else {
        OtherTypes.push_back(Ty);
      }
    }
    NI32 = std::max(NI32, NFI32);
    NI64 = std::max(NI64, NFI64);
    FuncName += std::string(F->getName()) + ".";
    FuncID.push_back(RNG.next32());
  }
  for (int I = 0; I < NI32; I++)
    ParamTy.push_back(I32);
  for (int I = 0; I < NI64; I++)
    ParamTy.push_back(I64);
  for (Type *Ty : OtherTypes)
    ParamTy.push_back(Ty);

  IntegerType *RetTy = IntegerType::get(M.getContext(), RetBitLen);
  FunctionType *FuncTy = FunctionType::get(RetTy, ParamTy, false);
  Function *NewFunction = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                           FuncName + "merge", M);
  NewFunction->addFnAttr(Attribute::NoInline);

  for (size_t I = 0; I < MergeList.size(); I++) {
    std::vector<CallInst *> VecCall;
    for (Use &U : MergeList[I]->uses()) {
      if (auto *Call = dyn_cast<CallInst>(U.getUser())) {
        if (Call->getCalledFunction() == MergeList[I])
          VecCall.push_back(Call);
      }
    }
    for (CallInst *Call : VecCall) {
      std::vector<Value *> CallArgs;
      std::vector<Value *> I32Args, I64Args, OtherArgs;
      CallArgs.push_back(ConstantInt::get(I32, FuncID[I]));
      for (Value *Arg : Call->args()) {
        Type *Ty = Arg->getType();
        if (auto *TI = dyn_cast<IntegerType>(Ty)) {
          if (TI->getBitWidth() == 32)
            I32Args.push_back(Arg);
          else if (TI->getBitWidth() == 64)
            I64Args.push_back(Arg);
          else
            OtherArgs.push_back(Arg);
        } else if (isa<PointerType>(Ty)) {
          I64Args.push_back(
              new PtrToIntInst(Arg, I64, "", it(Call)));
        } else {
          OtherArgs.push_back(Arg);
        }
      }
      for (int J = 0; J < NI32; J++)
        CallArgs.push_back(
            J < (int)I32Args.size() ? I32Args[J] : Constant::getNullValue(I32));
      for (int J = 0; J < NI64; J++)
        CallArgs.push_back(
            J < (int)I64Args.size() ? I64Args[J] : Constant::getNullValue(I64));
      for (Function *F : MergeList) {
        if (F != MergeList[I]) {
          for (Type *Ty : F->getFunctionType()->params()) {
            if (auto *TI = dyn_cast<IntegerType>(Ty))
              if (TI->getBitWidth() == 32 || TI->getBitWidth() == 64)
                continue;
            if (isa<PointerType>(Ty))
              continue;
            CallArgs.push_back(Constant::getNullValue(Ty));
          }
        } else {
          for (Value *Arg : OtherArgs)
            CallArgs.push_back(Arg);
        }
      }
      CallInst *NewCall =
          CallInst::Create(NewFunction, CallArgs, "", it(Call));
      if (MergeList[I]->getReturnType()->isVoidTy()) {
      } else if (MergeList[I]->getReturnType()->isPointerTy()) {
        Call->replaceAllUsesWith(new IntToPtrInst(
            NewCall, MergeList[I]->getReturnType(), "", it(Call)));
      } else if (cast<IntegerType>(MergeList[I]->getReturnType())
                     ->getBitWidth() < RetBitLen) {
        Call->replaceAllUsesWith(new TruncInst(
            NewCall, MergeList[I]->getReturnType(), "", it(Call)));
      } else {
        Call->replaceAllUsesWith(NewCall);
      }
      Call->eraseFromParent();
    }
  }

  BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", NewFunction);
  BasicBlock *SwitchB =
      BasicBlock::Create(M.getContext(), "switch", NewFunction);
  BranchInst::Create(SwitchB, Entry);
  SwitchInst *SwitchI =
      SwitchInst::Create(&*NewFunction->arg_begin(), SwitchB, 0, SwitchB);

  for (size_t I = 0; I < MergeList.size(); I++) {
    BasicBlock *CallFunc =
        BasicBlock::Create(M.getContext(), "", NewFunction, SwitchB);
    auto ItI32 = NewFunction->arg_begin();
    std::advance(ItI32, 1);
    auto ItI64 = NewFunction->arg_begin();
    std::advance(ItI64, 1 + NI32);
    auto ItOther = NewFunction->arg_begin();
    std::advance(ItOther, 1 + NI32 + NI64);
    for (Function *F : MergeList) {
      if (F == MergeList[I])
        break;
      for (Type *Ty : F->getFunctionType()->params()) {
        if (auto *TI = dyn_cast<IntegerType>(Ty))
          if (TI->getBitWidth() == 32 || TI->getBitWidth() == 64)
            continue;
        if (isa<PointerType>(Ty))
          continue;
        ++ItOther;
      }
    }
    std::vector<Value *> CallArgs;
    for (Argument &Arg : MergeList[I]->args()) {
      Type *Ty = Arg.getType();
      if (auto *TI = dyn_cast<IntegerType>(Ty)) {
        if (TI->getBitWidth() == 32)
          CallArgs.push_back(&*ItI32++);
        else if (TI->getBitWidth() == 64)
          CallArgs.push_back(&*ItI64++);
        else
          CallArgs.push_back(&*ItOther++);
      } else if (isa<PointerType>(Ty)) {
        CallArgs.push_back(new IntToPtrInst(&*ItI64++, Ty, "", CallFunc));
      } else {
        CallArgs.push_back(&*ItOther++);
      }
    }
    CallInst *CallI = CallInst::Create(MergeList[I], CallArgs, "", CallFunc);
    if (MergeList[I]->getReturnType()->isVoidTy())
      ReturnInst::Create(M.getContext(), ConstantInt::get(RetTy, 0), CallFunc);
    else if (MergeList[I]->getReturnType()->isPointerTy())
      ReturnInst::Create(M.getContext(),
                         new PtrToIntInst(CallI, RetTy, "", CallFunc),
                         CallFunc);
    else if (cast<IntegerType>(MergeList[I]->getReturnType())->getBitWidth() <
             RetBitLen)
      ReturnInst::Create(M.getContext(),
                         new ZExtInst(CallI, RetTy, "", CallFunc), CallFunc);
    else
      ReturnInst::Create(M.getContext(), CallI, CallFunc);
    SwitchI->addCase(ConstantInt::get(I32, FuncID[I]), CallFunc);
    InlineFunctionInfo IFI;
    InlineFunction(*CallI, IFI);
  }

  for (Function *F : MergeList)
    if (F->isDefTriviallyDead())
      F->eraseFromParent();

  return PreservedAnalyses::none();
}
