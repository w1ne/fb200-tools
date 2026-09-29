# Battery operation and power

What the stock firmware does for power, what ours does, the expected savings,
and how to measure them on the pedal.

## 1. Stock firmware

- Core at 600 MHz (ARM PLL, DIV_SELECT 100, VDD_SOC raised by DCDC REG3.TRG;
  [LABWIRED.md](LABWIRED.md) stage 2). The main loop is a busy loop: the
  stock image has no `WFI` (checked: the four `0xBF30` halfwords in its ITCM
  image are data, not code).
- No auto power-off ([STOCK_FEATURES.md](STOCK_FEATURES.md) §5), no idle
  dimming. The light rings are capped at 25 % (every byte >> 2).
- Battery: 2000 mAh, about 6 h (manual): about 330 mA average.
  Four levels from ADC1 IN9 (thresholds 0xE74, 0xE10, 0xD98, 0xC80), a
  blinking red status LED at level 0. The power switch is a hard cut.

## 2. Our firmware

| Area | What | Default | Code |
| --- | --- | --- | --- |
| CPU idle | the main loop sleeps (`WFI`) when no audio block and no USB event wait; any interrupt wakes it (SAI/eDMA every 0.73 ms, USB, UART, SysTick 1 ms). CCM CLPCR LPM = RUN: only the core sleeps, every clock runs | on | `src/cpu_idle.c`, `src/main.c` |
| Loop busy % | DWT: awake cycles (interrupts included) over elapsed time | - | `cpu` |
| Core clock | 600 / 528 (PLL2) / 396 MHz (PLL2 PFD2); 528 and 396 lower VDD_SOC to 1.225 V and power down the ARM PLL. Audio (PLL4), USB (PLL3), UART, FlexIO, I2C, ADC clocks do not change | 600, not saved | `power clock` |
| Unused blocks | at boot: USB2 PHY and its PLL off (the pedal uses USB1), video PLL off, clock gates off for peripherals no code uses (CAN, LPSPI, ENET, CSI, LCDIF, PXP, PWM, QTimer, ENC, ACMP, AOI, EWM, KPP, SPDIF, FlexSPI2, FlexIO1, SAI2, ADC2, LPUART2-4/6-8) | on | `ui/power.c` |
| LED level | display and knob LEDs lit 3, 2 or 1 ms of each 3 ms multiplex slot; the light rings scaled by the same % | 100 % | `power led` |
| Idle standby | after N minutes without footswitch, knob, app edit or input signal (> about -50 dBFS), and not charging: rings, knob LEDs and display dark (a slow dot blinks); audio keeps running; any activity wakes it | off | `power idle` |
| Gauge | IIR filter (2 s), stock thresholds with hysteresis (no level flapping under load), an mV and % estimate | - | `ui/power_logic.c` |
| Low battery | level 0: "LOb" on the display for 2 s, again every 5 min (and the stock blinking red LED) | on | |
| Critical battery | 30 s below about 3.35 V (raw 3118), not charging: settings saved, flash writes refused (a brown-out in a sector erase loses the sector), panel dark with "LOb" blinking, audio keeps running. Leaves at about 3.5 V or on the charger | on | |

Settings `led` and `idle` are saved at F:0x80100 (marker `PW`, after the
stock settings in the same sector; [UI_AND_STORAGE.md](UI_AND_STORAGE.md) §5).
`clock` and `sleep` are not saved: every boot starts at 600 MHz, sleep on.

The battery mV and % are estimates: the ADC divider is not measured
(`BATT_RAW_PER_V` in `ui/power_logic.h`, 930 counts per V assumed: it puts
the stock thresholds at 3.98 / 3.87 / 3.74 / 3.44 V).

Console:

```
power                       state, battery, settings
power sleep on|off          WFI in the main loop (runtime)
power clock 600|528|396     core clock (runtime)
power led 100|66|33         display, knob LED and ring brightness (saved)
power idle <min>            idle standby after <min> minutes, 0 = off (saved)
power log <s>               a battery line every <s> seconds, 0 = off
cpu                         engine load + loop busy % since the last `cpu`
clocks                      CCGR gates
```

## 3. Expected savings (estimates, to be measured)

Data sheet class numbers for the i.MX RT1050/1060 at 3.3 V; the LED numbers
are typical parts. The pedal's rails and LED resistors are not measured.

| Change | Estimate from the battery | How to check |
| --- | --- | --- |
| WFI (busy ~20 % instead of 100 %) | 40-60 mA | `power sleep off` vs `on`, meter |
| 528 MHz at 1.225 V (with WFI) | 5-10 mA | `power clock 528` |
| 396 MHz (with WFI) | 8-15 mA | `power clock 396` |
| Unused PLL, PHY, clock gates | 3-8 mA | compare with the 0.9.1 image |
| `power led 33` | 25-70 mA (depends on the lit LEDs and the ring colour) | meter |
| Standby (panel dark) | 40-120 mA | `power idle 1`, wait |

WFI + `led 66` could take the average from about 330 mA to about 250 mA
(6 h to about 8 h). WS2812 LEDs draw about 0.5-1 mA each even when dark (40
of them): only a supply switch would remove that, and none is known.

Not changed: the codec (the stock init table replayed; the NAU88L21 is a
low-power part and the stock never touches it after init), the Bluetooth
module (`AT+B500` turns only its audio off; no power pin is known), unused
pads (the board wiring is not known).

## 4. Hardware checklist

Console: `fb200 console <commands>`. Audio must stay glitch-free: before and
after each step, `stats` (skips) and `sai` (`unf`) must not grow over a
minute while you play.

1. Flash the slot, boot, `power`: `sleep on, clock 600 MHz, VDD_SOC 1275 mV,
   led 100%, idle 0 min (off)`. Check `clocks`: the gated peripherals are gone
   from the "(clocked)" list.
2. Load: play for a minute, then `cpu` twice (the first call resets the
   window). Expected: `engine block avg ~14% / max ~21%`, `loop busy`
   about 15-25 %, about 4000-6000 wakes/s (SAI1 RX + TX and the
   Bluetooth SAI3: 1378/s each, SysTick 1000/s, USB). `power sleep off`, wait, `cpu`
   twice: `loop busy 100.0%`. `power sleep on`.
3. Glitches: `stats`, `sai`, play 60 s, `stats`, `sai`: skips and `unf`
   unchanged. Same with a USB playback running (`usb mix`) and while
   saving a preset (hold a footswitch).
4. Current. Best: a multimeter (mA range) in series with the battery. A USB
   power meter between the charger and the pedal shows the differences only
   with a full battery (charge current near 0) and only if the charger feeds
   the load: check that `power sleep off` / `on` moves it. Read each for 30 s:
   - `power sleep off` (as 0.9.1) vs `power sleep on`: expect 40-60 mA less.
   - `power clock 528`, then `power clock 396`: expect a few mA less each;
     `cpu` twice: engine max about 24 % / 32 %; step 3 again. Back:
     `power clock 600` (VDD_SOC back to 1275 mV).
   - `power led 66`, `power led 33`: less; the display and knob LEDs must
     not flicker. Back to your choice.
   - `power idle 1`, do not touch the pedal and do not play for 1 min: the
     panel goes dark, a dot blinks; the meter drops. Touch a knob: all back.
     Play a note: back. `power idle 0` (or keep a value).
5. Gauge calibration: measure the battery voltage with a multimeter while
   `power` runs; send `raw` and `mV` pairs (full, half, near empty):
   `BATT_RAW_PER_V = raw * 1000 / mV`.
6. Drain log (optional, one battery run): `power log 60`, log the console to
   a file until the pedal switches off. It gives the real curve for the %
   table, the time to "LOb", and the voltage where the pedal dies (the
   critical threshold must be above it).
7. Charging: plug the charger: the status LED as before, `power` shows
   `charging=1`, standby and critical both end.
