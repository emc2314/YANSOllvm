#include "CryptoUtils.h"

#include "YANSOllvmSeed.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace llvm {

static cl::opt<std::string>
    YansoSeed("yanso-seed", cl::init("YANSOllvm"),
              cl::desc("Deterministic seed for yansollvm random choices"));

static uint64_t hashType(Type *Ty, uint64_t Seed) {
  std::string Text;
  raw_string_ostream OS(Text);
  Ty->print(OS);
  OS.flush();
  return yanso_hash_string(Text, Seed);
}

static uint64_t moduleStructuralHash(const Module &M, uint64_t Seed) {
  uint64_t H = yanso_hash_string(M.getTargetTriple().str(), Seed);
  H = yanso_hash_string(M.getDataLayoutStr(), H);
  for (const GlobalVariable &GV : M.globals()) {
    H = yanso_hash_string(GV.getName(), H);
    H = hashType(GV.getValueType(), H);
  }
  for (const Function &F : M) {
    H = yanso_hash_string(F.getName(), H);
    H = hashType(F.getFunctionType(), H);
    H = yanso_mix64(F.size() + 1, H);
    for (const BasicBlock &BB : F) {
      H = yanso_mix64(BB.size() + 1, H);
      for (const Instruction &I : BB) {
        H = yanso_mix64(I.getOpcode() + 1, H);
        H = yanso_mix64(I.getNumOperands() + 1, H);
        H = yanso_mix64(I.getType()->getTypeID() + 1, H);
      }
      if (const Instruction *T = BB.getTerminator())
        H = yanso_mix64(T->getNumSuccessors() + 1, H);
    }
  }
  return H;
}

uint64_t yanso_module_seed(const Module &M, StringRef PassName) {
  uint64_t H = yanso_hash_string(YansoSeed, YansoMixBasis);
  H = yanso_hash_string(PassName, H);
  return moduleStructuralHash(M, H);
}

uint64_t yanso_function_seed(const Function &F, StringRef PassName) {
  uint64_t H = yanso_module_seed(*F.getParent(), PassName);
  unsigned FunctionIndex = 0;
  for (const Function &Cur : *F.getParent()) {
    if (&Cur == &F)
      break;
    ++FunctionIndex;
  }
  H = yanso_mix64(FunctionIndex + 1, H);
  H = yanso_hash_string(F.getName(), H);
  H = hashType(F.getFunctionType(), H);
  H = yanso_mix64(F.size() + 1, H);
  for (const BasicBlock &BB : F) {
    H = yanso_mix64(BB.size() + 1, H);
    for (const Instruction &I : BB)
      H = yanso_mix64(I.getOpcode() + 1, H);
  }
  return H;
}

uint64_t yanso_basic_block_seed(const BasicBlock &BB, StringRef PassName) {
  const Function *F = BB.getParent();
  uint64_t H = yanso_function_seed(*F, PassName);
  unsigned BlockIndex = 0;
  for (const BasicBlock &Cur : *F) {
    if (&Cur == &BB)
      break;
    ++BlockIndex;
  }
  H = yanso_mix64(BlockIndex + 1, H);
  H = yanso_mix64(BB.size() + 1, H);
  for (const Instruction &I : BB)
    H = yanso_mix64(I.getOpcode() + 1, H);
  return H;
}

} // namespace llvm
