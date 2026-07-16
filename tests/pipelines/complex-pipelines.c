// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/complex.ll
// RUN: %opt -load-pass-plugin %plugin -passes=obfcon,vm,bb2func,merge,fla,icall,bb2func -verify-each -S %t/complex.ll -o %t/complex.fla-icall.ll
// RUN: %clang %t/complex.fla-icall.ll -o %t/complex.fla-icall
// RUN: %t/complex.fla-icall | grep '^alpha:37$'
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -S %t/complex.ll -o %t/complex.yanso.ll
// RUN: %clang %t/complex.yanso.ll -o %t/complex.yanso
// RUN: %t/complex.yanso | grep '^alpha:37$'
// RUN: %opt -load-pass-plugin %plugin -passes=connect,yanso -verify-each -S %t/complex.ll -o %t/complex.connect-yanso.ll
// RUN: %clang %t/complex.connect-yanso.ll -o %t/complex.connect-yanso
// RUN: %t/complex.connect-yanso | grep '^alpha:37$'

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
