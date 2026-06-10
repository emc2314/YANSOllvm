// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=bb2func,verify -S %t/input.ll -o %t/bb2func.ll
// RUN: %FileCheck %s --input-file=%t/bb2func.ll
// RUN: %clang %t/bb2func.ll -o %t/bb2func
// RUN: %t/bb2func | grep '^linear:1928$'

// A pure straight-line source function should still be reshaped: bb2func first
// splits the dense original block, extracts an artificial semantic slice, and
// leaves runtime behavior unchanged.  The helper must contain real arithmetic,
// not just a branch-only shell.
// CHECK-LABEL: define {{.*}}@linear(
// CHECK: call {{.*}}@linear.
// CHECK-LABEL: define internal {{.*}}@linear.
// CHECK: newFuncRoot:
// CHECK: {{ add | mul | xor | shl | sub }}

#include <stdio.h>

__attribute__((noinline)) int linear(int x) {
  int a = x + 1;
  int b = a * 3;
  int c = b ^ 7;
  int d = c + 11;
  int e = d * 5;
  int f = e - 19;
  int g = f ^ (x << 2);
  int h = g + 23;
  int i = h * 7;
  int j = i - 31;
  int k = j ^ 0x55;
  int l = k + a;
  return l;
}

int main(void) {
  printf("linear:%d\n", linear(12));
  return 0;
}
