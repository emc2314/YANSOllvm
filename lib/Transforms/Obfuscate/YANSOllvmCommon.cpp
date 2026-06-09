#include "YANSOllvmCommon.h"
#include "Utils.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Local.h"

#include <random>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace {
bool valueEscapes(Instruction *Inst) {
  BasicBlock *BB = Inst->getParent();
  for (Use &U : Inst->uses()) {
    Instruction *I = cast<Instruction>(U.getUser());
    if (I->getParent() != BB || isa<PHINode>(I))
      return true;
  }
  return false;
}

bool shouldDemoteReg(Instruction *I, BasicBlock *Entry,
                     const std::set<Instruction *> *SkipRegs) {
  if (SkipRegs && SkipRegs->count(I))
    return false;
  if (isa<AllocaInst>(I) && I->getParent() == Entry)
    return false;
  return valueEscapes(I) || I->isUsedOutsideOfBlock(I->getParent());
}

uint64_t powerMod(uint32_t A, uint32_t N, uint32_t Mod) {
  uint64_t Power = A, Result = 1;
  while (N) {
    if (N & 1)
      Result = (Result * Power) % Mod;
    Power = (Power * Power) % Mod;
    N >>= 1;
  }
  return Result;
}

bool witness(uint32_t A, uint32_t N) {
  uint32_t T, U, I;
  uint64_t Prev, Curr = 0;
  U = N / 2;
  T = 1;
  while (!(U & 1)) {
    U /= 2;
    ++T;
  }
  Prev = powerMod(A, U, N);
  for (I = 1; I <= T; ++I) {
    Curr = (Prev * Prev) % N;
    if ((Curr == 1) && (Prev != 1) && (Prev != N - 1))
      return true;
    Prev = Curr;
  }
  return Curr != 1;
}

bool isPrime(uint32_t Number) {
  if (((!(Number & 1)) && Number != 2) || (Number < 2) ||
      (Number % 3 == 0 && Number != 3))
    return false;
  if (Number < 1373653) {
    for (uint32_t K = 1; 36 * K * K - 12 * K < Number; ++K)
      if ((Number % (6 * K + 1) == 0) || (Number % (6 * K - 1) == 0))
        return false;
    return true;
  }
  if (Number < 9080191) {
    if (witness(31, Number))
      return false;
    if (witness(73, Number))
      return false;
    return true;
  }
  if (witness(2, Number))
    return false;
  if (witness(7, Number))
    return false;
  if (witness(61, Number))
    return false;
  return true;
}
} // namespace

void llvm::yansollvm_fix_stack(Function *F,
                               const std::set<BasicBlock *> *SkipPhiBlocks,
                               const std::set<Instruction *> *SkipRegs) {
  std::vector<PHINode *> TmpPhi;
  std::vector<Instruction *> TmpReg;
  BasicBlock *Entry = &F->getEntryBlock();

  std::set<Instruction *> Demoted;
  do {
    TmpPhi.clear();
    TmpReg.clear();
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (auto *Phi = dyn_cast<PHINode>(&I)) {
          if (!SkipPhiBlocks || !SkipPhiBlocks->count(&BB))
            TmpPhi.push_back(Phi);
          continue;
        }
        if (Demoted.count(&I))
          continue;
        if (shouldDemoteReg(&I, Entry, SkipRegs)) {
          TmpReg.push_back(&I);
          continue;
        }
      }
    }
    BasicBlock::iterator AllocaPoint = firstNonAlloca(*Entry);
    for (Instruction *Reg : TmpReg) {
      Demoted.insert(Reg);
      DemoteRegToStack(*Reg, false, AllocaPoint);
    }
    for (PHINode *Phi : TmpPhi)
      DemotePHIToStack(Phi, AllocaPoint);
  } while (!TmpReg.empty() || !TmpPhi.empty());
}

bool llvm::yansollvm_has_dynamic_stack_state(BasicBlock &BB) {
  for (Instruction &I : BB) {
    if (auto *AI = dyn_cast<AllocaInst>(&I)) {
      if (AI->isArrayAllocation())
        return true;
    } else if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
      if (II->getIntrinsicID() == Intrinsic::stacksave ||
          II->getIntrinsicID() == Intrinsic::stackrestore)
        return true;
    }
  }
  return false;
}

bool llvm::yansollvm_has_dynamic_stack_state(Function &F) {
  for (BasicBlock &BB : F)
    if (yansollvm_has_dynamic_stack_state(BB))
      return true;
  return false;
}

void llvm::yansollvm_create_trap_block(Function *F, BasicBlock *BB) {
  IRBuilder<> B(BB);
  // Architecture-neutral placeholder for the old x86 inline-asm garbage.
  // Keep the CFG-obfuscation shape (a switch default to an unreachable trap)
  // without embedding target-specific machine code. A future garbage generator
  // should be a separate target-aware component plugged in here.
  FunctionCallee Trap =
      Intrinsic::getOrInsertDeclaration(F->getParent(), Intrinsic::trap);
  B.CreateCall(Trap);
  B.CreateUnreachable();
}

uint32_t llvm::yansollvm_rand_prime(uint32_t Min, uint32_t Max, YansoRNG &RNG) {
  uint32_t P = Min + RNG.range(Max - Min + 1);
  while (!isPrime(P))
    P = Min + RNG.range(Max - Min + 1);
  return P;
}

uint64_t llvm::yansollvm_mod_inv(uint64_t A) {
  uint64_t X = A;
  for (int K = 2; K < 64; K *= 2)
    X = (X * (2 - A * X)) % (1ULL << K);
  return X * (2 - A * X);
}
