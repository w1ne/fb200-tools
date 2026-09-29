# Measurements

Stock vs. our firmware, measured through the mixer loopback rig (TrueMix600
mixer, USB to the Mac, pedal inline). Each run appends a section below.

How to run:

```bash
.venv/bin/python tools/measure.py run-all --device "FB200" --note "48 kHz, default preset"
.venv/bin/python tools/measure.py run-all --device "TrueMix600" --note "analog loopback, pedal inline"
```

Analysis code is covered by `tools/measure.py selftest` (synthetic signals)
which `tests/test_measure.py` runs.


## Sound quality over USB, v0.9.1 and imp/sound (2026-09-29)

Measured with `tools/sound_check.py` (USB only, no cable): the ADC idle
noise, the digital chain by reamping (`usb in`, the Mac plays into the chain)
and by the test generator (`tin`), the capture. Preset 1, all effects off
unless noted, master 42 % (-7.5 dB). The analog path (instrument jack -> ADC
front end, DAC -> output jack) needs a cable and an interface: not measured.
"imp/sound" = the pedal with the first imp/sound slot (run 2) where it was
measured, else the host models in `tests/test_dsp_host.py` /
`tests/test_delay.py` (marked "model").

| Metric | v0.9.1 (pedal) | imp/sound | How |
| --- | --- | --- | --- |
| ADC idle, 20 Hz..20 kHz | -70.7 dBFS (-63 dBFS at the input) | unchanged (analog) | `idle`, jack as found |
| ADC idle, 1..20 kHz | -92.0 dBFS | unchanged | `idle` |
| Hum 50/100 Hz | -72.5 / -77.6 dBFS | unchanged | `idle` |
| ADC DC offset | -11.7 LSB (-69 dBFS) | unchanged (stock) | `idle` |
| Dry gain | -7.5 dB = master 42 %; no other gain | unchanged | `dry` |
| -60 dBFS sine, THD, 100 Hz / 1 kHz | -28.0 / -31.2 dB | -29.8 / -35.8 dB (pedal, settled) | `dry` |
| same, model of host int16 -> x 0.42 -> capture | trunc -28.0 / -31.2 dB | round -29.7 / -35.2 dB | the limit: 16-bit in and out |
| -60 dBFS float sine, one conversion, THD | -41.5 dB (truncated) | -54.6 dB (rounded) | `outq_host_test` |
| Reamp ticks (1 kHz, 18.5 s) | 8, worst 10 ms window -25 dB | run 2 (v1 loop): 66, worst -40.6 dB; v2 loop: 0, worst -83 dB (model) | `glitch`, `engine_host_test` |
| Reamp 1 kHz THD+N 0.4..1.9 s after the stream start | no resampler (ticks only) | run 2 (v1): -37 dB; v2: < -76 dB (model) | `engine_host_test` |
| EQ low bands, THD+N of a -15 dBFS 1 kHz sine | -71.4 dB (EQ off: -80.3) | -82.0 dB = EQ off (pedal); error -69 -> -138 dB (model) | `eq`, `eq_host_test` |
| Delay repeat error, -40 / -20 / -6 dBFS | -46.2 / -66.1 / -80.3 dB | -64.2 / -84.7 / -86.4 dB (model) | `delay_host_test` |
| Amp gain 100, 3 kHz, inharmonic (aliasing) | model 1 -27.8, 4 -47.7, 10 -23.6 dB | parity (stock amp) | `amp` |
| Gate type 1 thr 60, decaying 55 Hz note | 4 open/close changes | parity (stock gate) | `gate` |
| Reverb CPU in a tail vs 10 s later | 22.0k / 22.0k cycles per block | - (no denormal cost) | `prof` |

Findings that are not changed (stock parity or analog):

- The instrument is on the ADC left channel only (ADC right muted: the hum
  and noise stay; ADC left muted: -96.7 dBFS, only the right channel's DC and
  noise). The stock sums L + R: the idle right channel adds 0.3 dB of noise
  and -5.5 LSB of DC.
- ADC oversampling (R2B 32/64/128/256 with R03): no change in the idle noise
  (-86.9 dBA in all four): the noise is the analog front end, not the ADC's
  decimation. The 16-bit I2S word is not the limit either: the ADC noise is
  ~17 dB above 16-bit quantisation.
- Amp aliasing: the stock upsamples 3x by linear interpolation and filters
  with one biquad before it keeps every 3rd sample. A generic hard-clip model
  of that structure (`3x lin + biquad`) gives -28.6 dB inharmonic at 3 kHz,
  drive 20 (the pedal: -23.6..-27.8 dB); with FIR interpolation and
  decimation: 3x -39.3, 6x -53.6, 8x -57.3 dB.
