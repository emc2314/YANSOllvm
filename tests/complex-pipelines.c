// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/complex.ll
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -sobf -icall -igv -ibr -S %t/complex.ll -o %t/complex.indirect.ll
// RUN: %clang %t/complex.indirect.ll -o %t/complex.indirect
// RUN: %t/complex.indirect | grep '^alpha:37$'
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -merge -bb2func -connect -obfCon -S %t/complex.ll -o %t/complex.mixed.ll
// RUN: %clang %t/complex.mixed.ll -o %t/complex.mixed
// RUN: %t/complex.mixed | grep '^alpha:37$'
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -merge -bb2func -fla -connect -obfCon -sub -bcf -S %t/complex.ll -o %t/complex.aggressive.ll
// RUN: %clang %t/complex.aggressive.ll -o %t/complex.aggressive
// RUN: %t/complex.aggressive | grep '^alpha:37$'

#include <stdio.h>

static const char *message = "alpha";
static int global_seed = 7;

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }

static int dispatch(int x, int (*op)(int, int)) {
  int v = 0;
  switch (x & 3) {
  case 0:
    v = op(x, global_seed);
    break;
  case 1:
    v = op(x ^ 3, global_seed + 1);
    break;
  case 2:
    v = op(x | 5, global_seed - 2);
    break;
  default:
    v = op(x & 9, global_seed + 3);
    break;
  }
  return v;
}

int main(void) {
  int (*op)(int, int) = add;
  int a = dispatch(5, op);
  op = mul;
  int b = dispatch(6, op);
  int result = a + b - 12;
  printf("%s:%d\n", message, result);
  return result == 37 ? 0 : 1;
}
