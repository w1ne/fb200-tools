![banner](images/banner.svg)

# fb200-tools

Open-source Python library and CLI for the **FLAMMA FB200** (Mooer-based) bass
multi-effects pedal. It talks to the pedal's vendor USB HID interface directly
from macOS, Linux, or Windows: read device and firmware versions, list, import,
delete, and back up IR (impulse response) slots, and convert WAV files into the
pedal's IR format. Runtime code needs only the Python standard library;
`hidapi` is required only when talking to real hardware. The USB HID protocol
is documented in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Features

- `fb200 info` — product, application, firmware, Bluetooth, and hardware versions.
- IR management: list all 9 slots, import WAV files, delete slots, and write a
  backup manifest.
- WAV → IR conversion matching the official editor: 44.1 kHz, channel 0,
  1024 `float32` samples.
- Mock transport so the full test suite runs without a pedal.
- `fb200 probe` — raw frame research tool for protocol exploration.
- Protocol documentation derived from the official app and verified on hardware.

## Status

| Version | Status | Contents |
|---------|--------|----------|
| v0.1 | Done | device info, IR list/import/delete/backup, docs |
| v0.2 | Done | firmware container tooling: `fw inspect`, `fw extract-block`, `fw patch-string`, plus format/analysis docs |
| v0.3 (current) | In progress | flashing (`fw flash`), recovery docs, stock round-trip and a hardware-verified proof patch (firmware version string) |

## Install

Requires Python 3.10 or newer.

Development (library, CLI, and tests):

```bash
git clone https://github.com/w1ne/fb200-tools
cd fb200-tools
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
```

Hardware access additionally needs `hidapi`; from a clone:

```bash
.venv/bin/pip install -e ".[hid]"
```

Use `.venv/bin/pip install -e ".[dev,hid]"` to get both. The package is not on
PyPI yet; `pip install "fb200-tools[hid]"` will be the install command once it
is published. On Linux, raw HID access may require a udev rule or permissions
for the device.

On Windows use `.venv\Scripts\pip` and `.venv\Scripts\fb200`, or activate the
virtual environment first (`source .venv/bin/activate` on macOS/Linux,
`.venv\Scripts\activate` on Windows).

## Usage

After activating the venv (or prefixing commands with `.venv/bin/` on
macOS/Linux), run:

```bash
fb200 info                          # versions and USB identity
fb200 ir list                       # all 9 slots: names or (empty)
fb200 ir import 3 my-ircab.wav      # convert and upload to slot 3 (replaces it)
fb200 ir import 3 my-ircab.wav --name "My IR"
fb200 ir delete 3                   # erase a slot
fb200 ir backup ./backup            # write manifest.json of slot names
```

Slots are numbered 1–9. Importing into an occupied slot **replaces** its
contents, and IR payloads cannot be downloaded back from the pedal, so there is
no undo; `ir backup` backs up slot names only. For protocol research:

```bash
fb200 probe --listen 2              # print raw HID reports for 2 seconds
fb200 probe --send "aa55010000c8cf" # send a raw frame
```

![fb200-tools protocol stack: the CLI and library layers (`pedal.py`/`updater.py` request/response and flash orchestration, `protocol.py` frames `AA 55 | len u16le | fn | data | CRC16`, `transport.py` HID reports of 64 B whose first byte is the valid length) down to the pedal over USB](images/protocol-stack.svg)

## Safety

Flashing firmware is opt-in: dry-run by default, writing only with an explicit
`--yes`, validating the image's `FB200` product tag, and saving patches to a
new file unless told otherwise. Flashing carries a real risk of rendering the
pedal temporarily or permanently unusable — use at your own risk. See
[`DISCLAIMER.md`](DISCLAIMER.md).

![FB200 .mr firmware container layout: a 128-byte header carrying the Mooer_TAG magic, the FB200 product tag, SEND_CMD/REC_CMD, the block count, UPDATE_ADDR and VERSION; then block 0 (200,704-byte application, fn=0x04) and block 1 (3,286,016-byte models, fn=0x06), each preceded by its own 512-byte tag whose START_PAGE is sent as u16 big-endian in write frames; page size 512 B](images/firmware-layout.svg)

## Documentation

- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — USB identities, HID report framing,
  CRC16, command set, IR format, and the firmware update mode.
- [`DISCLAIMER.md`](DISCLAIMER.md) — risk and affiliation notice.

## Credits

- [wattsline/Flamma-FF20](https://github.com/wattsline/Flamma-FF20) — protocol
  research for the sibling Flamma FF20; cross-checked framing and CRC.
- [ThijsWithaar/MooerManager](https://github.com/ThijsWithaar/MooerManager) —
  USB control of Mooer pedals.
- [shpala/MooerLooperManager](https://github.com/shpala/MooerLooperManager) —
  manager for Mooer GL100/GL200 loopers.
- [utajum/mooer-drummer-x2](https://github.com/utajum/mooer-drummer-x2) —
  reverse-engineered Mooer Drummer X2 HID protocol.

## License

MIT — see [`LICENSE`](LICENSE).

All images are original works, licensed with the project (MIT).

This project is an independent, community reverse-engineering effort. It is not
affiliated with, endorsed by, or supported by FLAMMA Innovation or MOOER Audio.
All product names and trademarks belong to their respective owners and are used
here only to describe interoperability.
