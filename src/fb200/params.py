"""What every effect parameter does: ranges, units and sound meaning.

One source for the MCP `parameter_docs` tool/resource (the grounded context an
AI agent needs to turn "make it brighter" into parameter values) and for the
app's editors (sliders and selects). The HID blocks follow `pedal.MODULES`
(field order and names); `tests/test_params.py` checks that every MODULES
field has an entry here. Meanings come from the DSP ports in
firmware/audio/src/dsp/*.h and docs/STOCK_FEATURES.md; "no effect" = the
stock and our firmware do not read the field.
"""

from __future__ import annotations

from fb200.pedal import MODULES, SETTINGS_FIELDS

ON = {"min": 0, "max": 1, "unit": "bool", "meaning": "block on (1) / bypassed (0)"}


def _knob(meaning: str, lo: int = 0, hi: int = 100, unit: str = "knob") -> dict:
    return {"min": lo, "max": hi, "unit": unit, "meaning": meaning}


def _select(meaning: str, options: dict[int, str]) -> dict:
    return {"min": min(options), "max": max(options), "unit": "select", "options": options,
            "meaning": meaning}


AMP_MODELS = {1: "Ampog 2OD", 2: "Ampog B18 CL", 3: "Ampog SVT4", 4: "Ampog SVT VALVE",
              5: "Mvrkbass 500", 6: "Mvrkbass 501", 7: "Akuila 750 CL", 8: "Akuila 750 DS",
              9: "Akuila 751", 10: "BASSER CRUNCH"}
CAB_TYPES = {**{i: f"stock cab {i}" for i in range(1, 11)},
             **{10 + s: f"user IR slot {s}" for s in range(1, 10)}}
MOD_TYPES = {0: "Phaser", 1: "Step Phaser", 2: "Flanger", 3: "Jet Flanger", 4: "Tremolo",
             5: "Stutter Tremolo", 6: "Vibrato", 7: "Rotary", 8: "Analog Chorus",
             9: "Multi Chorus", 10: "Ring Mod", 11: "Filter"}
REVERB_TYPES = {0: "Room", 1: "Hall", 2: "Plate", 3: "Spring", 4: "Mod"}

# Blocks set over HID (set_<tool>), in chain order. Fields in MODULES order.
BLOCKS: dict[str, dict] = {
    "gate": {
        "tool": "set_gate", "summary": "Noise gate, first in the chain: a soft downward "
        "expander that closes on quiet signal (hum, string noise between notes).",
        "fields": {
            "enabled": ON,
            "type": _knob("no effect (the stock has one gate)", 0, 4, "index"),
            "threshold": _knob("gate threshold: T = 0.01 t^2 (t = knob/100); higher closes "
                               "on louder signal and can cut note tails; below 10 blends the "
                               "dry signal in"),
        }},
    "comp": {
        "tool": "set_comp", "summary": "Compressor 'CS Comp': evens out the dynamics "
        "(fingerstyle, slap). out = compressed * level * 6.",
        "fields": {
            "enabled": ON,
            "type": _knob("no effect (one compressor type)", 0, 21, "index"),
            "attack": _knob("gain-reduction timing: a 0..2.9 ms look-behind ((100 - attack) "
                            "% of 2.9 ms); lower lets more of the pick/slap transient through"),
            "threshold": _knob("threshold -60..0 dB in ~0.6 dB steps (0 = -60 dB: compresses "
                               "everything; 100 = 0 dB: almost nothing)"),
            "ratio": _knob("ratio 1:1 .. 1:10 (slope 1 / (1 + 9 sqrt(ratio/100)))"),
            "level": _knob("make-up output level"),
        }},
    "amp": {
        "tool": "set_amp", "summary": "Amp model (preamp + waveshaper) and its tone stack. "
        "A new model loads its default knobs.",
        "fields": {
            "enabled": ON,
            "model": _select("amp model; 1..10 play (a model outside 1..10 keeps the last "
                             "one). Names in the manual's order, the index mapping is not "
                             "verified by ear", AMP_MODELS),
            "gain": _knob("drive into the waveshaper: 0..50 clean to edge, 50..100 up to "
                          "6x more drive (grit, distortion, compression)"),
            "bass": _knob("tone stack bass: peaking filter at 100 Hz (50 = about flat)"),
            "mid": _knob("tone stack mid: peaking filter at the midfreq centre"),
            "midfreq": _select("mid centre frequency", {0: "200 Hz", 1: "400 Hz", 2: "800 Hz",
                                                        3: "1.6 kHz", 4: "3 kHz"}),
            "treble": _knob("tone stack treble: peaking filter at 4.5 kHz (brightness, "
                            "string/finger noise)"),
            "volume": _knob("amp output level"),
        }},
    "cab": {
        "tool": "set_cab", "summary": "Speaker cabinet: a stock cab impulse response or a "
        "user IR (import with ir_import). Strong effect on lows and highs.",
        "fields": {
            "enabled": ON,
            "type": _select("cabinet: 1..10 stock cabs, 11..19 user IR slots 1..9",
                            CAB_TYPES),
            "p1": _knob("no effect", 0, 4),
            "p2": _knob("no effect"),
            "p3": _knob("no effect"),
            "p4": _knob("no effect", 0, 9),
        }},
    "mod": {
        "tool": "set_mod", "summary": "Modulation (after the EQ). A new type loads its "
        "defaults. Most types: out = (1 - mix) dry + mix wet.",
        "fields": {
            "enabled": ON,
            "type": _select("modulation type", MOD_TYPES),
            "p1": _knob("rate (LFO speed)"),
            "p2": _knob("mix (wet amount)"),
            "p3": _knob("type specific: phaser sweep range, flanger feedback, filter "
                        "bandwidth (10 + 190 p3 Hz), step phaser steps"),
            "p4": _knob("type specific: chorus depth"),
            "p5": _knob("no effect", 0, 240),
        }},
    "delay": {
        "tool": "set_delay", "summary": "Bass delay (open firmware addition, console "
        "`delay`): repeats lose low end each pass so the bass stays tight. The HID delay "
        "block (0x85) is not played: use set_delay.",
        "fields": {
            "on": {"min": 0, "max": 1, "unit": "bool", "meaning": "delay on/off"},
            "time_ms": _knob("repeat time", 20, 1000, "ms"),
            "feedback": _knob("repeats (feedback x 0.95 max)"),
            "mix": _knob("repeat level (dry stays at unity)"),
            "lowcut": _knob("high-pass on the repeats: 0 off, else 20 * 25^(k/100) Hz "
                            "(20..500 Hz; 63 = 150 Hz) - keeps the low end clean"),
            "tone": _knob("low-pass on the repeats: 100 off, else 1000 * 10^(k/100) Hz "
                          "(1..10 kHz); lower = darker repeats"),
        }},
    "reverb": {
        "tool": "set_reverb", "summary": "Reverb, last in the chain: out = dry + level * "
        "wet. A new type loads its defaults.",
        "fields": {
            "enabled": ON,
            "type": _select("reverb type", REVERB_TYPES),
            "p1": _knob("no effect", 0, 200),
            "level": _knob("wet level"),
            "decay": _knob("tail length (comb feedback)"),
            "p4": _knob("tone: one-pole low-pass on the wet signal; higher = brighter tail"),
        }},
}

EQ = {
    "tool": "set_eq", "summary": "Bass EQ after the cab (console `eq`, stored with "
    "save_preset): HPF, LPF (12 dB/oct) and 5 peaking bands. Changes glide.",
    "fields": {
        "on": {"min": 0, "max": 1, "unit": "bool", "meaning": "EQ on/off"},
        "hpf_hz": _knob("high-pass: 0 = off, else 20..200 Hz; removes sub rumble and "
                        "boom (40..60 Hz tightens a muddy low end)", 0, 200, "Hz"),
        "lpf_hz": _knob("low-pass: 0 = off, else 2000..20000 Hz; lower = darker, less "
                        "string noise", 0, 20000, "Hz"),
        "band": _knob("band 1..5 (defaults 40, 100, 250, 800, 3000 Hz)", 1, 5, "index"),
        "freq_hz": _knob("band centre", 30, 10000, "Hz"),
        "gain_db": _knob("band gain", -15, 15, "dB"),
        "q": {"min": 0.3, "max": 4.0, "unit": "q", "meaning": "band width: higher = "
              "narrower (1.0 default, 0.7 broad, 2..4 surgical)"},
    }}

GLOBAL = {
    "set_output": {"gain_db": _knob("output gain after the master: >= 0 dB, or -6 / -12",
                                    -12, 24, "dB"),
                   "mute": {"min": 0, "max": 1, "unit": "bool", "meaning": "output mute"}},
    "drums": {"on": {"min": 0, "max": 1, "unit": "bool", "meaning": "drum machine on/off"},
              "rhythm": _knob("pattern", 1, 40, "index"),
              "bpm": _knob("tempo", 40, 300, "bpm"),
              "level": _knob("drum level")},
    "settings": {
        "cab_global": _knob("global cab switch: 0 forces the cab off in every preset "
                            "(for an external cab or amp), 1 = the preset decides", 0, 1),
        "input_gain": _knob("input gain step: 0 mute, 1..11 = -55..-5 dB, 12..13 = 0 dB "
                            "(13 default), 14..25 = +0.5..+6 dB", 0, 25, "index"),
        "tuner": _knob("tuner mode on the pedal", 0, 1, "bool"),
        "bt_audio": _knob("Bluetooth audio on: a phone plays backing tracks into the "
                          "output (not through the effects)", 0, 1, "bool"),
        "ring_color": _knob("light-ring colour of the current slot (0 = red)", 0, 9, "index"),
        "ring_level": _knob("light-ring brightness of the current slot"),
    },
}

RECIPES = """\
Heuristics (bass tone, general practice - measure with audio_test to confirm):
- brighter / more definition: treble +10..20, EQ band 5 (2.5..4 kHz) +2..+4 dB,
  EQ LPF off or >= 8 kHz, reverb p4 up; a brighter cab or IR.
- darker / less string noise: treble down, EQ LPF 4..6 kHz, band 5 cut.
- muddy / boomy low end: EQ band 3 (200..300 Hz) -2..-4 dB, EQ HPF 40..50 Hz,
  amp bass down 5..10; keep band 1 (40 Hz) flat unless it booms.
- more punch / growl: band 4 (700..900 Hz) +2..+4 dB, amp mid up, midfreq 800 Hz,
  some amp gain (40..60).
- scooped / slap: comp on (threshold 30..45, ratio 50..70, attack 20..40,
  level to match), mids cut (band 3/4 -3..-5 dB), treble and band 5 up,
  bass slightly up, gain low (clean), reverb off or low.
- fingerstyle warm: comp gentle (ratio 20..40), treble down, band 2 (100 Hz) +2 dB.
- level: after big changes match the output with amp volume or comp level.
Change a few values at a time; `save_preset` only after the user agrees."""

_SETTINGS_ORDER = tuple(SETTINGS_FIELDS)


def check_complete() -> list[str]:
    """Names of MODULES / settings fields without docs (empty = complete)."""
    missing = []
    for name, (_i, _off, fields) in MODULES.items():
        if name == "delay":          # the HID delay block is not played; see BLOCKS["delay"]
            continue
        missing += [f"{name}.{f}" for f in fields if f not in BLOCKS[name]["fields"]]
    missing += [f"settings.{f}" for f in _SETTINGS_ORDER if f not in GLOBAL["settings"]]
    return missing


def parameter_docs() -> dict:
    """Every block, field, range, unit and sound meaning, plus tone recipes."""
    return {"chain": "in -> gate -> comp -> amp -> cab -> eq -> mod -> delay -> reverb -> "
                     "master -> output",
            "blocks": BLOCKS, "eq": EQ, "global": GLOBAL, "recipes": RECIPES}
