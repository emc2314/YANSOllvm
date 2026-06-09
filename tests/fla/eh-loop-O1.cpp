// Regression: -fla must not hang on optimized EH code with a loop.
//
// At -O1+ the loop body holds an invoke result (%call) that is consumed across
// blocks by a PHI.  yansollvm_fix_stack must demote that invoke result, but the
// store it inserts in the normal-dest block keeps the invoke value "used outside
// of its block" forever -- demotion can never make an invoke result local.  An
// earlier "demote one value, then rescan" loop spun on this forever (and a plain
// batch rescan spun on the same invoke), so this case wedged opt indefinitely.
// The fix tracks already-demoted registers so each escaping value is handled at
// most once.  This test is intentionally compiled at -O1 (the -O0 EH tests do
// not exercise cross-block SSA / invoke results and never reproduced the hang).
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang++ -O1 -emit-llvm -S %s -o %t/eh-loop-O1.ll
// RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -fla -S %t/eh-loop-O1.ll -o %t/eh-loop-O1.fla.ll
// RUN: grep 'switch i64' %t/eh-loop-O1.fla.ll
// RUN: grep 'invoke' %t/eh-loop-O1.fla.ll
// RUN: grep 'landingpad' %t/eh-loop-O1.fla.ll
// RUN: %clang++ %t/eh-loop-O1.fla.ll -o %t/eh-loop-O1
// RUN: %t/eh-loop-O1 | grep '^acc=100$'

#include <cstdio>

struct G {
  int id;
  G(int i) : id(i) { printf("ctor %d\n", id); }
  ~G() { printf("dtor %d\n", id); }
};

__attribute__((noinline)) static int risky(int x) {
  G g(x);
  if (x > 3)
    throw x * 10;
  return x + 1;
}

int main() {
  int acc = 0;
  for (int i = 0; i < 6; i++) {
    try {
      G outer(100 + i);
      acc += risky(i);
    } catch (int e) {
      printf("caught %d\n", e);
      acc += e;
    }
  }
  printf("acc=%d\n", acc);
  return acc == 100 ? 0 : 1;
}
