/* fb200-labwired-smoke: the smallest image that proves the FB200 LabWired
 * twin boots and does I/O. Bare registers, no SDK.
 *
 *   1. un-gate the CCM clocks of LPUART5 and GPIO4 (CCGR3 CG1, CG6);
 *   2. print "RT1052 SMOKE OK\n" on LPUART5 (the FB200 Bluetooth UART, which
 *      labwired/system.yaml routes to the host console) at the reset baud;
 *   3. make GPIO4_IO00 (knob LED 1) an output and flip it with DR_TOGGLE.
 *
 * The LabWired twin checks the text AND the GPIO4 GDIR/DR registers
 * (labwired/smoke.yaml), so printed text alone cannot pass it.
 * Register addresses: IMXRT1050RM (CCM, LPUART, GPIO chapters). */

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

#define CCM_CCGR3        REG(0x400FC074u) /* CG1 lpuart5 [3:2], CG6 gpio4 [13:12] */

#define LPUART5_BASE     0x40194000u
#define LPUART5_STAT     REG(LPUART5_BASE + 0x14u)
#define LPUART5_CTRL     REG(LPUART5_BASE + 0x18u)
#define LPUART5_DATA     REG(LPUART5_BASE + 0x1Cu)
#define STAT_TDRE        (1u << 23)
#define CTRL_TE          (1u << 19)
#define CTRL_RE          (1u << 18)

#define GPIO4_GDIR       REG(0x401C4004u)
#define GPIO4_DR_TOGGLE  REG(0x401C408Cu)
#define KNOB_LED1        (1u << 0) /* GPIO4_IO00, active low */

extern uint32_t _estack;
void reset_handler(void);

static void default_handler(void)
{
    for (;;) {
    }
}

/* [SP][reset][NMI][HardFault] - linked at 0x60010000, where the chip yaml's
 * reset_vector_offset puts the image's vector table (the i.MX RT boot ROM,
 * the FlexSPI config block and the IVT are not simulated). */
__attribute__((section(".vectors"), used))
static void (*const vectors[])(void) = {
    (void (*)(void))&_estack,
    reset_handler,
    default_handler,
    default_handler,
};

static void uart_puts(const char *s)
{
    while (*s) {
        while ((LPUART5_STAT & STAT_TDRE) == 0) {
        }
        LPUART5_DATA = (uint32_t)(uint8_t)*s++;
    }
}

void reset_handler(void)
{
    CCM_CCGR3 |= (3u << 2) | (3u << 12);
    LPUART5_CTRL = CTRL_TE | CTRL_RE;
    GPIO4_GDIR |= KNOB_LED1;
    uart_puts("RT1052 SMOKE OK\n");
    GPIO4_DR_TOGGLE = KNOB_LED1;
    for (;;) {
    }
}
