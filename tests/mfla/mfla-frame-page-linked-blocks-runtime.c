// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/pages.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -mfla-frames-per-page=1 -mfla-page-table-block-entries=1 -S %t/pages.ll -o %t/pages.mfla.ll
// RUN: %clang %t/pages.mfla.ll -o %t/pages.exe
// RUN: %t/pages.exe > %t/pages.stdout
// RUN: grep '^mfla-pages:11$' %t/pages.stdout

#include <stdio.h>

static volatile int observed_sink;

__attribute__((noinline, used)) static int rec(int n) {
  if (n <= 0)
    return 1;
  return rec(n - 1) + n;
}

int main(void) {
  int r = rec(4);
  observed_sink = r;
  printf("mfla-pages:%d\n", r);
  return observed_sink == 11 && r == 11 ? 0 : 1;
}
