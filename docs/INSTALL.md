# Installing the open FB200 firmware

This guide covers building the images, the one-time install, everyday updates over USB,
recovery, and going back to the stock firmware.

> **Read [`DISCLAIMER.md`](../DISCLAIMER.md) first.** Flashing custom firmware is at your
> own risk. The procedure below is tested on one pedal (stock firmware V1.0.1, hardware
> rev A). The vendor bootloader and your presets are never overwritten, and holding
> **A+D at power-on** always gets you back to the vendor updater.

## What you need

- A FLAMMA FB200 and a USB cable.
- **The official FB200 firmware file (`.mr`)**, as distributed by FLAMMA for the
  official updater (tested: V1.0.1). The build extracts the vendor bootloader stub and
  the stock sound data (amp models, cab IRs, tone stack, drum rhythms) from **your
  copy** of this file. This repository ships none of it, and the images you build
  contain it, so **do not redistribute the built images**.
- macOS or Linux. Windows is not supported yet for the USB console and updates.
- Python 3.10+ with this package, a Python with [`unicorn`](https://www.unicorn-engine.org/)
  (used at build time to unpack the stock image), `make`, `curl`, `git`.
- An Arm GNU toolchain **with newlib**:
  - Debian/Ubuntu: `sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi`
  - macOS: the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
    (`arm-none-eabi`, AArch64 or x86_64 host). Homebrew's `arm-none-eabi-gcc` has no
    C library and will not work.

```bash
git clone https://github.com/w1ne/fb200-tools && cd fb200-tools
python3 -m venv .venv
.venv/bin/pip install -e ".[hid]" unicorn
```

## 1. Build the images

```bash
export CROSS=/path/to/arm-gnu-toolchain/bin/arm-none-eabi-   # if not on PATH
PY_UNICORN=.venv/bin/python firmware/tools/build_images.sh ~/Downloads/FB200_V1.0.1.mr
```

The script builds the recovery and application images. It then runs the whole boot
chain (vendor loader, then recovery, then application) in an emulator and writes
three files to `out/`:

| File | Used for |
| --- | --- |
| `fb200-twostage.mr` | the first install, through the pedal's own update mode |
| `fb200-app.slot` | every later update, over USB |
| `fb200-recovery.bin` | updating the recovery stage (rarely needed) |

## 2. First install (once): update mode + USB

1. Unplug the pedal. **Hold footswitches A and D** while plugging in USB. The pedal
   enters the vendor update mode (USB `0483:5703`).
2. Flash the two-stage image:
   ```bash
   .venv/bin/fb200 fw flash out/fb200-twostage.mr --yes --no-jump
   ```
   The tool reports `device did not re-enumerate` at the end. That is expected: the
   pedal comes back with the new firmware's identity.
3. The pedal now starts in **recovery**, because the update mode cannot write the
   application's data. Install the application over USB:
   ```bash
   .venv/bin/fb200 update app out/fb200-app.slot
   ```
4. The pedal restarts into the firmware. The display shows your current preset
   (for example `P0C`).

## 3. Updating

From now on no button combo is needed:

```bash
.venv/bin/fb200 update app out/fb200-app.slot
```

This works from the running firmware and from recovery. It checks that it is writing
to the right place, erases, writes, verifies a CRC, and restarts. If an update is cut
off, recovery keeps the USB console; run the command again.

## 4. When something goes wrong

- **The firmware crashes or hangs.** Recovery takes over automatically after a fault,
  or after an 8-second watchdog if it hangs. The USB console stays up. To see what
  happened:
  ```bash
  .venv/bin/fb200 console crumbs              # why recovery stayed
  .venv/bin/fb200 crash --elf firmware/audio/build/fb200-app.elf   # symbolized crash dump
  .venv/bin/fb200 console boot                # start the application again
  ```
- **Force recovery:** `fb200 console recovery`.
- **Last resort:** hold **A+D at power-on** and reinstall (section 2), or go back to
  stock (section 5). The vendor bootloader is never modified.

## 5. Back to the stock firmware

Hold **A+D** while plugging in, then:

```bash
.venv/bin/fb200 fw flash ~/Downloads/FB200_V1.0.1.mr --yes --no-jump
```

Your presets, settings and user IRs live in a separate flash area. Neither firmware
overwrites them, so they survive the round trip.

## Using the console

The firmware has a USB serial console (`/dev/cu.usbmodemAUDIO*` on macOS,
`/dev/ttyACM*` on Linux):

```bash
.venv/bin/fb200 console                 # interactive; type `help`
.venv/bin/fb200 console preset cpu      # run commands and print the output
```

Useful commands:

| Command | Does |
| --- | --- |
| `preset [n]` | show or select a preset |
| `cpu` | CPU load of the audio chain |
| `tuner on` | turn the tuner on |
| `drums on` | start the drum machine |
| `power` | battery and charger state |
| `ui` / `uimon on` | knob and footswitch state, live |
| `tin sine 110` | send a test tone into the effects chain |
| `crumbs` / `crashdump` | crash diagnostics |
| `recovery` / `boot` / `reset` | control which stage runs |

## Linux permissions

For USB access without root, add a udev rule:

```
# /etc/udev/rules.d/70-fb200.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="34db", ATTR{idProduct}=="800f", MODE="0666"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="34db", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="5703", MODE="0666"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0483", MODE="0666"
```

Then run `sudo udevadm control --reload && sudo udevadm trigger`. The console tty
needs membership of the `dialout` (or `uucp`) group.
