/* Vendor loader's fixed entry point: it jumps to ITCM 0x4d6 after loading the
 * blob (docs/FIRMWARE_BRINGUP.md). ITCM 0x4d6 is only 2-byte aligned, so this
 * stub sits exactly there and branches to the C body. */
    .syntax unified
    .section .stage2,"ax",%progbits
    .p2align 1
    .thumb_func
    .global stage2
stage2:
    b.w     stage2_main
