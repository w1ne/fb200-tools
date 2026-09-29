# FB200 Studio (desktop app PoC)

Edit the FB200 like the vendor app does, and ask an AI assistant to do it for
you ("make it brighter", "the low end is muddy", "prepare a slap preset").
The pedal must run the open firmware and be connected over USB.

Status: proof of concept. Tested against a fake pedal and a fake Claude client
only. It has not run against a real pedal yet (see "Verified" below).

## Run

```sh
pip install -e '.[app]'          # from the repo root: fb200 + starlette, uvicorn, anthropic, hidapi, numpy, sounddevice
export ANTHROPIC_API_KEY=...     # or enter the key in Settings (see "API key")
fb200-app                        # or: python -m fb200_app; opens http://127.0.0.1:8200/
```

Options: `--port 8200`, `--console /dev/cu.usbmodemAUDIO1` (the CDC console;
default: auto), `--no-browser`, `--host` (default `127.0.0.1`; the API has no
login, so do not bind it to a shared network).

## Why a local web app and not Tauri

The recommended plan was a Tauri shell that spawns `fb200 mcp` as a sidecar.
This machine has cargo and node but no Tauri CLI; `cargo install tauri-cli`
plus the first app build compiles several hundred crates, and the app would
still need a bundled Python to run the sidecar. The local web app keeps one
stack (Python), imports `fb200` directly, has no build step and is tested by
pytest. It keeps the property that matters from the plan: the MCP server's
`tools/list` is the only tool schema. A Tauri or phone shell can wrap the same
HTTP API later.

## Architecture

```
 browser UI (static/index.html, app.js)          any HTTP client: desktop now,
   editors built from parameter_docs              a phone browser or app later
        |  JSON over HTTP (POST needs header X-FB200-App: 1)
        v
 server.py  Starlette: /api/status /api/docs /api/tools/{name} /api/upload
        |              /api/config  /api/chat (+ /events long poll, /confirm, /stop, /reset)
        |-----------------------------.
        v                              v
 chat.py  ChatRunner                agent.py  AgentSession (Claude tool-use loop)
   background thread, event log,      messages.create(model, tools=tools/list,
   blocks on user confirmations         system=INSTRUCTIONS + app guidance,
        |                               adaptive thinking, prompt caching)
        v                              |
 toolhost.py  ToolHost  <--------------'
   the in-process MCP server (fb200.mcp_server.build_server):
   list_tools() = tools/list, call() = call_tool; needs_confirmation() policy
        |
        v
 fb200.mcp_server.PedalTools  -- one lock --> HID (effects, presets, IRs, settings)
                                            CDC console (delay, EQ, drums, tuner, save)
                                            USB audio (audio_test)
```

No layer below `server.py` knows about HTTP: the agent takes an `emit(event)`
and a `confirm(call) -> bool` callback. A phone client can reuse the same API.

Transport notes: Bluetooth carries control and backing tracks (the phone
plays into the output, not through the effects; the pedal cannot send its
processed sound back over BT). USB carries control and measurement: the pedal
is a class-compliant USB audio device (2 in / 2 out), so a computer (or a
phone on a USB-C OTG cable, not yet tried: docs/PARITY.md) can play and
record, and the agent can measure before/after.

## Features (vendor app parity)

| Feature | Status | Tool(s) |
|---|---|---|
| Connection status, firmware version | done | `pedal_info`, `preset` |
| Preset list (40 names), select | done | `preset_list` (HID `0x96`), `preset` |
| Rename preset | done, confirm | `rename_preset` (HID `0x99`) |
| Save preset | done, confirm | `save_preset` |
| Gate, comp, amp, cab, mod, reverb: every field | done | `set_*` (HID `0x80..0x86`) |
| Delay (open firmware) | done | `set_delay` (console) |
| EQ: HPF, LPF, 5 bands | done | `set_eq` (console) |
| User IR list, import (WAV + process_ir options), delete, use as cab | done, confirm | `ir_list`, `ir_import`, `ir_delete`, `set_cab` |
| Long IRs (slots 20..83, up to 4096 taps): list, import, delete | agent only (no UI panel yet), confirm | `long_ir_list`, `long_ir_import`, `long_ir_delete`, `set_cab` |
| Drum machine on/off, pattern, bpm, level | done | `drums` (console) |
| Tuner readout | done (one reading per click) | `tuner` |
| Output gain / mute | done | `set_output` |
| Global settings: input gain, global cab, BT audio, light ring | done, confirm | `settings` (HID `0xB0`) |
| Module order (`0xA0`) | not done: no effect on the sound (STOCK_FEATURES.md) | - |
| Factory reset | agent only, via console `factory`, confirm | `console` |
| Bluetooth name (`0xB3`), `0xB7`/`0xB8` settings | not done | - |
| Preset import/export files, cloud presets | not done (the vendor cloud cannot be replicated) | - |
| Live tuner stream | not done (poll on click) | - |
| Firmware update | not here: `fb200 update` (no flash tool over MCP) | - |

## Assistant

- Models: `claude-sonnet-5` (default) or `claude-opus-5-5` (Settings or the
  chat model menu). Adaptive thinking, effort `medium`, prompt caching of
  tools + system + history.
- Grounding: the tool `parameter_docs` (also the MCP resource
  `fb200://parameter-docs`) gives every field's range, unit and sound
  meaning, and bass tone recipes. The source is `src/fb200/params.py`, keyed
  by `pedal.MODULES`; a test fails when a MODULES field has no entry. The UI
  builds its sliders and selects from the same docs.
- Each step shows in the chat: thinking summary, tool call and result
  (collapsible). `audio_test` results show the level and an octave-band chart
  (the last two runs: before / latest).
- Confirmation: `save_preset`, `rename_preset`, `ir_import`, `ir_delete`,
  `long_ir_import`, `long_ir_delete`,
  `settings` changes, `crash_dump clear`, and console `save`, `irdel`, `factory`,
  `reset`, `reboot`, `recovery`, `crash`, `hang`, `poke*`, `bt`, `fwtest`
  wait for Allow / Decline in the chat (5 min timeout = declined). The same
  policy (`toolhost.needs_confirmation`) guards the UI: the server refuses
  these calls without `confirmed: true`, which the UI sends only after its
  dialog. Flash streaming is refused by the MCP server itself.

## API key

`ANTHROPIC_API_KEY` wins. Else Settings stores the key in
`~/.config/fb200/app.json` (mode 0600, outside the repo; `XDG_CONFIG_HOME`
moves it). The browser never receives the key, only whether one is set. The
OS keychain is not used yet.

## Verified

Verified (pytest, `tests/test_app.py`, fake pedal from `tests/test_mcp_server.py`
+ HID fakes for `0x96`/`0x99`/`0xB0`/`0x63`/`0x67`, fake Claude client, no
network):

- the tool schema equals the MCP `tools/list`; parameter docs cover every field
- the agent loop: tool calls reach the fake pedal (HID frames checked), all
  results of one turn go back in one message, tool errors go back as
  `is_error`, the step limit, declined and approved confirmations
- HTTP: status, docs, tool calls, 409 without confirmation, the header guard,
  WAV upload checks (path traversal), config never returns the key (file 0600),
  a full chat with a confirmation over the long-poll API
- UI smoke: the page and assets load, `app.js` parses (`node --check`)
- manual: the app served on the fake pedal, a scripted chat run over curl

Not verified:

- anything on a real pedal: preset names over `0x96`, rename over `0x99`
  and the `0xB0` settings write (the open firmware implements them,
  firmware/audio/src/proto/proto.c; the HID fakes follow PROTOCOL.md)
- a real Claude API call (tests use a fake client)
- `audio_test` in the app (needs the pedal's USB audio)
- the UI in a browser by eye (no browser automation was available); only
  the HTTP API was driven
- amp model names vs index (manual order, not checked by ear)
