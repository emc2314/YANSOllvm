// RUN: %clang -O1 -fno-optimize-sibling-calls -S -emit-llvm %s -o %t.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t.ll -o %t.mfla.ll
// RUN: grep '__yansollvm_mfla_main' %t.mfla.ll
// RUN: grep 'mfla.call.cont' %t.mfla.ll
// RUN: %not grep ' call .*@leaf' %t.mfla.ll
// RUN: %not grep ' call .*@mid' %t.mfla.ll
// RUN: %clang %t.mfla.ll -o %t.exe
// RUN: %t.exe

__attribute__((noinline, used)) static int leaf(int x) { return x * 3 + 1; }

__attribute__((noinline, used)) static int mid(int x) { return leaf(x + 2) - x; }

__attribute__((noinline, used)) static int root_a(int x) { return mid(x) + leaf(x); }

__attribute__((noinline, used)) static int root_b(int x) { return leaf(x - 1) + mid(x + 1); }

int main(void) {
  return root_a(5) == 33 && root_b(4) == 27 && root_a(0) == 8 && root_b(1) == 12
             ? 0
             : 1;
}
