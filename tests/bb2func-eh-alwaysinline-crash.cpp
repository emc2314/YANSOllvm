// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang++ -std=gnu++14 -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=bb2func,verify -S %t/input.ll -o %t/bb2func.ll
// RUN: %clang++ %t/bb2func.ll -o %t/bb2func
// RUN: %t/bb2func | grep '^hello world$'

// Reduced from llvm-test-suite SingleSource/UnitTests/EH/simple-2.cpp.
// BB2FuncPass must not extract EH regions from alwaysinline/personality functions.
#include <iostream>
#include <string>

#define THROW(FUNC, TYPE, VAL)                                                 \
  void throw_##FUNC() __attribute__((always_inline));                          \
  void throw_##FUNC() {                                                        \
    TYPE x = VAL;                                                              \
    std::cout << #FUNC << x << "\n";                                           \
    throw x;                                                                   \
  }

THROW(string, std::string, "hello world")

int main() {
  try {
    throw_string();
  } catch (std::string &s) {
    std::cout << s << "\n";
  }
  return 0;
}
