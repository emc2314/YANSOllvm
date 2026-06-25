// RUN: %clang -O1 -S -emit-llvm %s -o %t.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %t.ll -o %t.mfla.ll
// RUN: grep '__yansollvm_mfla_main' %t.mfla.ll
// RUN: grep 'indirectbr' %t.mfla.ll
// RUN: %not grep 'switch i32' %t.mfla.ll
// RUN: %not grep '@__yansollvm_mfla_frame = internal global' %t.mfla.ll
// RUN: grep 'alloca \[.* x i8\], align 16' %t.mfla.ll
// RUN: %not grep '@__yansollvm_mfla_ret_cont_xor = internal global' %t.mfla.ll
// RUN: %not grep '@__yansollvm_mfla_ret_cont_edge = internal global' %t.mfla.ll
// RUN: %not grep '__yansollvm_mfla_ret_sp' %t.mfla.ll
// RUN: %not grep '__yansollvm_mfla_ret_stack' %t.mfla.ll
// RUN: %not grep 'select i1 .*blockaddress' %t.mfla.ll
// RUN: %not grep 'select i1 .*mfla\.phi\.old' %t.mfla.ll
// RUN: %not grep '\.true\.phi' %t.mfla.ll
// RUN: %not grep '\.false\.phi' %t.mfla.ll
// RUN: %not grep ' call .*@write_slot' %t.mfla.ll
// RUN: %not grep ' call .*@vector_sum' %t.mfla.ll
// RUN: %clang %t.mfla.ll -o %t.exe
// RUN: %t.exe

static int G;
static int A = 10;
static int B = 20;

__attribute__((noinline, used)) static long helper(long x, int *p) {
  if (*p & 1)
    return x + *p;
  return x - *p;
}

__attribute__((noinline, used)) static int call_helper(int x) {
  long y = helper((long)x, &A);
  return (int)y;
}

__attribute__((noinline, used)) static long loop_sum(int n) {
  long s = 0;
  for (int i = 0; i <= n; ++i)
    s += i;
  return s;
}

__attribute__((noinline, used, optnone)) static int choose(int x) {
  switch (x) {
  case 0:
    return 11;
  case 2:
    return 13;
  case 5:
    return 17;
  default:
    return 19;
  }
}

__attribute__((noinline, used)) static int *pick(int *a, int *b, int flag) {
  return flag ? a : b;
}

__attribute__((noinline, used)) static int read_pick(int flag) {
  int *p = pick(&A, &B, flag);
  return *p + 1;
}

__attribute__((noinline, used)) static void write_slot(int x) { G = x + 5; }

__attribute__((noinline, used)) static int run_write(int x) {
  write_slot(x);
  return G * 2;
}

__attribute__((noinline, used)) static double mixfp(double a, float b, int flag) {
  double base = flag ? a : (double)b;
  return base + 1.5;
}

__attribute__((noinline, used)) static unsigned char narrow(unsigned char x) {
  return (unsigned char)(x + 7);
}

typedef int v4i __attribute__((vector_size(16)));

__attribute__((noinline, used)) static int vector_sum(v4i a, v4i b) {
  v4i c = a + b;
  return c[0] + c[1] + c[2] + c[3];
}

__attribute__((noinline, used)) int exported_api(int z) {
  return run_write(z) + choose(2);
}

int main(void) {
  v4i va = {1, 2, 3, 4};
  v4i vb = {5, 6, 7, 8};
  return call_helper(15) == 5 && loop_sum(10) == 55 && choose(5) == 17 &&
                 choose(4) == 19 && read_pick(0) == 21 && read_pick(1) == 11 &&
                 run_write(16) == 42 && exported_api(7) == 37 &&
                 mixfp(2.5, 4.0f, 1) == 4.0 && narrow(250) == 1 &&
                 vector_sum(va, vb) == 36
             ? 0
             : 1;
}
