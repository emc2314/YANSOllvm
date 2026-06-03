#include "ObfConPass.h"
#include "YANSOllvmCommon.h"
#include "Utils.h"

#include "YANSOllvmSeed.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

using namespace llvm;

namespace {
class ObfConImpl {
  std::vector<Value *> IntegerVect;
  std::unordered_set<Value *> OriginalInst;
  YansoRNG *RNG;

  bool isValidCandidateInstruction(Instruction &Inst) const {
    return !isa<GetElementPtrInst>(&Inst) && !isa<ReturnInst>(&Inst);
  }

  ConstantInt *isSplitCandidateOperand(Value *V) const {
    auto *C = dyn_cast<ConstantInt>(V);
    if (!C)
      return nullptr;
    uint64_t V64 = C->getValue().getLimitedValue();
    return (V64 && V64 != UINT64_MAX) ? C : nullptr;
  }

  ConstantInt *isObfCandidateOperand(Value *V) const {
    auto *C = dyn_cast<ConstantInt>(V);
    return (C && C->isZero()) ? C : nullptr;
  }

  void registerInteger(Value &V, bool Original = false) {
    if (V.getType()->isIntegerTy() && !isa<ConstantInt>(&V)) {
      if (Original)
        OriginalInst.insert(&V);
      else
        IntegerVect.push_back(&V);
    }
  }

  Value *splitConst(Instruction &Inst, ConstantInt *VReplace) {
    IntegerType *ReplacedType = cast<IntegerType>(VReplace->getType());
    IntegerType *I64 = IntegerType::get(Inst.getParent()->getContext(), 64);
    IRBuilder<> B(&Inst);
    Value *Replaced = B.CreateIntCast(VReplace, I64, true);
    uint64_t V = VReplace->getValue().getLimitedValue();
    switch (RNG->range(3)) {
    case 0: {
      uint64_t RandV = (RNG->next64() | 1ULL);
      BinaryOperator *RV1 = BinaryOperator::Create(
          (RNG->range(2) ? BinaryOperator::Add : BinaryOperator::Xor),
          ConstantInt::get(I64, RandV), ConstantInt::get(I64, 0), "",
          it(Inst));
      BinaryOperator *RV2 = BinaryOperator::Create(
          (RNG->range(2) ? BinaryOperator::Add : BinaryOperator::Xor),
          ConstantInt::get(I64, yansollvm_mod_inv(RandV) * V),
          ConstantInt::get(I64, 0), "", it(Inst));
      Replaced = B.CreateMul(RV1, RV2);
      break;
    }
    case 1: {
      uint64_t RandV = RNG->next64();
      BinaryOperator *RV1 = BinaryOperator::Create(
          (RNG->range(2) ? BinaryOperator::Add : BinaryOperator::Xor),
          ConstantInt::get(I64, RandV), ConstantInt::get(I64, 0), "",
          it(Inst));
      BinaryOperator *RV2 = BinaryOperator::Create(
          (RNG->range(2) ? BinaryOperator::Add : BinaryOperator::Xor),
          ConstantInt::get(I64, RandV ^ V), ConstantInt::get(I64, 0), "",
          it(Inst));
      Replaced = B.CreateXor(RV1, RV2);
      break;
    }
    default: {
      uint64_t RandV = RNG->next64();
      BinaryOperator *RV1 = BinaryOperator::Create(
          (RNG->range(2) ? BinaryOperator::Add : BinaryOperator::Xor),
          ConstantInt::get(I64, RandV), ConstantInt::get(I64, 0), "",
          it(Inst));
      BinaryOperator *RV2 = BinaryOperator::Create(
          (RNG->range(2) ? BinaryOperator::Add : BinaryOperator::Xor),
          ConstantInt::get(I64, V - RandV), ConstantInt::get(I64, 0), "",
          it(Inst));
      Replaced = B.CreateAdd(RV1, RV2);
    }
    }
    return B.CreateIntCast(Replaced, ReplacedType, true);
  }

  Value *createExpression(Value *X, const uint32_t P, IRBuilder<> &B) {
    Type *Ty = X->getType();
    Constant *Any = ConstantInt::get(Ty, (1 + RNG->range(255)));
    Constant *Prime = ConstantInt::get(Ty, P);
    Constant *OverflowMask = ConstantInt::get(Ty, 0xFF);
    Value *Temp = B.CreateOr(X, Any);
    Temp = B.CreateAnd(OverflowMask, Temp);
    Temp = B.CreateMul(Temp, Temp);
    Temp = B.CreateMul(Prime, Temp);
    registerInteger(*Temp);
    return Temp;
  }

  Value *replaceZero(Instruction &Inst, ConstantInt *VReplace) {
    IntegerType *ReplacedType = cast<IntegerType>(VReplace->getType());
    IntegerType *I32 = IntegerType::get(Inst.getParent()->getContext(), 32);
    Value *Replaced = nullptr;
    if (IntegerVect.empty())
      return nullptr;
    IRBuilder<> B(&Inst);
    size_t IX = RNG->range(IntegerVect.size());
    Value *X = B.CreateIntCast(IntegerVect[IX], I32, false);
    if (IntegerVect.size() == 1) {
      Value *Temp = B.CreateNot(X);
      Temp = B.CreateOr(Temp, ConstantInt::get(I32, 0x7AFAFA69));
      Temp = B.CreateAnd(Temp, ConstantInt::get(I32, 0xA061440));
      Replaced = B.CreateAnd(X, ConstantInt::get(I32, 0x1050504));
      Replaced = B.CreateOr(Replaced, ConstantInt::get(I32, 0x1010104));
      Replaced = B.CreateAdd(Replaced, Temp);
      Replaced = B.CreateXor(Replaced, ConstantInt::get(I32, 185013572));
      Replaced = B.CreateIntCast(Replaced, ReplacedType, false);
    } else {
      size_t IY = RNG->range(IntegerVect.size());
      while (IX == IY)
        IY = RNG->range(IntegerVect.size());
      Value *Y = B.CreateIntCast(IntegerVect[IY], I32, false);
      Value *Temp = nullptr;
      switch (RNG->range(3)) {
      case 0: {
        uint32_t P1 = yansollvm_rand_prime(1 << 8, 1 << 16, *RNG);
        uint32_t P2 = yansollvm_rand_prime(1 << 8, 1 << 16, *RNG);
        while (P1 == P2)
          P2 = yansollvm_rand_prime(1 << 8, 1 << 16, *RNG);
        Value *L = createExpression(X, P1, B);
        Value *R = createExpression(Y, P2, B);
        Replaced =
            B.CreateSExt(B.CreateICmp(CmpInst::ICMP_EQ, L, R), ReplacedType);
        break;
      }
      case 1:
        Replaced = B.CreateAdd(X, Y);
        Temp = B.CreateXor(X, Y);
        Replaced = B.CreateSub(Replaced, Temp);
        Temp = B.CreateAnd(X, Y);
        Temp = B.CreateShl(Temp, ConstantInt::get(I32, 1));
        Replaced = B.CreateXor(Replaced, Temp);
        Replaced = B.CreateIntCast(Replaced, ReplacedType, false);
        break;
      case 2: {
        Value *A = B.CreateNot(Y);
        A = B.CreateOr(X, A);
        Value *C = B.CreateOr(X, Y);
        C = B.CreateNot(C);
        C = B.CreateMul(C, ConstantInt::get(I32, -3));
        Value *D = B.CreateNot(X);
        D = B.CreateMul(D, ConstantInt::get(I32, 2));
        D = B.CreateSub(D, Y);
        Replaced = B.CreateXor(X, Y);
        Replaced = B.CreateSub(Replaced, A);
        Replaced = B.CreateSub(Replaced, C);
        Replaced = B.CreateXor(Replaced, D);
        Replaced = B.CreateIntCast(Replaced, ReplacedType, false);
        break;
      }
      }
    }
    if (Replaced)
      registerInteger(*Replaced);
    return Replaced;
  }

public:
  bool run(Function &F) {
    YansoRNG LocalRNG(yanso_function_seed(F, "obfcon"));
    RNG = &LocalRNG;
    bool Modified = false;
    OriginalInst.clear();
    for (BasicBlock &BB : F)
      for (auto I = BB.getFirstInsertionPt(), E = BB.end(); I != E; ++I)
        registerInteger(*I, true);

    for (BasicBlock &BB : F) {
      for (auto I = BB.getFirstInsertionPt(), E = BB.end(); I != E; ++I) {
        Instruction &Inst = *I;
        if (!isValidCandidateInstruction(Inst))
          continue;
        size_t OpSize = Inst.getNumOperands();
        if (isa<SwitchInst>(&Inst))
          OpSize = 1;
        for (size_t Op = 0; Op < OpSize; ++Op) {
          if (ConstantInt *C = isSplitCandidateOperand(Inst.getOperand(Op))) {
            if (auto *CI = dyn_cast<CallInst>(&Inst))
              if (CI->paramHasAttr(Op, Attribute::ImmArg))
                break;
            if (Value *NewVal = splitConst(Inst, C)) {
              Inst.setOperand(Op, NewVal);
              Modified = true;
            }
          }
        }
      }

      IntegerVect.clear();
      BasicBlock *PBB = BB.getSinglePredecessor();
      while (PBB) {
        for (auto I = PBB->getFirstInsertionPt(), E = PBB->end(); I != E; ++I)
          if (OriginalInst.count(&*I))
            registerInteger(*I);
        PBB = PBB->getSinglePredecessor();
      }
      for (Argument &Arg : F.args())
        registerInteger(Arg);
      for (auto I = BB.getFirstInsertionPt(), E = BB.end(); I != E; ++I) {
        Instruction &Inst = *I;
        if (isValidCandidateInstruction(Inst)) {
          size_t OpSize = Inst.getNumOperands();
          if (isa<SwitchInst>(&Inst))
            OpSize = 1;
          if (isa<CallInst>(&Inst))
            OpSize = 0;
          for (size_t Op = 0; Op < OpSize; ++Op) {
            if (ConstantInt *C = isObfCandidateOperand(Inst.getOperand(Op))) {
              if (Value *NewVal = replaceZero(Inst, C)) {
                Inst.setOperand(Op, NewVal);
                Modified = true;
              }
            }
          }
        }
        if (OriginalInst.count(&Inst))
          registerInteger(Inst);
      }
    }
    return Modified;
  }
};
} // namespace

PreservedAnalyses ObfConPass::run(Function &F, FunctionAnalysisManager &) {
  if (!Enabled)
    return PreservedAnalyses::all();
  ObfConImpl Impl;
  return Impl.run(F) ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
