/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* CMSIS system-clock globals.
 *
 * The MCUXpresso SDK's system_MIMXRT1062.c normally provides these, but that
 * file is deliberately not compiled: our linker.ld, src/startup.c and
 * src/vectors.c own the boot contract (see board/README.md). clock_config.c
 * assigns SystemCoreClock in BOARD_BootClockRUN(); SystemCoreClockUpdate()
 * re-derives it from the CCM afterwards. */
#include <stdint.h>
#include "fsl_clock.h"

uint32_t SystemCoreClock = 24000000u;

void SystemCoreClockUpdate(void)
{
    SystemCoreClock = CLOCK_GetFreq(kCLOCK_CpuClk);
}
