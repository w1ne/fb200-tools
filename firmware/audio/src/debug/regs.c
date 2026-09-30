/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Safe 32-bit register access for the console. A read or write of a
 * clock-gated i.MX RT peripheral stalls the bus forever (it hung the pedal
 * once), so every access is checked against the peripheral's CCM clock gate
 * first. Gates come from the SDK's kCLOCK_* constants (fsl_clock.h). */
#include <stdint.h>
#include <stddef.h>
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include "cdc_log.h"
#include "regs.h"

#define NO_GATE 0xFFFFFFFFu   /* always clocked */

typedef struct {
    uint32_t base, size;
    uint32_t gate;            /* clock_ip_name_t, or NO_GATE */
    const char *name;
} periph_t;

static const periph_t periphs[] = {
    { CCM_BASE, 0x4000, NO_GATE, "CCM" },
    { CCM_ANALOG_BASE, 0x1000, NO_GATE, "CCM_ANALOG/PMU" },
    { USBPHY1_BASE, 0x1000, NO_GATE, "USBPHY1" },
    { SRC_BASE, 0x4000, NO_GATE, "SRC" },
    { 0x400F4000u, 0x4000, NO_GATE, "GPC" },
    { DCDC_BASE, 0x4000, NO_GATE, "DCDC" },
    { 0xE000E000u, 0x1000, NO_GATE, "SCS (NVIC/SCB/SysTick)" },
    { IOMUXC_BASE, 0x4000, kCLOCK_Iomuxc, "IOMUXC" },
    { IOMUXC_GPR_BASE, 0x4000, kCLOCK_IomuxcGpr, "IOMUXC_GPR" },
    { IOMUXC_SNVS_BASE, 0x4000, kCLOCK_IomuxcSnvs, "IOMUXC_SNVS" },
    { SNVS_BASE, 0x4000, NO_GATE, "SNVS" },
    { OCOTP_BASE, 0x4000, kCLOCK_Ocotp, "OCOTP" },
    { WDOG1_BASE, 0x4000, kCLOCK_Wdog1, "WDOG1" },
    { WDOG2_BASE, 0x4000, kCLOCK_Wdog2, "WDOG2" },
    { RTWDOG_BASE, 0x4000, kCLOCK_Wdog3, "RTWDOG" },
    { GPIO1_BASE, 0x4000, kCLOCK_Gpio1, "GPIO1" },
    { GPIO2_BASE, 0x4000, kCLOCK_Gpio2, "GPIO2" },
    { GPIO3_BASE, 0x4000, kCLOCK_Gpio3, "GPIO3" },
    { GPIO4_BASE, 0x4000, kCLOCK_Gpio4, "GPIO4" },
    { GPIO5_BASE, 0x4000, kCLOCK_Gpio5, "GPIO5" },
    { LPI2C1_BASE, 0x4000, kCLOCK_Lpi2c1, "LPI2C1" },
    { LPI2C2_BASE, 0x4000, kCLOCK_Lpi2c2, "LPI2C2" },
    { LPI2C3_BASE, 0x4000, kCLOCK_Lpi2c3, "LPI2C3" },
    { LPI2C4_BASE, 0x4000, kCLOCK_Lpi2c4, "LPI2C4" },
    { SAI1_BASE, 0x4000, kCLOCK_Sai1, "SAI1" },
    { SAI2_BASE, 0x4000, kCLOCK_Sai2, "SAI2" },
    { SAI3_BASE, 0x4000, kCLOCK_Sai3, "SAI3" },
    { DMA0_BASE, 0x4000, kCLOCK_Dma, "eDMA" },
    { DMAMUX_BASE, 0x4000, kCLOCK_Dma, "DMAMUX" },
    { USB1_BASE, 0x1000, kCLOCK_UsbOh3, "USB1" },
    { FLEXSPI_BASE, 0x4000, kCLOCK_FlexSpi, "FLEXSPI" },
    { LPUART1_BASE, 0x4000, kCLOCK_Lpuart1, "LPUART1" },
    { LPUART2_BASE, 0x4000, kCLOCK_Lpuart2, "LPUART2" },
    { LPUART3_BASE, 0x4000, kCLOCK_Lpuart3, "LPUART3" },
    { LPUART4_BASE, 0x4000, kCLOCK_Lpuart4, "LPUART4" },
    { LPUART5_BASE, 0x4000, kCLOCK_Lpuart5, "LPUART5" },
    { LPUART6_BASE, 0x4000, kCLOCK_Lpuart6, "LPUART6" },
    { LPUART7_BASE, 0x4000, kCLOCK_Lpuart7, "LPUART7" },
    { LPUART8_BASE, 0x4000, kCLOCK_Lpuart8, "LPUART8" },
    { LPSPI1_BASE, 0x4000, kCLOCK_Lpspi1, "LPSPI1" },
    { LPSPI2_BASE, 0x4000, kCLOCK_Lpspi2, "LPSPI2" },
    { LPSPI3_BASE, 0x4000, kCLOCK_Lpspi3, "LPSPI3" },
    { LPSPI4_BASE, 0x4000, kCLOCK_Lpspi4, "LPSPI4" },
    { PIT_BASE, 0x4000, kCLOCK_Pit, "PIT" },
    { GPT1_BASE, 0x4000, kCLOCK_Gpt1, "GPT1" },
    { GPT2_BASE, 0x4000, kCLOCK_Gpt2, "GPT2" },
    { ADC1_BASE, 0x4000, kCLOCK_Adc1, "ADC1" },
    { ADC2_BASE, 0x4000, kCLOCK_Adc2, "ADC2" },
    { PWM1_BASE, 0x4000, kCLOCK_Pwm1, "PWM1" },
    { PWM2_BASE, 0x4000, kCLOCK_Pwm2, "PWM2" },
    { PWM3_BASE, 0x4000, kCLOCK_Pwm3, "PWM3" },
    { PWM4_BASE, 0x4000, kCLOCK_Pwm4, "PWM4" },
    { TMR1_BASE, 0x4000, kCLOCK_Timer1, "TMR1" },
    { TMR2_BASE, 0x4000, kCLOCK_Timer2, "TMR2" },
    { TMR3_BASE, 0x4000, kCLOCK_Timer3, "TMR3" },
    { TMR4_BASE, 0x4000, kCLOCK_Timer4, "TMR4" },
    { LCDIF_BASE, 0x4000, kCLOCK_Lcd, "LCDIF" },
    { FLEXIO1_BASE, 0x4000, kCLOCK_Flexio1, "FLEXIO1" },
    { FLEXIO2_BASE, 0x4000, kCLOCK_Flexio2, "FLEXIO2" },
    { KPP_BASE, 0x4000, kCLOCK_Kpp, "KPP" },
    { SPDIF_BASE, 0x4000, kCLOCK_Spdif, "SPDIF" },
    { TRNG_BASE, 0x4000, kCLOCK_Trng, "TRNG" },
    { XBARA1_BASE, 0x4000, kCLOCK_Xbar1, "XBARA1" },
};

static const periph_t *lookup(uint32_t addr)
{
    for (size_t i = 0; i < sizeof periphs / sizeof periphs[0]; i++) {
        if (addr - periphs[i].base < periphs[i].size) return &periphs[i];
    }
    return NULL;
}

static int gate_on(uint32_t gate)
{
    if (gate == NO_GATE) return 1;
    volatile uint32_t *ccgr = &CCM->CCGR0 + (gate >> 8);
    return ((*ccgr >> (gate & 0xFFu)) & 3u) != 0u;
}

int reg_access_ok(uint32_t addr, const char **name)
{
    *name = "";
    if (addr & 3u) { log_printf("unaligned\r\n"); return 0; }
    if (addr < 0x40000000u || (addr >= 0x60000000u && addr < 0xE0000000u)) {
        *name = "memory";
        return 1;   /* TCM/OCRAM/flash: the memory checks in console.c apply */
    }
    const periph_t *p = lookup(addr);
    if (!p) { log_printf("%08x: not in the peripheral table (unsafe)\r\n", (unsigned)addr); return 0; }
    *name = p->name;
    if (!gate_on(p->gate)) {
        log_printf("%08x: %s clock is gated; access would stall the bus\r\n",
                   (unsigned)addr, p->name);
        return 0;
    }
    return 1;
}

void clocks_print(void)
{
    volatile uint32_t *ccgr = &CCM->CCGR0;
    for (int i = 0; i < 8; i++) log_printf("CCGR%d=%08x ", i, (unsigned)ccgr[i]);
    log_printf("\r\n");
    for (size_t i = 0; i < sizeof periphs / sizeof periphs[0]; i++) {
        if (periphs[i].gate != NO_GATE && gate_on(periphs[i].gate))
            log_printf("%s ", periphs[i].name);
    }
    log_printf("(clocked)\r\n");
}
