// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/smoke.ll
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -fla -sub -split -S %t/smoke.ll -o %t/smoke.obf.ll
// RUN: %clang %t/smoke.obf.ll -o %t/smoke
// RUN: %t/smoke | grep '^-108$'

#include <stdio.h>

static int fibish(int n) {
  int a = 1, b = 1;
  for (int i = 0; i < n; ++i) {
    int c = (a + b) ^ (i * 3 + 7);
    a = b + (c & 3);
    b = c - a;
  }
  return (a ^ b) + n;
}

int main(void) {
  int s = 0;
  for (int i = 0; i < 9; ++i)
    s += fibish(i);
  printf("%d\n", s);
  return s == -108 ? 0 : 1;
}
