#include "blink/builtin.h"

#if defined(__aarch64__) && defined(__COSMOCC__)
void __clear_cache(char *beg, char *end) {
  /* Use dc/ic instructions to flush aarch64 icache */
  unsigned long addr;
  static unsigned long cache_line = 64;
  for (addr = (unsigned long)beg & ~(cache_line - 1);
       addr < (unsigned long)end; addr += cache_line) {
    __asm__ volatile("dc cvau, %0" : : "r"(addr) : "memory");
  }
  __asm__ volatile("dsb ish" : : : "memory");
  for (addr = (unsigned long)beg & ~(cache_line - 1);
       addr < (unsigned long)end; addr += cache_line) {
    __asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");
  }
  __asm__ volatile("dsb ish\n\tisb" : : : "memory");
}
#endif
