#ifndef FB200_COLD_H
#define FB200_COLD_H
/* COLD: one function runs in place from flash (linker .xiptext), like the
 * files in COLD_SRC (Makefile). Only for code that nothing on the audio
 * path, in an ISR or during a flash write reaches: firmware/tools/hot_path.py
 * fails the build test otherwise. Empty for recovery (no XIP code) and for
 * host builds. */
#if defined(__arm__) && !defined(FB200_RECOVERY)
#define COLD __attribute__((section(".xiptext.fn"), noinline))
#else
#define COLD
#endif
#endif
