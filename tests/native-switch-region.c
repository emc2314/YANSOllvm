// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/native-switch-region.ll
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -fla -S %t/native-switch-region.ll -o %t/native-switch-region.fla.ll
// RUN: grep 'switch i32' %t/native-switch-region.fla.ll
// RUN: grep 'switch i64' %t/native-switch-region.fla.ll
// RUN: %clang %t/native-switch-region.fla.ll -o %t/native-switch-region.fla
// RUN: %t/native-switch-region.fla | grep '^native-switch:240$'
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -merge -bb2func -fla -connect -obfCon -sub -bcf -S %t/native-switch-region.ll -o %t/native-switch-region.aggressive.ll
// RUN: %clang %t/native-switch-region.aggressive.ll -o %t/native-switch-region.aggressive
// RUN: %t/native-switch-region.aggressive | grep '^native-switch:240$'

#include <stdio.h>

__attribute__((noinline)) static int choose(int x) {
  int y = 0;
  switch (x & 7) {
  case 0:
    y = x + 11;
    break;
  case 1:
  case 2:
    y = x * 3;
    break;
  case 3:
    y = x - 5;
    break;
  case 4:
    y = x ^ 19;
    break;
  case 5:
    y = x + 23;
    break;
  default:
    y = x - 7;
    break;
  }

  if (y & 1)
    y += 5;
  else
    y ^= 9;
  return y;
}

int main(void) {
  int acc = 0;
  /* Expected values for x=3..14: -9,28,21,4,9,24,32,23,15,36,45,12. */
  for (int i = 0; i < 12; ++i)
    acc += choose(i + 3);
  printf("native-switch:%d\n", acc);
  return acc == 240 ? 0 : 1;
}
