/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Core power, the ITCM part: the main-loop sleep (WFI) and the core clock
 * mux switch. The rest (cpu_power.c) is cold code. See cpu_power.h. */
#include "cpu_power.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"

bool g_cpu_sleep = true;
uint64_t g_cpu_busy;             /* awake cycles since the last cpu_busy() */
uint32_t g_cpu_t_wake, g_cpu_wakes;

void cpu_idle(bool (*work_pending)(void))
{
    if (!g_cpu_sleep) {
        uint32_t t = DWT->CYCCNT;
        g_cpu_busy += t - g_cpu_t_wake;
        g_cpu_t_wake = t;
        return;
    }
    /* Interrupts off from the check to the WFI: an interrupt that arrives
     * in between stays pending and ends the WFI at once. The handlers run
     * after __enable_irq, so their time counts as busy. */
    __disable_irq();
    if (!work_pending()) {
        g_cpu_busy += DWT->CYCCNT - g_cpu_t_wake;
        __DSB();
        __WFI();
        g_cpu_t_wake = DWT->CYCCNT;
        g_cpu_wakes++;
    }
    __enable_irq();
}

/* Move the core (AHB) root to pre_periph source `sel` (0 PLL2, 1 PLL2 PFD2,
 * 3 ARM PLL / ARM_PODF). The pre_periph mux has no glitch-free handshake:
 * hop to periph_clk2 (PLL3 480 MHz) while it changes, as the boot setup
 * does. A few microseconds with interrupts off, from ITCM. */
void cpu_switch_core(uint32_t sel)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    CLOCK_SetMux(kCLOCK_PeriphMux, 1u);
    CLOCK_SetMux(kCLOCK_PrePeriphMux, sel);
    CLOCK_SetMux(kCLOCK_PeriphMux, 0u);
    __DSB();
    __ISB();
    __set_PRIMASK(pm);
}
