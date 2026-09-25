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
| v0.1 | Done (tag `v0.1.0`) | transport, protocol, device info, IR list/import/delete/backup, WAV conversion, docs |
| v0.2 | Planned | firmware container tooling: `fw inspect`, `fw extract-block`, `fw patch-string`, plus format/analysis docs |
| v0.3 | Planned | flashing (`fw flash`), recovery docs, stock round-trip and a proof patch verified on hardware |

## Install

Development (library, CLI, and tests):

```bash
git clone https://github.com/w1ne/fb200-tools
cd fb200-tools
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
```

Hardware access additionally needs `hidapi`:

```bash
pip install "fb200-tools[hid]"
```

In an editable checkout, use `.venv/bin/pip install -e ".[dev,hid]"` to get
both. On Linux, raw HID access may require a udev rule or permissions for the
device.

## Usage

```bash
fb200 info                          # versions and USB identity
fb200 ir list                       # all 9 slots: names or (empty)
fb200 ir import 3 my-ircab.wav      # convert and upload to slot 3
fb200 ir import 3 my-ircab.wav --name "My IR"
fb200 ir delete 3                   # erase a slot
fb200 ir backup ./backup            # write manifest.json of slot names
```

Slots are numbered 1–9. IR payloads cannot be downloaded back from the pedal,
so `ir backup` backs up slot names only. For protocol research:

```bash
fb200 probe --listen 2              # print raw HID reports for 2 seconds
fb200 probe --send "aa55010000c8cf" # send a raw frame
```

## Safety

Flashing firmware is **not available yet**. When it lands it will be opt-in:
dry-run by default, writing only with an explicit `--yes`, validating the
image's `FB200` product tag, and saving patches to a new file unless told
otherwise. Flashing carries a real risk of rendering the pedal temporarily or
permanently unusable — use at your own risk. See [`DISCLAIMER.md`](DISCLAIMER.md).

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

This project is an independent, community reverse-engineering effort. It is not
affiliated with, endorsed by, or supported by FLAMMA Innovation or MOOER Audio.
All product names and trademarks belong to their respective owners and are used
here only to describe interoperability.
