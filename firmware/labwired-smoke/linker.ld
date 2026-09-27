/* fb200-labwired-smoke: executes in place from FlexSPI flash.
 * The vector table sits at 0x60010000 (flash base + the chip yaml's
 * reset_vector_offset), the address the FB200 stock block 0 also uses.
 * No .data/.bss: the code keeps everything in registers and on the stack. */

ENTRY(reset_handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x60010000, LENGTH = 64K
  DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS
{
  .text : {
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
  } > FLASH

  /DISCARD/ : { *(.data*) *(.bss*) *(COMMON) *(.ARM.exidx*) }

  _estack = ORIGIN(DTCM) + LENGTH(DTCM);
}
