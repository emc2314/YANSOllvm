// XFAIL: *
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang++ -std=gnu++14 -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=sobf,verify -S %t/input.ll -o %t/sobf.ll
// RUN: %clang++ %t/sobf.ll -lm -o %t/sobf
// RUN: timeout 5s %t/sobf > %t/out.txt

// XFAIL reproducer from llvm-test-suite
// SingleSource/Regression/C++/EH/inlined_cleanup.cpp. StringEncryptionPass
// currently makes this EH cleanup test repeat destructor output without bound.
#include <cstdio>
#include <cstring>

class Cleanup {
  char name[10];
public:
  Cleanup(const char* n) {
    strcpy(name, n);
  }
  ~Cleanup() {
    printf("Cleanup for %s!\n", name);
  }
};

static void foo() {
  Cleanup C("num");
  throw 3;
}

int main(void) {
  try {
    foo();
  } catch (int i) {
    printf("Caught %d!\n", i);
  }
  try {
    Cleanup a("a");
    throw Cleanup("c");
    Cleanup b("b");
  } catch (Cleanup &c) {
    printf("Caught cleanup!\n");
  }
  try {
    Cleanup a("ap");
    throw new Cleanup("cp");
    Cleanup b("bp");
  } catch (Cleanup *c) {
    printf("Caught cleanup!\n");
    delete c;
  }
  return 0;
}
