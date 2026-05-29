#include "YANSOllvmCommon.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Local.h"

#include <random>
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

void llvm::yansollvm_fix_stack(Function *F) {
  std::vector<PHINode *> TmpPhi;
  std::vector<Instruction *> TmpReg;
  BasicBlock *Entry = &F->getEntryBlock();
  do {
    TmpPhi.clear();
    TmpReg.clear();
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (auto *Phi = dyn_cast<PHINode>(&I)) {
          TmpPhi.push_back(Phi);
          continue;
        }
        if (!(isa<AllocaInst>(&I) && I.getParent() == Entry) &&
            (valueEscapes(&I) || I.isUsedOutsideOfBlock(&BB))) {
          TmpReg.push_back(&I);
          continue;
        }
      }
    }
    BasicBlock::iterator AllocaPoint = Entry->begin();
    while (AllocaPoint != Entry->end() && isa<AllocaInst>(AllocaPoint))
      ++AllocaPoint;
    for (Instruction *I : TmpReg)
      DemoteRegToStack(*I, false, AllocaPoint);
    for (PHINode *Phi : TmpPhi)
      DemotePHIToStack(Phi, AllocaPoint);
  } while (!TmpReg.empty() || !TmpPhi.empty());
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
