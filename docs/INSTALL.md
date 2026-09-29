# Installing the open FB200 firmware

This guide covers the first install, updates, recovery, going back to the stock
firmware, and building from source.

> **Read [`DISCLAIMER.md`](../DISCLAIMER.md) first.** Flashing custom firmware is at your
> own risk. The procedure is tested on one pedal (stock firmware V1.0.1, hardware
> rev A). The vendor bootloader and your presets are never overwritten, and holding
> **A+D at power-on** always gets you back to the vendor updater.

## The easy way: in the browser

Open **<https://shylenko.com/fb200-tools/flash.html>** in Chrome or Edge on Windows,
macOS, Linux or ChromeOS. Nothing to install. The page takes the latest release and
guides you step by step. Safari and Firefox cannot talk to USB devices.

The rest of this guide does the same from the command line.

## What you need

- A FLAMMA FB200 and a USB data cable.
- For the first install only: **the official FB200 firmware file (`.mr`)**, as
  distributed by FLAMMA for its updater (tested: V1.0.1). It supplies the vendor
  loader and the stock sound data (amp models, cab IRs, tone stack, drum rhythms).
  The released images contain none of it. The files made from your `.mr` contain it,
  so **do not share them**.
- Python 3.10+ and this package:
  ```bash
  python3 -m pip install "fb200-tools[hid] @ git+https://github.com/w1ne/fb200-tools"
  ```
  The USB console and updates work on macOS and Linux. On Windows, use the browser.

## 1. First install (once)

The pedal runs the stock firmware. The first install goes through the pedal's own
update mode, then over USB.

1. Make the install image from your `.mr` and the latest release:
   ```bash
   fb200 fw twostage ~/Downloads/FB200_V1.0.1.mr -o fb200-twostage.mr
   ```
2. Unplug the pedal. **Hold footswitches A and D** while you plug in USB. The pedal
   enters update mode (USB `0483:5703`). Then:
   ```bash
   fb200 fw flash fb200-twostage.mr --yes --no-jump
   ```
   The tool reports `device did not re-enumerate` at the end. That is expected: the
   pedal comes back with the new firmware's identity.
3. The pedal now starts in **recovery**. Install the application and the sound data:
   ```bash
   fb200 update app latest
   fb200 update stock ~/Downloads/FB200_V1.0.1.mr
   ```
4. The pedal restarts into the firmware. The display shows your current preset
   (for example `P0C`).

## 2. Updating

No button combo and no `.mr` file:

```bash
fb200 update app latest
```

This downloads the latest release, checks it against the release manifest, and
writes it over USB. It works from the running firmware and from recovery. The pedal
checks what it writes (CRC) and restarts. If an update is cut off, recovery keeps the
USB console: run the command again. The sound data stays in its own flash area.

To check the sound data: `fb200 console stock` must print `flash ok, in use`. If it
does not, run `fb200 update stock FB200_V1.0.1.mr` again.

## 3. When something goes wrong

- **The firmware crashes or hangs.** Recovery takes over automatically after a fault,
  or after an 8-second watchdog if it hangs. The USB console stays up. To see what
  happened:
  ```bash
  fb200 console crumbs              # why recovery stayed
  fb200 crash --elf fb200-app.elf   # symbolized crash dump (ELF from your build)
  fb200 console boot                # start the application again
  ```
- **Force recovery:** `fb200 console recovery`.
- **Last resort:** hold **A+D at power-on** and reinstall (section 1), or go back to
  stock (section 4). The vendor bootloader is never modified.

## 4. Back to the stock firmware

Hold **A+D** while plugging in, then:

```bash
fb200 fw flash ~/Downloads/FB200_V1.0.1.mr --yes --no-jump
```

Your presets, settings and user IRs live in a separate flash area. Neither firmware
overwrites them, so they survive the round trip.

## Using the console

The firmware has a USB serial console (`/dev/cu.usbmodemAUDIO*` on macOS,
`/dev/ttyACM*` on Linux):

```bash
fb200 console                 # interactive; type `help`
fb200 console preset cpu      # run commands and print the output
```

Useful commands:

| Command | Does |
| --- | --- |
| `preset [n]` | show or select a preset |
| `cpu` | CPU load of the audio chain and of the main loop |
| `stock` | is the stock sound data present and in use |
| `tuner on` | turn the tuner on |
| `drums on` | start the drum machine |
| `power` | battery, charger, sleep, clock, LED level, idle standby ([POWER.md](POWER.md)) |
| `ui` / `uimon on` | knob and footswitch state, live |
| `tin sine 110` | send a test tone into the effects chain |
| `crumbs` / `crashdump` | crash diagnostics |
| `recovery` / `boot` / `reset` | control which stage runs |

## Linux permissions

For USB access without root (browser and command line), add a udev rule:

```
# /etc/udev/rules.d/70-fb200.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="34db", ATTR{idProduct}=="800f", MODE="0666"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="34db", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="5703", MODE="0666"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0483", MODE="0666"
SUBSYSTEM=="tty", ATTRS{idVendor}=="34db", MODE="0666"
```

Then run `sudo udevadm control --reload && sudo udevadm trigger`.

## Building from source

You need an Arm GNU toolchain **with newlib**, `make`, `curl` and `git`:

- Debian/Ubuntu: `sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi`
- macOS: the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  (`arm-none-eabi`). Homebrew's `arm-none-eabi-gcc` has no C library and will not work.

```bash
git clone https://github.com/w1ne/fb200-tools && cd fb200-tools
python3 -m venv .venv && .venv/bin/pip install -e ".[dev,hid]"
export CROSS=/path/to/arm-gnu-toolchain/bin/arm-none-eabi-   # if not on PATH
firmware/tools/build_images.sh                        # the release images, in out/
firmware/tools/build_images.sh ~/Downloads/FB200_V1.0.1.mr   # + your fb200-twostage.mr
```

Then use your files instead of `latest`: `fb200 update app out/fb200-app.slot`. With
`PY_UNICORN` set to a Python that has `unicorn`, the script also runs the whole boot
chain (vendor loader, recovery, application) in an emulator.
