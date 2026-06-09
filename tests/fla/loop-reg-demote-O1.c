// Regression: -fla must not hang on an optimized loop with cross-block SSA.
//
// At -O1+ this function has loop-carried values and multi-incoming PHIs whose
// definitions are used across blocks.  Demoting one of them inserts reload loads
// that are themselves used across blocks; an earlier "demote one value, then
// rescan" loop kept re-selecting those freshly created reloads and spun forever,
// appending another ".reload" suffix every iteration.  No exception handling is
// involved -- any optimized loop reproduced it, which is why the existing -O0
// pipeline tests never caught it.  The fix records demoted registers so reloads
// created by demotion are never demoted again.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O1 -emit-llvm -S %s -o %t/loop-reg-demote-O1.ll
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -fla -S %t/loop-reg-demote-O1.ll -o %t/loop-reg-demote-O1.fla.ll
// RUN: grep 'switch i64' %t/loop-reg-demote-O1.fla.ll
// RUN: %clang %t/loop-reg-demote-O1.fla.ll -o %t/loop-reg-demote-O1
// RUN: %t/loop-reg-demote-O1 | grep '^sum=90$'

#include <stdio.h>

__attribute__((noinline)) static int compute(int a, int b, int c) {
  int r = 0;
  if (a > b) {
    r = a * 2;
    if (c > 0)
      r += c;
    else
      r -= c;
  } else {
    r = b * 3;
    if (c > 5)
      r *= 2;
    else
      r += 1;
  }
  for (int i = 0; i < c; i++)
    r += i;
  return r;
}

int main(void) {
  int s = 0;
  for (int i = 0; i < 5; i++)
    s += compute(i, 4 - i, i + 2);
  printf("sum=%d\n", s);
  return s == 90 ? 0 : 1;
}
