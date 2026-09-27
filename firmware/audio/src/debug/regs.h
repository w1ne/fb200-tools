#ifndef FB200_REGS_H
#define FB200_REGS_H
#include <stdint.h>
/* 1 when a 32-bit access at addr is safe: memory, or a known peripheral
 * whose CCM clock gate is open. Prints why not otherwise. */
int reg_access_ok(uint32_t addr, const char **name);
void clocks_print(void);
#endif
