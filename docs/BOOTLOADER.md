# FB200 Boot Chain and the DFU Handover

Reverse-engineered 2026-09-27 by dumping the bootloader region over the USB
console (`dumpmem`) from the running open firmware. This is how the pedal
boots, how it decides between the application and update (DFU) mode, and how
our firmware performs the same handover so flashing never needs A+D.

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

## 3. Handover recipe (implemented in `firmware/audio`)

The stock app's `0xC1` command performs this handover; our firmware does the
same in the `handover` console command:

1. `*(0x20010174) = *(0x600CF000)`, `*(0x2001051C) = *(0x60003004)`.
2. Drive the update pin high: pad `GPIO_B1_08` (ALT5 = GPIO2_IO24),
   `GDIR` output, `DR_SET` bit 24.
3. USB disconnect, short delay, `NVIC_SystemReset()`.

The bootloader then takes the DFU path, enumerates as `0483:5703`, and
`fb200 fw flash ... --no-jump` works. A plain `NVIC_SystemReset()` without
the handshake boots the application again, which is how the earlier attempts
failed.

## 4. Practical notes

- After flashing, the updater's exit jumps straight into the image; no power
  cycle is needed.
- A+D at power-on still works as the recovery path (same decision block).
- The bootloader region is read-only from the application's point of view;
  the flag byte at `0x60086000` is in flash and was already `0xFF`.
- `src` (console) reads `SRC_SRSR`/`SBMR1`/`SBMR2` for reset-cause debugging.
