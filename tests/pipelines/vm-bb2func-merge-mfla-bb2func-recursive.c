// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/pipeline.ll
// RUN: %clang %t/pipeline.ll -o %t/orig
// RUN: %t/orig > %t/orig.stdout
// RUN: grep '^pipeline-mfla:54$' %t/orig.stdout
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -bb2func -merge -mfla -bb2func -S %t/pipeline.ll -o %t/pipeline.obf.ll
// RUN: grep '__yansollvm_mfla_main' %t/pipeline.obf.ll
// RUN: %clang %t/pipeline.obf.ll -o %t/obf
// RUN: %t/obf > %t/obf.stdout
// RUN: grep '^pipeline-mfla:54$' %t/obf.stdout

#include <stdio.h>

static volatile int observed_sink;

__attribute__((noinline, used)) static int rec(int n) {
  if (n <= 0)
    return 1;
  return rec(n - 1) + n;
}

__attribute__((noinline, used)) static int even(int n);

__attribute__((noinline, used)) static int odd(int n) {
  if (n <= 0)
    return 0;
  return even(n - 1) + 3;
}

__attribute__((noinline, used)) static int even(int n) {
  if (n <= 0)
    return 2;
  return odd(n - 1) + 5;
}

__attribute__((noinline, used)) static int run_all(int x) {
  int a = rec(x);
  int b = rec(x - 2);
  int c = even(4);
  int d = odd(3);
  return a + b + c + d;
}

int main(void) {
  int r = run_all(5);
  observed_sink = r;
  printf("pipeline-mfla:%d\n", r);
  if (observed_sink != 54)
    return 2;
  return r == 54 ? 0 : 1;
}
