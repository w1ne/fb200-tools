/* 256-entry vector table; copied to ITCM 0x0 and selected via VTOR = 0.
 * Entry [1] keeps the flash reset-stub address (as stock does). */
#include <stdint.h>

extern uint32_t _estack;
extern void reset_stub(void);
void Default_Handler(void)
{
    for (;;) {
    }
}

__attribute__((section(".vectors"), used))
const void *const g_vectors[256] = {
    &_estack,
    reset_stub,
    [2 ... 255] = Default_Handler,
};
