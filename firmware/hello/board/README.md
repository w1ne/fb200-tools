# Board glue

`board_config.h` holds FB200 facts. The BSP sources (`family.c`, `clock_config.c`,
`pin_mux.c` and the common `board.c`) are compiled directly from the pinned
TinyUSB release under `.deps/tinyusb/hw/bsp/imxrt/` (reference board
`mimxrt1060_evk`, family `imxrt`) so there is exactly one copy of each file.
The MCUXpresso SDK driver subset those files need (clock, GPIO, LPUART, OCOTP,
common) is fetched at TinyUSB-pinned commits from
`.deps/tinyusb/hw/mcu/nxp/` and `.deps/tinyusb/lib/CMSIS_6/`.

- TinyUSB: MIT.
- Files derived from the NXP MCUXpresso SDK inside TinyUSB: BSD-3-Clause.

Crystal frequency is assumed to be 24 MHz (`BOARD_XTAL_HZ`), matching the BSP's
`BOARD_XTAL0_CLK_HZ`; confirm from the PCB before flashing. If the crystal
differs, adapt `clock_config.c` (or add a custom clock config here).

The BSP boot files are **not** compiled: `startup_*.S`, `system_*.c` and
`fsl_flexspi_nor_boot.c` would take over the vector table, reset path and boot
header. Our `linker.ld`, `src/startup.c` and `src/vectors.c` own that contract;
`src/system_clock.c` provides the two CMSIS symbols (`SystemCoreClock`,
`SystemCoreClockUpdate`) that the excluded `system_*.c` would otherwise supply.

The Homebrew `arm-none-eabi-gcc` formula ships no libc headers while the SDK/TinyUSB
sources include `<assert.h>`, `<string.h>`, `<stdlib.h>`, `<stdio.h>` and
`<inttypes.h>`. `src/compat/` supplies minimal freestanding headers (and
`src/compat/stubs.c` allocator/trap stubs) so the build is self-contained; on
toolchains that do ship newlib these shims shadow it with equivalent
declarations.
