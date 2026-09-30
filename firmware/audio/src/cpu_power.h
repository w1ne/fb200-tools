/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_CPU_POWER_H
#define FB200_CPU_POWER_H
/* Core power: sleep (WFI) in the main loop when there is no work, the loop
 * busy time (DWT), and the core clock switch. cpu_idle.c is ITCM code (the
 * main loop and the clock mux switch run it), cpu_power.c cold code. */
#include <stdbool.h>
#include <stdint.h>

/* Once at boot, before the main loop: WFI must not enter a low-power mode
 * (CCM CLPCR LPM = RUN: the core sleeps, every clock and SysTick keep
 * running, any interrupt wakes it). */
void cpu_power_init(void);
/* End of a main-loop pass: sleep until the next interrupt unless
 * `work_pending` (checked with interrupts off: no wake-up is lost). */
void cpu_idle(bool (*work_pending)(void));
void cpu_sleep_enable(bool on);
bool cpu_sleep_enabled(void);
/* Loop busy share since the last call: busy cycles (awake, ISRs included)
 * over the elapsed ms * core clock; wakes = sleeps per second. */
void cpu_busy(uint32_t *busy_permille, uint32_t *wakes_per_s);
/* Core clock in MHz: 600 (the boot clock, ARM PLL), 528 (PLL2) or 396
 * (PLL2 PFD2). The audio (PLL4), USB (PLL3), UART, FlexIO and I2C clocks
 * do not depend on it. Returns 0, or -1 for an unsupported value. */
int cpu_set_clock(unsigned mhz);
unsigned cpu_clock_mhz(void);
void cpu_switch_core(uint32_t pre_periph_sel);   /* cpu_idle.c, used by cpu_set_clock */
unsigned cpu_vdd_soc_mv(void);
#endif
