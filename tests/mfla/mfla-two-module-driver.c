#include <stdio.h>

extern int rec_entry(int n);
static volatile int observed_sink;

int main(void) {
  int total = 0;
  for (int i = 0; i < 100; ++i)
    total += rec_entry(i % 10);
  observed_sink = total;
  printf("two-module-leak:%d\n", total);
  return observed_sink == 1750 && total == 1750 ? 0 : 1;
}
