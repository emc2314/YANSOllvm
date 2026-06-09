#include "VMPass.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

#include <string>
#include <vector>

using namespace llvm;

namespace {
class VirtualizeImpl {
  StringMap<Function *> Cache;

  static constexpr StringRef Prefix = "__yansollvm_vm_";

  static void attrs(Function *F) {
    F->addFnAttr(Attribute::NoInline);
    F->addFnAttr(Attribute::OptimizeNone);
  }

  static bool isSupportedInt(Type *Ty) { return isa<IntegerType>(Ty); }

  static bool isSupportedPointer(Type *Ty) { return isa<PointerType>(Ty); }

  static bool isSupportedScalar(Type *Ty) {
    return isSupportedInt(Ty) || isSupportedPointer(Ty);
  }

  static StringRef binaryName(unsigned Opcode) {
    switch (Opcode) {
    case BinaryOperator::Add:
      return "Add";
    case BinaryOperator::Sub:
      return "Sub";
    case BinaryOperator::Mul:
      return "Mul";
    case BinaryOperator::UDiv:
      return "UDiv";
    case BinaryOperator::SDiv:
      return "SDiv";
    case BinaryOperator::URem:
      return "URem";
    case BinaryOperator::SRem:
      return "SRem";
    case BinaryOperator::Shl:
      return "Shl";
    case BinaryOperator::AShr:
      return "AShr";
    case BinaryOperator::LShr:
      return "LShr";
    case BinaryOperator::And:
      return "And";
    case BinaryOperator::Or:
      return "Or";
    case BinaryOperator::Xor:
      return "Xor";
    default:
      return "";
    }
  }

  static StringRef predicateName(CmpInst::Predicate Pred) {
    switch (Pred) {
    case CmpInst::ICMP_EQ:
      return "ICmpEQ";
    case CmpInst::ICMP_NE:
      return "ICmpNE";
    case CmpInst::ICMP_UGT:
      return "ICmpUGT";
    case CmpInst::ICMP_UGE:
      return "ICmpUGE";
    case CmpInst::ICMP_ULT:
      return "ICmpULT";
    case CmpInst::ICMP_ULE:
      return "ICmpULE";
    case CmpInst::ICMP_SGT:
      return "ICmpSGT";
    case CmpInst::ICMP_SGE:
      return "ICmpSGE";
    case CmpInst::ICMP_SLT:
      return "ICmpSLT";
    case CmpInst::ICMP_SLE:
      return "ICmpSLE";
    default:
      return "";
    }
  }

  static StringRef castName(unsigned Opcode) {
    switch (Opcode) {
    case Instruction::Trunc:
      return "Trunc";
    case Instruction::ZExt:
      return "ZExt";
    case Instruction::SExt:
      return "SExt";
    case Instruction::PtrToInt:
      return "PtrToInt";
    case Instruction::IntToPtr:
      return "IntToPtr";
    default:
      return "";
    }
  }

  static StringRef intrinsicName(Intrinsic::ID ID) {
    switch (ID) {
    case Intrinsic::fshl:
      return "FShl";
    case Intrinsic::fshr:
      return "FShr";
    case Intrinsic::bswap:
      return "BSwap";
    case Intrinsic::bitreverse:
      return "BitReverse";
    case Intrinsic::ctpop:
      return "CtPop";
    case Intrinsic::ctlz:
      return "Ctlz";
    case Intrinsic::cttz:
      return "Cttz";
    case Intrinsic::abs:
      return "Abs";
    case Intrinsic::smin:
      return "SMin";
    case Intrinsic::smax:
      return "SMax";
    case Intrinsic::umin:
      return "UMin";
    case Intrinsic::umax:
      return "UMax";
    default:
      return "";
    }
  }

  static std::string typeSuffix(Type *Ty) {
    if (auto *ITy = dyn_cast<IntegerType>(Ty))
      return (Twine("i") + Twine(ITy->getBitWidth())).str();
    if (auto *PTy = dyn_cast<PointerType>(Ty))
      return (Twine("p") + Twine(PTy->getAddressSpace())).str();
    llvm_unreachable("unsupported VM handler type");
  }

  static std::string typedName(StringRef Base, Type *Ty,
                               StringRef Suffix = "") {
    return (Twine(Prefix) + Base + "_" + typeSuffix(Ty) + Suffix).str();
  }

  static std::string castHandlerName(StringRef Base, Type *SrcTy, Type *DstTy) {
    return (Twine(Prefix) + Base + "_" + typeSuffix(SrcTy) + "_" +
            typeSuffix(DstTy))
        .str();
  }

  Function *createBinaryHandler(Module &M, unsigned Opcode, IntegerType *Ty) {
    StringRef Name = binaryName(Opcode);
    if (Name.empty())
      return nullptr;

    std::string FullName = typedName(Name, Ty);
    Function *&F = Cache[FullName];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(Ty, {Ty, Ty}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);

    switch (Opcode) {
    case BinaryOperator::Add:
      emitAdd(B, X, Y);
      break;
    case BinaryOperator::Sub:
      emitSub(B, Ty, X, Y);
      break;
    case BinaryOperator::And:
      emitAnd(B, X, Y);
      break;
    case BinaryOperator::Or:
      emitOr(B, X, Y);
      break;
    case BinaryOperator::Xor:
      emitXor(B, Ty, X, Y);
      break;
    default:
      B.CreateRet(B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opcode), X,
                                Y));
      break;
    }

    attrs(F);
    return F;
  }

  static void emitAdd(IRBuilder<> &B, Value *X, Value *Y) {
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
  }

  static void emitSub(IRBuilder<> &B, IntegerType *Ty, Value *X, Value *Y) {
    Value *R = B.CreateAdd(X, B.CreateNot(Y));
    R = B.CreateAdd(R, ConstantInt::get(Ty, 1));
    B.CreateRet(R);
  }

  static void emitAnd(IRBuilder<> &B, Value *X, Value *Y) {
    Value *A = B.CreateAnd(X, Y);
    A = B.CreateNot(A);
    Value *C = B.CreateNot(X);
    C = B.CreateOr(C, Y);
    Value *D = B.CreateNot(Y);
    D = B.CreateAnd(X, D);
    Value *R = B.CreateAdd(C, D);
    R = B.CreateSub(R, A);
    B.CreateRet(R);
  }

  static void emitOr(IRBuilder<> &B, Value *X, Value *Y) {
    Value *A = B.CreateXor(X, Y);
    Value *C = B.CreateNot(X);
    C = B.CreateAnd(C, Y);
    Value *R = B.CreateAdd(A, Y);
    R = B.CreateSub(R, C);
    B.CreateRet(R);
  }

  static void emitXor(IRBuilder<> &B, IntegerType *Ty, Value *X, Value *Y) {
    Value *A = B.CreateAdd(X, Y);
    Value *C = B.CreateAnd(X, Y);
    Value *R = B.CreateShl(C, ConstantInt::get(Ty, 1));
    R = B.CreateSub(A, R);
    B.CreateRet(R);
  }

  Function *createICmpHandler(Module &M, CmpInst::Predicate Pred, Type *Ty) {
    StringRef Name = predicateName(Pred);
    if (Name.empty())
      return nullptr;

    std::string FullName = typedName(Name, Ty);
    Function *&F = Cache[FullName];
    if (F)
      return F;

    IntegerType *I1 = Type::getInt1Ty(M.getContext());
    FunctionType *FuncTy = FunctionType::get(I1, {Ty, Ty}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    B.CreateRet(B.CreateICmp(Pred, X, Y));
    attrs(F);
    return F;
  }

  Function *createIntrinsicHandler(Module &M, Intrinsic::ID ID, IntegerType *Ty,
                                   ConstantInt *ImmArg = nullptr) {
    StringRef Name = intrinsicName(ID);
    if (Name.empty())
      return nullptr;

    std::string FullName = typedName(
        Name, Ty, ImmArg ? (ImmArg->isZero() ? "_0" : "_1") : "");
    Function *&F = Cache[FullName];
    if (F)
      return F;

    unsigned Arity = intrinsicDataArgCount(ID);
    if (!Arity)
      return nullptr;

    SmallVector<Type *, 4> Params(Arity, Ty);
    FunctionType *FuncTy = FunctionType::get(Ty, Params, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);

    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    SmallVector<Value *, 4> Args;
    for (Argument &Arg : F->args())
      Args.push_back(&Arg);
    if (ImmArg)
      Args.push_back(ConstantInt::get(Type::getInt1Ty(M.getContext()),
                                      !ImmArg->isZero()));

    FunctionCallee Intr = Intrinsic::getOrInsertDeclaration(&M, ID, {Ty});
    B.CreateRet(B.CreateCall(Intr, Args));
    attrs(F);
    return F;
  }

  static unsigned intrinsicDataArgCount(Intrinsic::ID ID) {
    switch (ID) {
    case Intrinsic::fshl:
    case Intrinsic::fshr:
      return 3;
    case Intrinsic::bswap:
    case Intrinsic::bitreverse:
    case Intrinsic::ctpop:
    case Intrinsic::ctlz:
    case Intrinsic::cttz:
    case Intrinsic::abs:
      return 1;
    case Intrinsic::smin:
    case Intrinsic::smax:
    case Intrinsic::umin:
    case Intrinsic::umax:
      return 2;
    default:
      return 0;
    }
  }

  Function *createCastHandler(Module &M, unsigned Opcode, Type *SrcTy,
                              Type *DstTy) {
    StringRef Name = castName(Opcode);
    if (Name.empty())
      return nullptr;

    std::string FullName = castHandlerName(Name, SrcTy, DstTy);
    Function *&F = Cache[FullName];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(DstTy, {SrcTy}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    Value *X = &*F->arg_begin();
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    B.CreateRet(B.CreateCast(static_cast<Instruction::CastOps>(Opcode), X,
                             DstTy));
    attrs(F);
    return F;
  }

  Function *createSelectHandler(Module &M, Type *Ty) {
    std::string FullName = typedName("Select", Ty);
    Function *&F = Cache[FullName];
    if (F)
      return F;

    Type *I1 = Type::getInt1Ty(M.getContext());
    FunctionType *FuncTy = FunctionType::get(Ty, {I1, Ty, Ty}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    auto It = F->arg_begin();
    Value *Cond = &*It++;
    Value *TrueV = &*It++;
    Value *FalseV = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);

    if (auto *ITy = dyn_cast<IntegerType>(Ty)) {
      Value *CondZ = B.CreateZExt(Cond, ITy);
      Value *Mask = B.CreateSub(ConstantInt::get(ITy, 0), CondZ);
      Value *TruePart = B.CreateAnd(TrueV, Mask);
      Value *FalsePart = B.CreateAnd(FalseV, B.CreateNot(Mask));
      B.CreateRet(B.CreateOr(TruePart, FalsePart));
    } else {
      B.CreateRet(B.CreateSelect(Cond, TrueV, FalseV));
    }
    attrs(F);
    return F;
  }

  static bool isSupportedIntrinsicCall(CallInst *CI) {
    auto *RetTy = dyn_cast<IntegerType>(CI->getType());
    if (!RetTy || intrinsicName(CI->getIntrinsicID()).empty())
      return false;

    switch (CI->getIntrinsicID()) {
    case Intrinsic::fshl:
    case Intrinsic::fshr:
      return CI->arg_size() == 3 && CI->getArgOperand(0)->getType() == RetTy &&
             CI->getArgOperand(1)->getType() == RetTy &&
             CI->getArgOperand(2)->getType() == RetTy;
    case Intrinsic::bswap:
    case Intrinsic::bitreverse:
    case Intrinsic::ctpop:
      return CI->arg_size() == 1 && CI->getArgOperand(0)->getType() == RetTy;
    case Intrinsic::ctlz:
    case Intrinsic::cttz:
    case Intrinsic::abs:
      return CI->arg_size() == 2 && CI->getArgOperand(0)->getType() == RetTy &&
             isa<ConstantInt>(CI->getArgOperand(1));
    case Intrinsic::smin:
    case Intrinsic::smax:
    case Intrinsic::umin:
    case Intrinsic::umax:
      return CI->arg_size() == 2 && CI->getArgOperand(0)->getType() == RetTy &&
             CI->getArgOperand(1)->getType() == RetTy;
    default:
      return false;
    }
  }

  static bool isSupportedCast(CastInst *CI) {
    if (castName(CI->getOpcode()).empty())
      return false;

    switch (CI->getOpcode()) {
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
      return isSupportedInt(CI->getSrcTy()) && isSupportedInt(CI->getDestTy());
    case Instruction::PtrToInt:
      return isSupportedPointer(CI->getSrcTy()) && isSupportedInt(CI->getDestTy());
    case Instruction::IntToPtr:
      return isSupportedInt(CI->getSrcTy()) && isSupportedPointer(CI->getDestTy());
    default:
      return false;
    }
  }

  static bool isSupportedSelect(SelectInst *SI) {
    return SI->getCondition()->getType()->isIntegerTy(1) &&
           isSupportedScalar(SI->getType()) &&
           SI->getTrueValue()->getType() == SI->getType() &&
           SI->getFalseValue()->getType() == SI->getType();
  }

public:
  bool run(Module &M) {
    bool Modified = false;
    std::vector<BinaryOperator *> BinOps;
    std::vector<ICmpInst *> ICmps;
    std::vector<CallInst *> Intrinsics;
    std::vector<CastInst *> Casts;
    std::vector<SelectInst *> Selects;

    for (Function &F : M) {
      for (Instruction &I : instructions(F)) {
        if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
          if (isSupportedInt(BO->getType()) && !binaryName(BO->getOpcode()).empty())
            BinOps.push_back(BO);
          continue;
        }
        if (auto *ICI = dyn_cast<ICmpInst>(&I)) {
          if (isSupportedScalar(ICI->getOperand(0)->getType()) &&
              ICI->getOperand(0)->getType() == ICI->getOperand(1)->getType() &&
              !predicateName(ICI->getPredicate()).empty())
            ICmps.push_back(ICI);
          continue;
        }
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (CI->getCalledFunction() &&
              CI->getIntrinsicID() != Intrinsic::not_intrinsic &&
              isSupportedIntrinsicCall(CI))
            Intrinsics.push_back(CI);
          continue;
        }
        if (auto *CI = dyn_cast<CastInst>(&I)) {
          if (isSupportedCast(CI))
            Casts.push_back(CI);
          continue;
        }
        if (auto *SI = dyn_cast<SelectInst>(&I)) {
          if (isSupportedSelect(SI))
            Selects.push_back(SI);
          continue;
        }
      }
    }

    for (BinaryOperator *BO : BinOps) {
      auto *Ty = cast<IntegerType>(BO->getType());
      Function *Func = createBinaryHandler(M, BO->getOpcode(), Ty);
      if (!Func)
        continue;
      IRBuilder<> B(BO);
      Value *R = B.CreateCall(Func, {BO->getOperand(0), BO->getOperand(1)});
      BO->replaceAllUsesWith(R);
      BO->eraseFromParent();
      Modified = true;
    }

    for (ICmpInst *ICI : ICmps) {
      Type *Ty = ICI->getOperand(0)->getType();
      Function *Func = createICmpHandler(M, ICI->getPredicate(), Ty);
      if (!Func)
        continue;
      IRBuilder<> B(ICI);
      Value *R = B.CreateCall(Func, {ICI->getOperand(0), ICI->getOperand(1)});
      ICI->replaceAllUsesWith(R);
      ICI->eraseFromParent();
      Modified = true;
    }

    for (CallInst *CI : Intrinsics) {
      auto *Ty = cast<IntegerType>(CI->getType());
      ConstantInt *ImmArg = nullptr;
      if (CI->arg_size() == 2 && CI->getArgOperand(1)->getType()->isIntegerTy(1))
        ImmArg = dyn_cast<ConstantInt>(CI->getArgOperand(1));
      Function *Func = createIntrinsicHandler(M, CI->getIntrinsicID(), Ty, ImmArg);
      if (!Func)
        continue;
      IRBuilder<> B(CI);
      SmallVector<Value *, 4> Args;
      for (unsigned I = 0, E = CI->arg_size(); I != E; ++I) {
        if (ImmArg && I == E - 1)
          continue;
        Args.push_back(CI->getArgOperand(I));
      }
      Value *R = B.CreateCall(Func, Args);
      CI->replaceAllUsesWith(R);
      CI->eraseFromParent();
      Modified = true;
    }

    for (CastInst *CI : Casts) {
      Type *SrcTy = CI->getSrcTy();
      Type *DstTy = CI->getDestTy();
      Function *Func = createCastHandler(M, CI->getOpcode(), SrcTy, DstTy);
      if (!Func)
        continue;
      IRBuilder<> B(CI);
      Value *R = B.CreateCall(Func, {CI->getOperand(0)});
      CI->replaceAllUsesWith(R);
      CI->eraseFromParent();
      Modified = true;
    }

    for (SelectInst *SI : Selects) {
      Type *Ty = SI->getType();
      Function *Func = createSelectHandler(M, Ty);
      if (!Func)
        continue;
      IRBuilder<> B(SI);
      Value *R = B.CreateCall(
          Func, {SI->getCondition(), SI->getTrueValue(), SI->getFalseValue()});
      SI->replaceAllUsesWith(R);
      SI->eraseFromParent();
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
