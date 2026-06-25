// RUN: %clang -O1 -fno-optimize-sibling-calls -S -emit-llvm %s -o %t.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t.ll -o %t.mfla.ll
// RUN: grep '__yansollvm_mfla_main' %t.mfla.ll
// RUN: grep 'call .*@rec' %t.mfla.ll
// RUN: grep 'call .*@even' %t.mfla.ll
// RUN: grep 'call .*@odd' %t.mfla.ll
// RUN: grep 'alloca \[.* x i8\], align 16' %t.mfla.ll
// RUN: %not grep '@__yansollvm_mfla_frame = internal global' %t.mfla.ll
// RUN: %clang %t.mfla.ll -o %t.exe
// RUN: %t.exe | grep '^mfla-recursive:54$'

#include <stdio.h>

static volatile int sink;

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

__attribute__((noinline, used)) static int twice_rec(int n) {
  int a = rec(n);
  int b = rec(n - 2);
  return a + b;
}

int main(void) {
  int r = twice_rec(5) + even(4) + odd(3);
  sink = r;
  printf("mfla-recursive:%d\n", r);
  if (sink != 54)
    return 2;
  return r == 54 ? 0 : 1;
}
