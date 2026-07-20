// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O0 %s -o %t/original
// RUN: %clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t/input.ll
// RUN: %opt -load-pass-plugin %plugin -passes=obfcon -verify-each -S %t/input.ll -o %t/obf.ll
// RUN: grep 'obfcon\.' %t/obf.ll
// RUN: %clang %t/obf.ll -o %t/obfuscated
// RUN: %t/original > %t/original.out
// RUN: %t/obfuscated > %t/obfuscated.out
// RUN: diff %t/original.out %t/obfuscated.out

#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) static uint64_t combine(uint8_t a, uint16_t b,
                                                   uint32_t c, uint64_t d) {
  uint8_t x8 = (uint8_t)(a + 37u);
  uint16_t x16 = (uint16_t)(b ^ 0x9e37u);
  uint32_t x32 = (c * 0x9e3779b9u) + 12345u;
  uint64_t x64 = (d ^ 0x6a09e667f3bcc908ULL) + 1u;
  return x8 + x16 + x32 + x64;
}

int main(void) {
  printf("%llu\n",
         (unsigned long long)combine(3u, 17u, 99u, 201u));
  return 0;
}
