// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=bb2func,verify -S %t/input.ll -o %t/bb2func.ll
// RUN: %FileCheck %s --input-file=%t/bb2func.ll
// RUN: %clang %t/bb2func.ll -o %t/bb2func
// RUN: %t/bb2func | grep '^branch-region:249$'

// CHECK-LABEL: define {{.*}}@branch_region(
// CHECK: call {{.*}}@branch_region.
// CHECK-LABEL: define internal {{.*}}@branch_region.
// CHECK: newFuncRoot:
// CHECK: br label

#include <stdio.h>

__attribute__((noinline)) int branch_region(int x, int y) {
  int r;
  if (x > y) {
    int t = x + 13;
    r = t * 3;
  } else {
    int e = y - x + 17;
    r = e * 5;
  }

  if ((r & 1) == 0)
    r += 11;
  else
    r += 19;
  return r;
}

int main(void) {
  int a = branch_region(20, 3);
  int b = branch_region(2, 9);
  printf("branch-region:%d\n", a + b);
  return 0;
}
