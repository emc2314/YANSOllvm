// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/local-ssa.ll
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -bb2func -merge -mfla -S %t/local-ssa.ll -o %t/local-ssa.obf.ll
// RUN: %clang %t/local-ssa.obf.ll -o %t/local-ssa.obf
// RUN: %t/local-ssa.obf > %t/local-ssa.stdout
// RUN: grep '^mfla-local-ssa:23$' %t/local-ssa.stdout

#include <stdio.h>

static volatile int observed_sink;

__attribute__((noinline, used)) static int rec(int n) {
  if (n <= 0)
    return 1;
  return rec(n - 1) + n;
}

__attribute__((noinline, used)) static int run_all(int x) {
  int a = rec(x);
  int b = rec(x - 2);
  return a + b;
}

int main(void) {
  int r = run_all(5);
  observed_sink = r;
  printf("mfla-local-ssa:%d\n", r);
  return observed_sink == 23 && r == 23 ? 0 : 1;
}
