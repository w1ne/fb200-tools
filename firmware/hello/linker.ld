/* fb200-hello, vendor image format.
 *
 * Block 0 layout (see docs/FIRMWARE_BRINGUP.md):
 *   0x000..0x400  vector table, copied to ITCM 0x0 by stage2
 *   0x400..0x7d4  vendor boot region (stub, loader, table) - supplied by the
 *                 packer from the stock template, not built here
 *   0x7d4..0x1e39c ITCM payload, memcpy'd to ITCM 0x400 by the vendor loader
 *
 * The vendor loader jumps to the fixed address ITCM 0x4d6 after loading,
 * which is blob offset 0xd6 - stage2 sits exactly there. */

ENTRY(stage2)

MEMORY
{
  ITCM   (rwx) : ORIGIN = 0x00000000, LENGTH = 0x20000
  FLASH  (rx)  : ORIGIN = 0x60010000, LENGTH = 0x31000
}

SECTIONS
{
  /* Vector table: VMA ITCM 0x0, stored at block0 0x0. */
  .vectors 0x0 : { KEEP(*(.vectors)) } > ITCM AT> FLASH

  /* ITCM payload: VMA ITCM 0x400, stored at block0 0x7d4 (LMA). */
  .blob 0x400 : AT(0x600107d4) {
    . += 0xd6;                 /* vendor loader's fixed entry point (ITCM 0x4d6) */
    KEEP(*(.stage2))
    *(.text*)
    *(.rodata*)
    *(.data*)
    . = ALIGN(4);
    __blob_end__ = .;
  } > ITCM

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start__ = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > ITCM

  /* Stock stack top (image[0] = initial SP). */
  _estack = 0x20058000;

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
