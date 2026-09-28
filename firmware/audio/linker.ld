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
  /* OCRAM2 (512 kB, 0x20200000..0x2027FFFF). The vendor loader unpacks the
   * stock's 0x5AA0-byte OCRAM data to 0x20200000 at boot (its load table
   * entry 3, docs/FIRMWARE_BRINGUP.md); nothing of ours reads it. We start
   * above it. Not kept over a warm reset (recovery.c): NOLOAD, code clears
   * what it uses (delay_init); .ocramtext and .ocramdata are copied at
   * every boot. */
  OCRAM  (rwx) : ORIGIN = 0x20210000, LENGTH = 0x70000
}

SECTIONS
{
  /* App only: cold code, run from OCRAM (docs/FIRMWARE_BRINGUP.md, "Hot and
   * cold code"). The input list is COLD_SRC in the Makefile, generated into
   * cold.ld (empty for recovery). Code and const data of those files, loaded
   * from the slot data blob (F:0x41000, see .ocramdata below) and copied by
   * stage2_main, which also makes it visible to instruction fetches. First in
   * SECTIONS: the first matching pattern wins, so .blob's *(.text*) does not
   * take these files. Calls between ITCM and OCRAM go through linker veneers
   * (out of BL range). */
  .ocramtext : AT(0x60041000) {
    . = ALIGN(32);
    __ocramtext_start__ = .;
    INCLUDE cold.ld
    . = ALIGN(32);
    __ocramtext_end__ = .;
  } > OCRAM
  __ocramtext_load__ = LOADADDR(.ocramtext);

  .vectors 0x0 : { KEEP(*(.vectors)) } > ITCM AT> FLASH

  .blob 0x400 : AT(0x600107d4) {
    . += 0xd6;
    KEEP(*(.stage2))
    *(.text*)
    *(EXCLUDE_FILE(*CommonTables.c.o) .rodata*)
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

  /* App only: large const tables (CMSIS tables) are copied from flash to
   * RAM by stage2_main: reading them from flash at run time would stall the
   * audio during flash writes. Load image in flash right after the app
   * slot's ITCM image (F:0x41000, below the stock data at 0x61000;
   * src/dsp/stock_data.h), one blob that the app slot header carries with
   * its length and CRC (recovery.h): .ocramtext, .ocramdata, .dtcmdata.
   * .ocramdata: the 512-point rfft tables of the cab's long-IR tail
   * (dsp/conv2.c, ~4.9 kB). DTCM and ITCM have no room for them; the tail's
   * own buffers are in OCRAM too. Listed first: first match wins. */
  .ocramdata : AT(LOADADDR(.ocramtext) + SIZEOF(.ocramtext)) {
    . = ALIGN(4);
    __ocramdata_start__ = .;
    *CommonTables.c.o(.rodata.twiddleCoef_256 .rodata.twiddleCoef_rfft_512
                      .rodata.armBitRevIndexTable256 .rodata.arm_cfft_sR_f32_len256)
    . = ALIGN(4);
    __ocramdata_end__ = .;
  } > OCRAM
  __ocramdata_load__ = LOADADDR(.ocramdata);

  /* the other CommonTables: DTCM */
  .dtcmdata : AT(LOADADDR(.ocramdata) + SIZEOF(.ocramdata)) {
    . = ALIGN(4);
    __dtcmdata_start__ = .;
    *CommonTables.c.o(.rodata*)
    . = ALIGN(4);
    __dtcmdata_end__ = .;
  } > DTCM
  __dtcmdata_load__ = LOADADDR(.dtcmdata);

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start__ = .;
    /* TinyUSB's 2 kB-aligned queue heads first, where the section's own
     * alignment pads anyway: later in .bss they cost up to 2 kB of padding */
    *(.bss._dcd_data)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > DTCM

  /* big buffers that do not fit in DTCM (the delay line) */
  .ocram (NOLOAD) : {
    . = ALIGN(4);
    __ocram_start__ = .;
    *(.ocram*)
    . = ALIGN(4);
    __ocram_end__ = .;
  } > OCRAM

  _estack = 0x20058000;

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
