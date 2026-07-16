// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang++ -O0 -emit-llvm -S %s -o %t/eh-cleanup.ll
// RUN: %opt -load-pass-plugin %plugin -passes=fla -verify-each -S %t/eh-cleanup.ll -o %t/eh-cleanup.fla.ll
// RUN: grep 'switch i64' %t/eh-cleanup.fla.ll
// RUN: grep 'landingpad' %t/eh-cleanup.fla.ll
// RUN: %clang++ %t/eh-cleanup.fla.ll -o %t/eh-cleanup
// RUN: %t/eh-cleanup | grep '^eh-tangled:228,191,192,186,223,230,228:-633924900$'

#include <cstdio>

struct E {
  int code;
};
struct F {
  int code;
};
struct G {
  int code;
};

struct Guard {
  int *p;
  int v;
  ~Guard() { *p += v; }
};

__attribute__((noinline)) static void maybe_throw(int x) {
  switch (x) {
  case 1:
    throw E{3};
  case 2:
    throw F{5};
  case 3:
    throw G{7};
  default:
    return;
  }
}

__attribute__((noinline)) static int tangled(int x) {
  int acc = 0;
  for (int i = 0; i < 3; ++i) {
    acc += i + 1;
    try {
      Guard outer{&acc, 10 + i};
      switch ((x + i) % 6) {
      case 0:
        acc += 100;
        break;
      case 1: {
        Guard inner{&acc, 20};
        acc += 4;
        maybe_throw(1);
        acc += 1000;
        break;
      }
      case 2:
        acc += 8;
        goto after_try;
      case 3:
        try {
          Guard nested{&acc, 30};
          acc += 6;
          maybe_throw(2);
          acc += 2000;
        } catch (const F &f) {
          acc += f.code * 7;
          if (x & 1)
            goto loop_tail;
        }
        break;
      case 4:
        acc += 9;
        maybe_throw(3);
        break;
      default:
        acc += 7;
        break;
      }
      acc += 3;
    } catch (const E &e) {
      acc += e.code * 11;
    } catch (...) {
      acc += 41;
    }

after_try:
    acc += 5;
loop_tail:
    acc += 2;
  }
  return acc;
}

int main() {
  int vals[7];
  int total = 0;
  for (int i = 0; i < 7; ++i) {
    vals[i] = tangled(i);
    total = total * 131 + vals[i];
  }
  std::printf("eh-tangled:%d,%d,%d,%d,%d,%d,%d:%d\n", vals[0], vals[1],
              vals[2], vals[3], vals[4], vals[5], vals[6], total);
  return total == 0 ? 1 : 0;
}
