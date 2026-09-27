# FB200 Boot Chain and the DFU Handover

Reverse-engineered 2026-09-27 by dumping the bootloader region over the USB
console (`dumpmem`) from the running open firmware. This is how the pedal
boots, how it decides between the application and update (DFU) mode, and why
the handover into the vendor DFU failed, and the USB self-update that
replaces it.

## 1. Flash layout (verified by dump)

| Address | Contents |
|---------|----------|
| `0x60000000` | FlexSPI Configuration Block (`FCFB`, 512 B, lookup table) |
| `0x60001000` | IVT (header `0x412000D1`, entry `0x60002000`, DCD `0x60001030`, boot data `0x60001020`) |
| `0x60002000` | Bootloader vector table (SP `0x20050000`, reset `0x600024D1`) |
| `0x600024D0` | Bootloader reset handler: VTOR `0x60002000`, SP from `[0x60002000]`, FlexRAM GPRs (`0x00AA0000` / `0x00200007` / **`0xFFEAAAA9`**), SystemInit `0x600027E8`, then jumps to `0x60002400` |
| `0x60002400` | Bootloader main: the **same self-loading loader format** as the app (table + LZ decompressor + memcpy + memset) |
| `0x60008000`+ | Update/DFU code (USB descriptors for `0483:5703` at `0x6000AF40`), executed after the loader runs |
| `0x60010000` | Application image (the `.mr` block 0) |
| `0x60041000` | Model library (block 1) |
| `0x60086000` | Handover flag byte (see below) |

Note: the bootloader configures FlexRAM with `0xFFEAAAA9`; the stock
application reconfigures it with `0xFFAAAAA9` in its own reset stub.

## 2. The DFU decision block (`0x60008D50`–`0x60008D84`)

```
flag  = *(0x60086000)                      ; 0xFF = erased = "update allowed"
pin_a = read_pin(GPIO3, 12)                ; GPIO3_IO12 (pad GPIO_SD_B0_00)
pin_d = read_pin(GPIO2, 24)                ; GPIO2_IO24 (pad GPIO_B1_08)
if (pin_a || pin_d) {                      ; active high
    if (*(0x600CF000) == *(0x20010174) &&  ; flash == RAM handshake slot 1
        *(0x60003004) == *(0x2001051C) &&  ; flash == RAM handshake slot 2
        flag == 0xFF)
        enter_update_mode();               ; 0x6000333C (DFU: 0483:5703)
}
boot_application();
```

`read_pin` returns the raw GPIO `DR` bit (active high). `0x6000333C` is the
only DFU entry point in the bootloader, and this block is its only caller.

Interpretation: the two flash↔RAM equalities are a runtime handshake — the
application (or the updater) copies the two flash words into the RAM slots
before resetting, so only a cooperating firmware can trigger DFU. The flag
byte is simply erased flash (`0xFF`), so it does not gate a normal unit.

## 3. Handover into the vendor DFU: NOT solved (parked)

Four attempts failed on hardware (2026-09-27):

1. RAM handshake + driving `GPIO_B1_08` high + `NVIC_SystemReset()`: the
   bootloader boots the application anyway.
2. Jump to `0x6000333C`: this is the boot-the-app trampoline, not DFU.
3. Write the flag byte at `0x60086000` over FlexSPI IP commands: the write
   failed (no write-enable sequence; and the decision block wants `0xFF`
   there anyway).
4. Call the updater entry `0x600091BC` directly: the pedal hangs (it needs
   the bootloader's runtime state) and drops off USB.

These commands are removed from the console.

## 4. USB self-update (replaces the handover)

The open firmware rewrites its own application region, so the vendor DFU is
not needed for development. `firmware/audio/src/debug/selfupdate.c` uses the
RT1062 ROM FlexSPI NOR driver (ROM API tree at `0x0020001C`) with the FCB
copied from `0x60000000`. All firmware code runs from ITCM, so it can erase
and program flash while it runs.

- Region: `0x60010000..0x60041000` (block 0) only. The bootloader and the
  model library are never touched, so A+D stays the recovery path.
- Console: `fwinfo`, `crc <addr> <len>`, `fwbegin <len> <crc32>` + raw
  stream, then `reset`.
- Host: `firmware/tools/usb_update.py IMAGE.mr`. It first checks that flash
  at `0x60010400` holds the vendor boot region of the image (proves the
  mapping), then erases, streams, verifies the CRC32 read back from flash,
  and resets.
- Every image flashed this way must contain `selfupdate.c`, or the next
  update needs A+D again.

## 5. Practical notes

- After flashing, the updater's exit jumps straight into the image; no power
  cycle is needed.
- A+D at power-on still works as the recovery path (same decision block).
- The bootloader region is read-only from the application's point of view;
  the flag byte at `0x60086000` is in flash and was already `0xFF`.
- `src` (console) reads `SRC_SRSR`/`SBMR1`/`SBMR2` for reset-cause debugging.
