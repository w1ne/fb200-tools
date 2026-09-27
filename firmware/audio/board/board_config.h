/* Board facts for the FB200. Confirm against PCB photos before flashing. */
#pragma once

#define BOARD_XTAL_HZ 24000000u
/* The stock identity, so the official editor and `fb200` find the pedal
 * (docs/PROTOCOL.md §1). The serial stays ours: it names the console tty. */
#define AUDIO_USB_VID 0x34DB
#define AUDIO_USB_PID 0x800F
#define AUDIO_USB_MANUFACTURER "fb200-tools"
#define AUDIO_USB_PRODUCT "FB200 Audio"
#define AUDIO_USB_SERIAL "AUDIO_0001"
