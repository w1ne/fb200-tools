/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* fb200-labwired-smoke: the smallest image that proves the FB200 LabWired
 * twin boots and does I/O. Bare registers, no SDK.
 *
 *   1. un-gate the CCM clocks of LPUART5 and GPIO4 (CCGR3 CG1, CG6);
 *   2. print "RT1052 SMOKE OK\n" on LPUART5 (the FB200 Bluetooth UART, which
 *      labwired/system.yaml routes to the host console) at the reset baud;
 *   3. make GPIO4_IO00 (knob LED 1) an output and flip it with DR_TOGGLE;
 *   4. multiplex "LAb" on the 3-digit 14-segment display, forever, the way
 *      the open firmware's display driver does (firmware/audio/src/ui/
 *      display.c: selects off, segments, next select), with the same
 *      segment bits.
 *
 * The LabWired twin checks the text, the GPIO4 GDIR/DR registers and the
 * text the display model shows (labwired/smoke.yaml), so printed text alone
 * cannot pass it.
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

#define CCM_CCGR2        REG(0x400FC070u) /* CG13 gpio3 [27:26] */
#define GPIO3_DR         REG(0x401C0000u)
#define GPIO3_GDIR       REG(0x401C0004u)
#define GPIO4_DR         REG(0x401C4000u)
#define GPIO4_GDIR       REG(0x401C4004u)
#define GPIO4_DR_TOGGLE  REG(0x401C408Cu)
#define KNOB_LED1        (1u << 0) /* GPIO4_IO00, active low */

/* Display (docs/UI_AND_STORAGE.md section 1): segments GPIO4_IO16..30,
 * digit selects GPIO4_IO31, GPIO3_IO18, GPIO3_IO21, all active high.
 * Segment bits as in firmware/audio/src/ui/display.c. */
#define SA   (1u << 25)
#define SB   (1u << 24)
#define SC   (1u << 22)
#define SD   (1u << 21)
#define SE   (1u << 16)
#define SF   (1u << 30)
#define SG1  (1u << 26)
#define SG2  (1u << 20)
#define SEG_MASK 0x7FFF0000u
#define SEL0 (1u << 31)            /* GPIO4 */
#define SEL1 (1u << 18)            /* GPIO3 */
#define SEL2 (1u << 21)            /* GPIO3 */
#define DIGIT_LOOPS 20000u         /* about 0.1 ms per digit on the twin */

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

static void delay(uint32_t loops)
{
    for (volatile uint32_t i = 0; i < loops; i++) {
    }
}

static void select_digit(int d)
{
    GPIO4_DR = (GPIO4_DR & ~SEL0) | (d == 0 ? SEL0 : 0u);
    GPIO3_DR = (GPIO3_DR & ~(SEL1 | SEL2)) | (d == 1 ? SEL1 : 0u) |
               (d == 2 ? SEL2 : 0u);
}

void reset_handler(void)
{
    static const uint32_t text[3] = {
        SD | SE | SF,                          /* L */
        SA | SB | SC | SE | SF | SG1 | SG2,    /* A */
        SC | SD | SE | SF | SG1 | SG2,         /* b */
    };

    CCM_CCGR3 |= (3u << 2) | (3u << 12);
    CCM_CCGR2 |= 3u << 26;
    LPUART5_CTRL = CTRL_TE | CTRL_RE;
    GPIO4_GDIR |= KNOB_LED1;
    uart_puts("RT1052 SMOKE OK\n");
    GPIO4_DR_TOGGLE = KNOB_LED1;

    GPIO4_GDIR |= SEG_MASK | SEL0;
    GPIO3_GDIR |= SEL1 | SEL2;
    for (unsigned d = 0;; d = (d + 1u) % 3u) {
        select_digit(-1);                                  /* no ghosting */
        GPIO4_DR = (GPIO4_DR & ~SEG_MASK) | text[d];       /* keeps LED bits */
        select_digit((int)d);
        delay(DIGIT_LOOPS);
    }
}
