// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -std=gnu99 -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=bb2func,verify -S %t/input.ll -o %t/bb2func.ll
// RUN: %clang %t/bb2func.ll -lm -o %t/bb2func
// RUN: %t/bb2func | grep '^1\.261'

// Reduced from llvm-test-suite SingleSource/Benchmarks/BenchmarkGame/spectral-norm.c.
// BB2FuncPass must avoid extracting regions from functions that use VLA
// stacksave/stackrestore state.
#include <math.h>
#include <stdio.h>

__attribute__((noinline)) static double eval_A(int i, int j) {
  return 1.0 / (((i + j) * (i + j + 1) / 2) + i + 1);
}

__attribute__((noinline)) static void eval_A_times_u(int N, double u[],
                                                      double Au[]) {
  int i, j;
  for (i = 0; i < N; i++) {
    Au[i] = 0;
    for (j = 0; j < N; j++)
      Au[i] += eval_A(i, j) * u[j];
  }
}

__attribute__((noinline)) static void eval_At_times_u(int N, double u[],
                                                       double Au[]) {
  int i, j;
  for (i = 0; i < N; i++) {
    Au[i] = 0;
    for (j = 0; j < N; j++)
      Au[i] += eval_A(j, i) * u[j];
  }
}

__attribute__((noinline)) static void eval_AtA_times_u(int N, double u[],
                                                        double AtAu[]) {
  double v[N];
  eval_A_times_u(N, u, v);
  eval_At_times_u(N, v, AtAu);
}

int main(int argc, char **argv) {
  int N = argc == 2 ? argv[1][0] - '0' : 5;
  double u[N], v[N], vBv = 0, vv = 0;
  int i;
  for (i = 0; i < N; i++)
    u[i] = 1;
  for (i = 0; i < 2; i++) {
    eval_AtA_times_u(N, u, v);
    eval_AtA_times_u(N, v, u);
  }
  for (i = 0; i < N; i++) {
    vBv += u[i] * v[i];
    vv += v[i] * v[i];
  }
  printf("%0.9f\n", sqrt(vBv / vv));
  return 0;
}
