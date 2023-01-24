#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Transforms/Utils/Local.h"

#include "Util.h"

#include <random>
#include <string>

using namespace llvm;
bool valueEscapes(Instruction *Inst) {
  BasicBlock *BB = Inst->getParent();
  for (Value::use_iterator UI = Inst->use_begin(), E = Inst->use_end(); UI != E;
       ++UI) {
    Instruction *I = cast<Instruction>(*UI);
    if (I->getParent() != BB || isa<PHINode>(I)) {
      return true;
    }
  }
  return false;
}

void fixStack(Function *f) {
  // Try to remove phi node and demote reg to stack
  std::vector<PHINode *> tmpPhi;
  std::vector<Instruction *> tmpReg;
  BasicBlock *bbEntry = &*f->begin();

  do {
    tmpPhi.clear();
    tmpReg.clear();

    for (Function::iterator i = f->begin(); i != f->end(); ++i) {

      for (BasicBlock::iterator j = i->begin(); j != i->end(); ++j) {

        if (isa<PHINode>(j)) {
          PHINode *phi = cast<PHINode>(j);
          tmpPhi.push_back(phi);
          continue;
        }
        if (!(isa<AllocaInst>(j) && j->getParent() == bbEntry) &&
            (valueEscapes(&*j) || j->isUsedOutsideOfBlock(&*i))) {
          tmpReg.push_back(&*j);
          continue;
        }
      }
    }
    for (unsigned int i = 0; i != tmpReg.size(); ++i) {
      DemoteRegToStack(*tmpReg.at(i), f->begin()->getTerminator());
    }

    for (unsigned int i = 0; i != tmpPhi.size(); ++i) {
      DemotePHIToStack(tmpPhi.at(i), f->begin()->getTerminator());
    }

  } while (tmpReg.size() != 0 || tmpPhi.size() != 0);
}

InlineAsm *generateGarbage(Function *f) {
  bool is64 =
      Triple(f->getParent()->getTargetTriple()).getArch() == Triple::x86_64;
  std::random_device rd;
  std::mt19937 g(rd());
  std::uniform_int_distribution<uint32_t> rand(0, UINT32_MAX);
  std::string s = "";
  std::string junk[] = {"leaq	-4(%rbp), %rdx\n", "xorq %r11, %rsp\n",
                        "callq *(%rbx)\n", "popfq\n", "movq %rbp, %rsp\n"};
  if (is64) {
    for (int i = 0; i < 10; i++) {
      uint32_t j = rand(g) % 15;
      if (j < 5) {
        s += junk[j];
      }
    }
    if (rand(g) % 5 == 0) {
      s += "addq " + std::to_string(4 * (rand(g) % 20)) + ", %rsp\n";
      s += "popq %rbp\nretq\n";
    }
  }
  uint8_t onebyte[] = {0xEB, 0xE9, 0xE8, 0xE3, 0xE2, 0xE1, 0xE0, 0x70, 0x71,
                       0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
                       0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0xC2, 0xCA, 0xFF, 0x0F};
  s += ".byte " + std::to_string(onebyte[rand(g) % (sizeof(onebyte))]);
  InlineAsm *IA =
      InlineAsm::get(FunctionType::get(Type::getVoidTy(f->getContext()), false),
                     s, "", true, false);
  return IA;
}

uint32_t fnvHash(const uint32_t data, uint32_t b) {
  b = (b ^ ((data >> 0) & 0xFF)) * fnvPrime;
  b = (b ^ ((data >> 8) & 0xFF)) * fnvPrime;
  b = (b ^ ((data >> 16) & 0xFF)) * fnvPrime;
  b = (b ^ ((data >> 24) & 0xFF)) * fnvPrime;
  return b;
}

uint64_t powerMod(uint32_t a, uint32_t n, uint32_t mod) {
  uint64_t power = a, result = 1;

  while (n) {
    if (n & 1)
      result = (result * power) % mod;
    power = (power * power) % mod;
    n >>= 1;
  }
  return result;
}

bool witness(uint32_t a, uint32_t n) {
  uint32_t t, u, i;
  uint64_t prev, curr;

  u = n / 2;
  t = 1;
  while (!(u & 1)) {
    u /= 2;
    ++t;
  }

  prev = powerMod(a, u, n);
  for (i = 1; i <= t; ++i) {
    curr = (prev * prev) % n;
    if ((curr == 1) && (prev != 1) && (prev != n - 1))
      return true;
    prev = curr;
  }
  if (curr != 1)
    return true;
  return false;
}

bool isPrime(uint32_t number) {
  if (((!(number & 1)) && number != 2) || (number < 2) ||
      (number % 3 == 0 && number != 3))
    return (false);

  if (number < 1373653) {
    for (uint32_t k = 1; 36 * k * k - 12 * k < number; ++k)
      if ((number % (6 * k + 1) == 0) || (number % (6 * k - 1) == 0))
        return (false);
    return true;
  }

  if (number < 9080191) {
    if (witness(31, number))
      return false;
    if (witness(73, number))
      return false;
    return true;
  }

  if (witness(2, number))
    return false;
  if (witness(7, number))
    return false;
  if (witness(61, number))
    return false;
  return true;
}

uint32_t randPrime(uint32_t min, uint32_t max) {
  std::random_device rd;
  std::mt19937 g(rd());
  std::uniform_int_distribution<uint32_t> rand(min, max);
  uint32_t p = rand(g);
  while (!isPrime(p)) {
    p = rand(g);
  }
  return p;
}

uint64_t modinv(uint64_t a) {
  uint64_t x = a;
  for (int k = 2; k < 64; k *= 2) {
    x = (x * (2 - a * x)) % (1ULL << k);
  }
  return x * (2 - a * x);
}