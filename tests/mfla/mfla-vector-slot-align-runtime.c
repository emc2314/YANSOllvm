// RUN: %clang -O1 -mavx -S -emit-llvm %s -o %t.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t.ll -o %t.mfla.ll
// RUN: grep '__yansollvm_mfla_main' %t.mfla.ll
// RUN: grep 'indirectbr' %t.mfla.ll
// RUN: %not grep 'align 32' %t.mfla.ll
// RUN: %clang -mavx %t.mfla.ll -o %t.exe
// RUN: %t.exe

// Regression test for MFLA frame-slot alignment.
//
// An over-16-aligned value (e.g. <8 x float>, ABI align 32 on AVX) is spilled
// to a frame slot whose physical base is only 16-aligned (malloc / the static
// ctx alloca guarantee 16, and the frame stride is 16-aligned).  The emitted
// load/store must therefore be tagged with an alignment the storage can
// actually honor (<= 16); tagging it `align 32` is misaligned-access UB that
// can fault (#GP) on a vmovaps against a 16-aligned address.

#include <stdio.h>

typedef float v8 __attribute__((vector_size(32)));

__attribute__((noinline, used)) static v8 vadd(v8 a, v8 b) { return a + b; }
__attribute__((noinline, used)) static v8 vmul(v8 a, v8 b) { return a * b; }

int main(void) {
  v8 a = {1, 2, 3, 4, 5, 6, 7, 8};
  v8 b = {8, 7, 6, 5, 4, 3, 2, 1};
  v8 s = vadd(a, b);
  v8 p = vmul(a, b);
  // s is all 9s -> s[0]+s[7] == 18; p[0]+p[7] == 1*8 + 8*1 == 16.
  return (s[0] + s[7]) == 18 && (p[0] + p[7]) == 16 ? 0 : 1;
}
