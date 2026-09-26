/* Boot header consumed by the stock bootloader: image[0] = initial SP,
 * image[1] = absolute (thumb) reset stub address. */
    .syntax unified
    .section .boot_header,"a",%progbits
    .word 0x20058000
    .word reset_stub + 1
