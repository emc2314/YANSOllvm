// RUN: %clang -O1 -S -emit-llvm %s -o %t.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t.ll -o %t.mfla.ll
// RUN: grep '__yansollvm_mfla_main' %t.mfla.ll
// RUN: grep 'indirectbr' %t.mfla.ll
// RUN: %clang %t.mfla.ll -o %t.exe
// RUN: %t.exe

// Regression test for MFLA PHI lowering.
//
// At -O1 these loops compile to multiple PHI nodes in the same header that
// reference each other (e.g. a' = b, b' = a + b, or a pure swap a' = b,
// b' = a).  PHIs must be evaluated as a simultaneous (parallel) assignment:
// every incoming value is the value held *before* the edge is taken.  MFLA
// spills each PHI to a frame slot; if it stores them one at a time, a later
// PHI that reads an earlier PHI's slot sees the just-written value instead of
// the old one, silently corrupting the result.

#include <stdio.h>

// Pure swap -> two rotating PHIs that each want the OTHER's old value.
__attribute__((noinline, used)) static long swapper(int n) {
  long x = 3, y = 7;
  for (int i = 0; i < n; ++i) {
    long t = x;
    x = y;
    y = t;
  }
  return x * 1000 + y;
}

// Iterative Fibonacci -> a' = b, b' = a + b; b's incoming needs old a.
__attribute__((noinline, used)) static long fib(int n) {
  long a = 0, b = 1;
  for (int i = 0; i < n; ++i) {
    long t = a + b;
    a = b;
    b = t;
  }
  return a;
}

int main(void) {
  // swapper(1) swaps once -> (7, 3) -> 7003; swapper(2) swaps back -> 3007.
  return swapper(1) == 7003 && swapper(2) == 3007 && fib(10) == 55 &&
                 fib(20) == 6765
             ? 0
             : 1;
}
