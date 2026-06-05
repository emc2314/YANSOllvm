// XFAIL: *
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang++ -std=gnu++14 -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=bb2func,verify -S %t/input.ll -o %t/bb2func.ll
// Reduced from llvm-test-suite SingleSource/UnitTests/EH/simple-2.cpp.
// BB2FuncPass extracts EH regions out of alwaysinline functions and unconditionally
// adds noinline to extracted helpers, producing invalid alwaysinline+noinline IR.
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
