/* fb200-hello: code runs from ITCM at 0x0; the image lives in FlexSPI flash
 * at 0x60010000 behind an 8-byte boot header ([SP][reset stub]) consumed by
 * the stock bootloader. See docs/HARDWARE.md sections 2-3. */

ENTRY(reset_stub)

MEMORY
{
  BOOT   (rx)  : ORIGIN = 0x60010000, LENGTH = 8
  FLASH  (rx)  : ORIGIN = 0x60010008, LENGTH = 200696
  ITCM   (rwx) : ORIGIN = 0x00000000, LENGTH = 0x20000
  DTCM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x58000
}

SECTIONS
{
  .boot_header : { KEEP(*(.boot_header)) } > BOOT

  .boot_stub : {
    KEEP(*(.boot_stub))
    . = ALIGN(4);
  } > FLASH

  .itcm : {
    . = ALIGN(4);
    __itcm_start__ = .;
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
    __itcm_end__ = .;
  } > ITCM AT> FLASH
  __itcm_lma__ = LOADADDR(.itcm);

  .data : {
    . = ALIGN(4);
    __data_start__ = .;
    *(.data*)
    . = ALIGN(4);
    __data_end__ = .;
  } > DTCM AT> FLASH
  __data_lma__ = LOADADDR(.data);

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start__ = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > DTCM

  _estack = ORIGIN(DTCM) + LENGTH(DTCM);

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
