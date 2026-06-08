// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang++ -O3 -DNDEBUG -std=gnu++17 -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=sobf,verify -S %t/input.ll -o %t/obf.ll
// RUN: %clang++ %t/obf.ll -lm -o %t/obf
// RUN: %t/obf | grep '^4: PASS$'

// Reduced from llvm-test-suite SingleSource/Regression/C++/2011-03-28-Bitfield.cpp.
// Regression for StringEncryptionPass rewriting a clang relative pointer switch
// table.  The generated lazy initializer used to call itself recursively before
// setting its status flag, stack-overflowing before printing the expected
// result.

#include <stdio.h>

typedef struct _operation {
  unsigned int datatype : 3;
} operation;

operation op;

void __attribute__((__noinline__)) init() { op.datatype = 4; }

int main(int argc, char *argv[]) {
  init();
  if ((1 == op.datatype) || (2 == op.datatype) || (3 == op.datatype)) {
    printf("1, 2 or 3: FAIL\n");
  } else if (4 == op.datatype) {
    printf("4: PASS\n");
    return 0;
  } else {
    printf("Not 1,2,3 or 4: FAIL\n");
  }
  return -1;
}
