// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang++ -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/merge-eh-complex.ll
// RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -merge-max-group-size=4 -S %t/merge-eh-complex.ll -o %t/merge-eh-complex.out.ll
// RUN: grep 'define internal i64 @.*merge.*personality ptr @__gxx_personality_v0' %t/merge-eh-complex.out.ll
// RUN: grep 'call i64 @.*merge' %t/merge-eh-complex.out.ll
// RUN: %clang++ %t/merge-eh-complex.out.ll -o %t/merge-eh-complex
// RUN: %t/merge-eh-complex | grep '^eh-complex:128$'

#include <stdexcept>
#include <stdio.h>

struct LocalGuard {
  int *p;
  int delta;
  ~LocalGuard() { *p += delta; }
};

__attribute__((noinline)) int leaf_a(int x) {
  if (x == 3)
    throw 5;
  if (x == 4)
    throw std::runtime_error("leaf_a");
  return x + 10;
}

__attribute__((noinline)) int leaf_b(int x) {
  if (x < 0)
    throw 7L;
  if (x == 6)
    throw std::logic_error("leaf_b");
  return x * 2;
}

__attribute__((noinline)) int nested_a(int x) {
  int local = 1;
  LocalGuard guard{&local, 3};
  try {
    int left = leaf_a(x);
    int right = 0;
    try {
      right = leaf_b(x - 8);
    } catch (long v) {
      right = (int)v + local;
    }
    return left + right + local;
  } catch (const std::runtime_error &) {
    return 20 + local;
  }
}

__attribute__((noinline)) int nested_b(int x) {
  int local = 2;
  LocalGuard guard{&local, 4};
  try {
    if ((x & 1) == 0)
      return leaf_b(x) + local;
    return leaf_a(x) - local;
  } catch (int v) {
    return v + local;
  } catch (const std::exception &) {
    return 30 + local;
  }
}

__attribute__((noinline)) int chooser(int x) {
  try {
    int total = nested_a(x);
    total += nested_b(x + 1);
    if (x == 11)
      throw std::runtime_error("chooser");
    return total;
  } catch (const std::logic_error &) {
    return 40;
  } catch (const std::runtime_error &) {
    return 50;
  }
}

int main() {
  int acc = 0;
  try {
    // runtime_error caught inside nested_a: 20 + local(1) before dtor = 21.
    acc += nested_a(4);
  } catch (...) {
    acc += 1000;
  }

  try {
    // leaf_a throws int 5, caught inside nested_b: 5 + local(2) = 7.
    acc += nested_b(3);
  } catch (...) {
    acc += 1000;
  }

  try {
    // nested_a: 16 + (7 + 1) + 1 = 25; nested_b catches logic_error => 32;
    // total 57.
    acc += chooser(6);
  } catch (...) {
    acc += 1000;
  }

  try {
    acc += leaf_a(3); // propagates int 5 to main
  } catch (int v) {
    acc += v;
  } catch (...) {
    acc += 1000;
  }

  try {
    acc += leaf_b(6); // propagates logic_error to main
  } catch (const std::logic_error &) {
    acc += 55;
  } catch (...) {
    acc += 1000;
  }

  printf("eh-complex:%d\n", acc);
  return acc == 128 ? 0 : 1;
}
