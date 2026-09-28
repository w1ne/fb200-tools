/* fb200-audio: vendor image format.
 *   block0 0x000..0x400   vector table (copied to ITCM 0x0 by stage2)
 *   block0 0x400..0x7D4   vendor boot region (supplied by the packer)
 *   block0 0x7D4..0x1E39C ITCM payload (entry 0 memcpy -> ITCM 0x400)
 *   entry 4 memset range 0x20018B44..0x2004E3E8 = our .bss
 * Entry point: ITCM 0x4D6 (stage2.S). See docs/FIRMWARE_BRINGUP.md.
 *
 * RAM is what the pedal has (measured 2026-09-28): FlexRAM 512 kB,
 * IOMUXC_GPR17 = 0xFFAAAAA9 = 4 banks ITCM (128 kB), 11 banks DTCM
 * (0x20000000..0x20057FFF), 1 bank OCRAM (0x20200000..0x20207FFF). Nothing
 * answers at 0x20208000 and above. Memory budget: docs/FIRMWARE_BRINGUP.md,
 * "Memory map". */

ENTRY(stage2)

MEMORY
{
  ITCM    (rwx) : ORIGIN = 0x00000000, LENGTH = 0x20000
  FLASH   (rx)  : ORIGIN = 0x60010000, LENGTH = 0x31000
  /* App only: the slot data area, F:0x41000..0x61000 (SLOT_DATA, recovery.h).
   * Cold code runs from here in place (XIP, linker .xiptext). */
  XIP     (rx)  : ORIGIN = 0x60041000, LENGTH = 0x20000
  /* Low DTCM, below the vendor loader's .bss range. In our images the loader
   * table entries 1-3 are .bss memsets (src/fb200/images.py), so nothing
   * loads stock data here; the vendor bootloader (F:0x2000) uses
   * 0x20010000..0x20012F0C at every boot (its load table), before we run.
   * App only, NOLOAD, zeroed by stage2_main. The crash dump sits above it
   * (DUMP_ADDR, recovery.c: 0x20018A00, survives a warm reset). */
  DTCM_LO (rw)  : ORIGIN = 0x20000000, LENGTH = 0x18A00
  DTCM    (rw)  : ORIGIN = 0x20018B44, LENGTH = 0x3F4BC
  /* OCRAM: 32 kB, 0x20200000..0x20207FFF, and nothing above (the header).
   * Measured on the pedal (docs/FIRMWARE_BRINGUP.md, "Memory map"): at
   * 0x20208000.. data writes are dropped and reads return 0, with no fault.
   * The stock's loader table unpacks 0x5AA0 bytes of stock data to
   * 0x20200000 (entry 3); our images turn that entry into the .bss memset
   * (src/fb200/images.py) and nothing of ours reads it: the bank is ours.
   * Not kept over a warm reset (recovery.c): .ocram is NOLOAD and zeroed by
   * stage2_main; .ocramdata is copied at every boot. */
  OCRAM   (rw)  : ORIGIN = 0x20200000, LENGTH = 0x8000
}

/* Stack: top of DTCM, down to __stack_limit__. Measured high-water on the
 * pedal (v0.8.0, recovery + app): under 0x200 B. */
_estack = 0x20058000;
__stack_limit__ = _estack - 0x2000;

SECTIONS
{
  /* App only: cold code, run in place from flash (XIP). The input list is
   * COLD_SRC in the Makefile, generated into cold.ld (empty for recovery).
   * First in SECTIONS: the first matching pattern wins, so .blob's *(.text*)
   * does not take these files. The vendor stub runs from flash too; the
   * FlexSPI AHB read path is cached and prefetched (AHBCR 0x78) and the M7
   * I-cache/D-cache cover it (MPU off: default map, Normal WBWA).
   * Rule: nothing that runs while the flash is busy may be here
   * (firmware/tools/hot_path.py, flash-write roots). */
  .xiptext 0x60041000 : AT(0x60041000) {
    __xiptext_start__ = .;
    INCLUDE cold.ld
    . = ALIGN(32);
    __xiptext_end__ = .;
  } > XIP
  __xiptext_load__ = LOADADDR(.xiptext);

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
  /* The app is loaded by recovery's copier (src/debug/recovery.c), which
   * runs at ITCM 0x1F000: slot_valid() refuses a blob above it. (Recovery
   * itself is loaded by the vendor loader: 0x1DBC8 B, tests.) */
  ASSERT(__blob_end__ <= 0x1F000, "ITCM image overlaps the copier at 0x1F000")

  /* App only: CPU-only buffers in the ITCM above the code, NOLOAD, zeroed by
   * stage2_main. May cover the copier's 0x1F000..: the copier is done by
   * then. */
  .itcm_bss __blob_end__ (NOLOAD) : {
    . = ALIGN(8);
    __itcm_bss_start__ = .;
    *(.itcm_bss*)
    . = ALIGN(4);
    __itcm_bss_end__ = .;
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
   * RAM by stage2_main: the audio path must not read flash. Load image in
   * the slot data blob right after .xiptext (F:0x41000, below the stock data
   * at 0x61000; src/dsp/stock_data.h), one blob that the app slot header
   * carries with its length and CRC (recovery.h): .xiptext, .ocramdata,
   * .dtcmdata. .ocramdata: the 512-point rfft tables of the cab's long-IR
   * tail (dsp/conv2.c, ~4.9 kB). Listed first: first match wins. */
  .ocramdata : AT(LOADADDR(.xiptext) + SIZEOF(.xiptext)) {
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
  /* recovery's copier (and the vendor loader) zero exactly this range */
  ASSERT(__bss_end__ <= 0x2004E3E8, ".bss beyond the copier's memset range")

  /* App only: DTCM between .bss and the stack, NOLOAD, zeroed by
   * stage2_main. */
  .dtcm_hi (NOLOAD) : {
    . = ALIGN(4);
    __dtcm_hi_start__ = .;
    *(.dtcm_hi*)
    . = ALIGN(4);
    __dtcm_hi_end__ = .;
  } > DTCM
  ASSERT(__dtcm_hi_end__ <= __stack_limit__, "DTCM data runs into the stack reserve")

  .dtcm_lo (NOLOAD) : {
    __dtcm_lo_start__ = .;
    *(.dtcm_lo*)
    . = ALIGN(4);
    __dtcm_lo_end__ = .;
  } > DTCM_LO

  /* OCRAM (32 kB), NOLOAD, zeroed by stage2_main. */
  .ocram (NOLOAD) : {
    . = ALIGN(4);
    __ocram_start__ = .;
    *(.ocram*)
    . = ALIGN(4);
    __ocram_end__ = .;
  } > OCRAM

  /* The region lengths above already stop a link that overflows; this says
   * why, should a region ever be widened by mistake. */
  ASSERT(__ocram_end__ <= 0x20208000 && __ocramdata_end__ <= 0x20208000,
         "OCRAM is 32 kB (0x20200000..0x20207FFF): nothing exists above it")
  ASSERT(__dtcm_lo_end__ <= 0x20018A00 && __stack_limit__ <= 0x20058000,
         "DTCM is 0x20000000..0x20057FFF; the crash dump is at 0x20018A00")

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
