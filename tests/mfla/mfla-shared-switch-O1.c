// RUN: %clang -O1 -S -emit-llvm %s -o %t.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t.ll -o %t.mfla.ll
// RUN: grep '__yansollvm_mfla_main' %t.mfla.ll
// RUN: grep 'indirectbr' %t.mfla.ll
// RUN: %not grep 'switch i32' %t.mfla.ll
// RUN: %clang %t.mfla.ll -o %t.exe
// RUN: %t.exe

__attribute__((noinline, used, optnone)) static int shared_switch(int x) {
  int v;
  switch (x) {
  case 0:
  case 1:
    v = 10;
    break;
  case 2:
    v = 20;
    break;
  case 4:
    v = 40;
    break;
  default:
    v = 7;
    break;
  }
  return v + x;
}

__attribute__((noinline, used)) static int sum_shared(void) {
  return shared_switch(0) + shared_switch(1) + shared_switch(2) +
         shared_switch(3) + shared_switch(4);
}

int main(void) { return sum_shared() == 10 + 11 + 22 + 10 + 44 ? 0 : 1; }
