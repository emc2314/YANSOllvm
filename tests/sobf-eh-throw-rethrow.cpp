// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang++ -std=gnu++14 -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=sobf,verify -S %t/input.ll -o %t/sobf.ll
// RUN: %clang++ %t/sobf.ll -lm -o %t/sobf
// RUN: %t/sobf | grep '^8: 3$'

// Regression from llvm-test-suite
// SingleSource/Regression/C++/EH/throw_rethrow_test.cpp. StringEncryptionPass
// must preserve rethrow/catch behavior.
// This tests hard situations for throwing, including the case where an
// exception is active in more than one handler at a time (ie, it needs
// refcounting)
#include <cstdio>

struct foo {
  int i;
  foo() : i(1) { }
  foo(const foo&) : i(2) {}
};

int callee(unsigned i) {
  if (i < 3) throw (int)i;
  if (i < 6) throw 1.0;
  if (i < 9) throw foo();
  return 0;
}

void rethrow() {
  throw;
}

int main() {
  for (unsigned i = 0; i < 10; ++i) {
    try {
      return callee(i);
    } catch (foo &F) {
      try {
	rethrow();
      } catch (foo &F) {
        std::printf("%d: 3\n", i);
      }
    } catch (int) {
      std::printf("%d: 1\n", i);
    } catch (...) {
      std::printf("%d: 2\n", i);
    }
  }
}
