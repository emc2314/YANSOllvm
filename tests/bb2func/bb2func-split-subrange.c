// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=bb2func,verify -S %t/input.ll -o %t/bb2func.ll
// RUN: %FileCheck %s --input-file=%t/bb2func.ll
// RUN: %clang %t/bb2func.ll -o %t/bb2func
// RUN: %t/bb2func | grep '^split-subrange:48172$'

// The loop body creates a natural linear chain. bb2func should glue/resplit
// eligible windows before extraction, so the extracted helper is a semantic
// slice with real operations rather than a whole natural source-level chain.
// CHECK-LABEL: define {{.*}}@split_subrange(
// CHECK: call {{.*}}@split_subrange.
// CHECK-LABEL: define internal {{.*}}@split_subrange.
// CHECK: newFuncRoot:
// CHECK: {{ add | mul | xor | shl | sub }}

#include <stdio.h>

__attribute__((noinline)) int split_subrange(int x) {
  int acc = x;
  for (int n = 0; n < 9; ++n) {
    acc = acc + n * 3;
    acc = acc ^ (acc << 1);
    acc = acc - (n + 5);
    acc = acc * 3 + 7;
    acc = acc ^ (x + n);
  }
  return acc & 0xffff;
}

int main(void) {
  printf("split-subrange:%d\n", split_subrange(7));
  return 0;
}
