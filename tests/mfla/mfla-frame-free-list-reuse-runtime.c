// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/free-list.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t/free-list.ll -o %t/free-list.mfla.ll
// RUN: %clang %t/free-list.mfla.ll -o %t/free-list.exe
// RUN: %t/free-list.exe > %t/free-list.stdout
// RUN: grep '^mfla-free-list:4$' %t/free-list.stdout

#include <stdio.h>

static volatile int observed_sink;

__attribute__((noinline, used)) static int rec(int n) {
  if (n <= 0)
    return 1;
  return rec(n - 1) + n;
}

__attribute__((noinline, used)) static int twice(int x) {
  int a = rec(x);
  int b = rec(x);
  return a + b;
}

int main(void) {
  int r = twice(1);
  observed_sink = r;
  printf("mfla-free-list:%d\n", r);
  return observed_sink == 4 && r == 4 ? 0 : 1;
}
