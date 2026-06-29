#pragma once

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace llvm {
class APInt;
class BasicBlock;
class Module;
class Value;

inline constexpr uint64_t YansoMixBasis = 0x1145141919810ULL;
inline constexpr uint64_t YansoMixAddend = 0x19260817ULL;
inline constexpr unsigned YansoMixRotate = 13;

uint64_t yanso_mix64(uint64_t A, uint64_t B);
uint64_t yanso_hash_string(StringRef S, uint64_t Seed = YansoMixBasis);
uint64_t yanso_mod_inverse(uint64_t A);
uint64_t yanso_mod_inverse(uint64_t A, uint64_t Modulus);
APInt yanso_mod_inverse(const APInt &A);
APInt yanso_mod_inverse(const APInt &A, const APInt &Modulus);
Value *yanso_create_mix64_ir(Value *A, Value *B, BasicBlock *InsertAtEnd,
                             Module &M);

class YansoRNG {
public:
  explicit YansoRNG(uint64_t Seed) : Engine(Seed) {}

  uint64_t next64() { return Engine(); }
  uint32_t next32() { return static_cast<uint32_t>(next64() >> 32); }
  uint32_t range(uint32_t Max);
  void fill_bytes(char *Buffer, size_t Len);

  template <typename T> void shuffle(std::vector<T> &Values) {
    for (size_t I = Values.size(); I > 1; --I)
      std::swap(Values[I - 1], Values[range(static_cast<uint32_t>(I))]);
  }

  template <typename T> void shuffle(SmallVectorImpl<T> &Values) {
    for (size_t I = Values.size(); I > 1; --I)
      std::swap(Values[I - 1], Values[range(static_cast<uint32_t>(I))]);
  }

private:
  std::mt19937_64 Engine;
};

} // namespace llvm
