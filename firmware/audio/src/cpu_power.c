/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Core power, the cold part (XIP): busy statistics, clock and voltage
 * setup. The sleep and the clock mux switch are ITCM code (cpu_idle.c).
 * See cpu_power.h. */
#include "cpu_power.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"

extern uint32_t tusb_time_millis_api(void);

extern bool g_cpu_sleep;         /* cpu_idle.c (ITCM) */
extern uint64_t g_cpu_busy;
extern uint32_t g_cpu_t_wake, g_cpu_wakes;
static uint32_t s_ms0;
static unsigned s_mhz = 600u;
static uint32_t s_trg_boot;      /* DCDC REG3.TRG as the boot clock setup left it */

/* VDD_SOC for 528 and 396 MHz: 1.225 V. The data sheet asks >= 1.15 V up
 * to 528 MHz and >= 1.25 V for 600 MHz (overdrive); boot sets 1.275 V. */
#define TRG_LOW 0x11u
#define TRG_MV(t) (800u + 25u * (t))

void cpu_power_init(void)
{
    CCM->CLPCR &= ~CCM_CLPCR_LPM_MASK;   /* WFI = sleep in RUN mode, never WAIT/STOP */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_trg_boot = (DCDC->REG3 & DCDC_REG3_TRG_MASK) >> DCDC_REG3_TRG_SHIFT;
    s_mhz = (unsigned)(SystemCoreClock / 1000000u);
    g_cpu_t_wake = DWT->CYCCNT;
    s_ms0 = tusb_time_millis_api();
}

void cpu_sleep_enable(bool on) { g_cpu_sleep = on; }
bool cpu_sleep_enabled(void) { return g_cpu_sleep; }

void cpu_busy(uint32_t *busy_permille, uint32_t *wakes_per_s)
{
    uint32_t now = tusb_time_millis_api(), t = DWT->CYCCNT;
    g_cpu_busy += t - g_cpu_t_wake;
    g_cpu_t_wake = t;
    uint32_t ms = now - s_ms0;
    /* float, no 64-bit division: libgcc's would land in ITCM */
    float busy = (float)(uint32_t)(g_cpu_busy >> 32) * 4294967296.0f + (float)(uint32_t)g_cpu_busy;
    float wall = (float)ms * (float)(SystemCoreClock / 1000u);
    float pm = wall > 0.0f ? busy * 1000.0f / wall : 0.0f;
    *busy_permille = pm > 1000.0f ? 1000u : (uint32_t)pm;
    *wakes_per_s = ms ? g_cpu_wakes / ms * 1000u + g_cpu_wakes % ms * 1000u / ms : 0u;
    g_cpu_busy = 0;
    g_cpu_wakes = 0;
    s_ms0 = now;
}

static void set_trg(uint32_t trg)
{
    DCDC->REG3 = (DCDC->REG3 & ~DCDC_REG3_TRG_MASK) | DCDC_REG3_TRG(trg);
    for (uint32_t i = 0; i < 1000000u && !(DCDC->REG0 & DCDC_REG0_STS_DC_OK_MASK); i++) {
    }
}

static void set_systick(uint32_t hz)
{
    SystemCoreClock = hz;
    SysTick->LOAD = hz / 1000u - 1u;
    SysTick->VAL = 0u;
}

int cpu_set_clock(unsigned mhz)
{
    if (mhz == s_mhz) return 0;
    if (mhz != 600u && mhz != 528u && mhz != 396u) return -1;
    /* check the source before using it */
    if (!(CCM_ANALOG->PLL_SYS & CCM_ANALOG_PLL_SYS_LOCK_MASK) ||
        (CCM_ANALOG->PLL_SYS & (CCM_ANALOG_PLL_SYS_POWERDOWN_MASK | CCM_ANALOG_PLL_SYS_BYPASS_MASK)))
        return -1;
    if (mhz == 396u) {
        uint32_t pfd = CCM_ANALOG->PFD_528;
        if (((pfd & CCM_ANALOG_PFD_528_PFD2_FRAC_MASK) >> CCM_ANALOG_PFD_528_PFD2_FRAC_SHIFT) != 24u ||
            (pfd & CCM_ANALOG_PFD_528_PFD2_CLKGATE_MASK))
            return -1;
    }
    if (mhz == 600u) {
        set_trg(s_trg_boot);                                   /* voltage up first */
        CCM_ANALOG->PLL_ARM_CLR = CCM_ANALOG_PLL_ARM_POWERDOWN_MASK;
        for (uint32_t i = 0; i < 1000000u && !(CCM_ANALOG->PLL_ARM & CCM_ANALOG_PLL_ARM_LOCK_MASK); i++) {
        }
        if (!(CCM_ANALOG->PLL_ARM & CCM_ANALOG_PLL_ARM_LOCK_MASK)) return -1;
        cpu_switch_core(3u);
        set_systick(600000000u);
    } else {
        cpu_switch_core(mhz == 528u ? 0u : 1u);
        set_systick(mhz * 1000000u);
        if (s_mhz == 600u) {
            CCM_ANALOG->PLL_ARM_SET = CCM_ANALOG_PLL_ARM_POWERDOWN_MASK;   /* unused now */
            if (s_trg_boot > TRG_LOW) set_trg(TRG_LOW);                 /* voltage down last */
        }
    }
    s_mhz = mhz;
    return 0;
}

unsigned cpu_clock_mhz(void) { return s_mhz; }

unsigned cpu_vdd_soc_mv(void)
{
    return TRG_MV((DCDC->REG3 & DCDC_REG3_TRG_MASK) >> DCDC_REG3_TRG_SHIFT);
}
