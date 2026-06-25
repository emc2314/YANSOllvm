// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/fixed-alloca.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t/fixed-alloca.ll -o %t/fixed-alloca.mfla.ll
// RUN: grep '__yansollvm_mfla_main' %t/fixed-alloca.mfla.ll
// RUN: %clang %t/fixed-alloca.mfla.ll -o %t/fixed-alloca.exe
// RUN: %t/fixed-alloca.exe > %t/fixed-alloca.stdout
// RUN: grep '^mfla-fixed-alloca:5$' %t/fixed-alloca.stdout

#include <stdio.h>

static volatile int observed_sink;

__attribute__((noinline, used)) static int fill_and_sum(int seed) {
  int local[3];
  local[0] = seed;
  local[1] = seed + 1;
  local[2] = local[0] + local[1];
  printf("mfla-fixed-alloca:%d\n", local[2]);
  observed_sink = local[2];
  return local[0] + local[1] + local[2];
}

int main(void) {
  int r = fill_and_sum(2);
  return r == 10 && observed_sink == 5 ? 0 : 1;
}
