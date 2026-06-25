// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 -emit-llvm -S %s -o %t/callee.ll
// RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -mfla-frames-per-page=1 -mfla-page-table-block-entries=1 -S %t/callee.ll -o %t/callee.mfla.ll
// RUN: %clang -c %t/callee.mfla.ll -o %t/callee.o
// RUN: %clang -O0 -c %S/mfla-two-module-driver.c -o %t/driver.o
// RUN: %clang %t/driver.o %t/callee.o -o %t/two-module.exe
// RUN: valgrind --leak-check=full --error-exitcode=99 %t/two-module.exe > %t/stdout 2> %t/valgrind
// RUN: grep '^two-module-leak:1750$' %t/stdout
// RUN: grep 'All heap blocks were freed -- no leaks are possible' %t/valgrind

__attribute__((noinline, used)) static int rec_impl(int n) {
  if (n <= 0)
    return 1;
  return rec_impl(n - 1) + n;
}

__attribute__((noinline, used)) int rec_entry(int n) {
  return rec_impl(n);
}
