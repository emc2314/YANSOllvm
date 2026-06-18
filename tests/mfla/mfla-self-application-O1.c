// RUN: %clang -O1 -fno-optimize-sibling-calls -S -emit-llvm %s -o %t.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t.ll -o %t.once.ll
// RUN: grep '__yansollvm_mfla_main' %t.once.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t.once.ll -o %t.twice.ll
// RUN: grep '__yansollvm_mfla_main' %t.twice.ll
// RUN: grep '__yansollvm_mfla_main\.[0-9]' %t.twice.ll
// RUN: %not grep 'inttoptr (i32 1 to ptr)' %t.twice.ll
// RUN: grep '@__yansollvm_mfla_edge.*\.mfla' %t.twice.ll
// RUN: %clang %t.twice.ll -o %t.exe
// RUN: %t.exe

static volatile int G;

__attribute__((noinline, used)) static int step1(int x) { return x + 3; }

__attribute__((noinline, used)) static void sink(int x) { G += x & 7; }

int main(void) {
  G = 5;
  int a = step1(4);
  sink(a);
  int b = step1(5);
  sink(b);
  int c = step1(6);
  return (a + b + c == 24 && G == 12) ? 0 : 1;
}
