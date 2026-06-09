// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O1 -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=bb2func,verify -S %t/input.ll -o %t/bb2func.ll
// RUN: %FileCheck %s --input-file=%t/bb2func.ll
// RUN: %clang %t/bb2func.ll -o %t/bb2func
// RUN: %t/bb2func | grep '^linear:43$'

// A pure straight-line function has no branch/switch region candidate.  The
// pass may still use the legacy single-BB fallback, but it must not manufacture
// a multi-block region from the linear chain.
// CHECK-NOT: define internal {{.*}}@linear{{[.][A-Za-z0-9_.]*}}\(i32

#include <stdio.h>

__attribute__((noinline)) int linear(int x) {
  int a = x + 1;
  int b = a * 3;
  int c = b ^ 7;
  int d = c + 11;
  return d;
}

int main(void) {
  printf("linear:%d\n", linear(12));
  return 0;
}
