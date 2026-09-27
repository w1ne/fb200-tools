/* fb200-audio: vendor image format.
 *   block0 0x000..0x400   vector table (copied to ITCM 0x0 by stage2)
 *   block0 0x400..0x7D4   vendor boot region (supplied by the packer)
 *   block0 0x7D4..0x1E39C ITCM payload (entry 0 memcpy -> ITCM 0x400)
 *   entry 4 memset range 0x20018B44..0x2004E3E8 = our .bss
 * Entry point: ITCM 0x4D6 (stage2.S). See docs/FIRMWARE_BRINGUP.md. */

ENTRY(stage2)

MEMORY
{
  ITCM   (rwx) : ORIGIN = 0x00000000, LENGTH = 0x20000
  FLASH  (rx)  : ORIGIN = 0x60010000, LENGTH = 0x31000
  DTCM   (rw)  : ORIGIN = 0x20018B44, LENGTH = 0x358A4
}

SECTIONS
{
  .vectors 0x0 : { KEEP(*(.vectors)) } > ITCM AT> FLASH

  .blob 0x400 : AT(0x600107d4) {
    . += 0xd6;
    KEEP(*(.stage2))
    *(.text*)
    *(EXCLUDE_FILE(*stock_dsp_data.c.o *stock_drums_data.c.o *CommonTables.c.o) .rodata*)
    *(.data*)
    . = ALIGN(4);
    __blob_end__ = .;
  } > ITCM

  /* Recovery only: the app launcher, staged at ITCM 0x1F000 from block 0
   * offset 0xF000 (src/debug/recovery.c). The app's blob must stay below. */
  .copier 0x1F000 : AT(0x6001F000) {
    __copier_start__ = .;
    KEEP(*(.copier))
    . = ALIGN(4);
    __copier_end__ = .;
  } > ITCM

  /* App only: large const tables (stock DSP data, drum rhythms, CMSIS tables)
   * live in DTCM at run time and in flash right after the app slot's ITCM
   * image (F:0x41000, below the presets at 0x71000). stage2_main copies them;
   * the app slot header carries their length and CRC (recovery.h). Reading
   * them from flash at run time would stall the audio during flash writes. */
  .dtcmdata : AT(0x60041000) {
    . = ALIGN(4);
    __dtcmdata_start__ = .;
    *stock_dsp_data.c.o(.rodata*)
    *stock_drums_data.c.o(.rodata*)
    *CommonTables.c.o(.rodata*)
    . = ALIGN(4);
    __dtcmdata_end__ = .;
  } > DTCM

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start__ = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > DTCM

  _estack = 0x20058000;

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
