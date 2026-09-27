# What "better than stock" means: research (2026-09-27)

Web research behind the M4-M8 items in `PARITY.md`. CPU figures without a
source are estimates. Budget: 600 MHz Cortex-M7 = 12.5k cycles/sample at
48 kHz (13.6k at 44.1 kHz); 512 KB FlexRAM, no external RAM - RAM is the
tighter limit.

## Corrections worth knowing

- The official editor writes user IRs as 44.1 kHz, 1024 float32 samples
  (the engine may truncate to 512 taps - verify on the device).
- The stock asset blocks (IRs, drum samples, amp data) are vendor IP: never
  redistribute them. Ship CC0/CC-BY content, or read what is already in the
  user's own image/flash.
- NAM models are trained at **48 kHz** (A2 receptive field ~6,350 samples,
  ~132 ms). Running them at 44.1 kHz shifts their response by ~8 %, so NAM
  needs the 48 kHz core ([T3K A2 guide](https://www.tone3000.com/guides/nam-a2-the-complete-guide)).
  The NAU88L21 does 48 kHz / 24-bit ([datasheet](https://www.nuvoton.com/export/resource-files/DS_NAU88L21_DataSheet_EN_Rev2.8.pdf)).

## A. Must-have (clear wins, feasible)

1. **48 kHz / 24-bit core, low latency.** 16-32 sample blocks, <3 ms
   analog-to-analog; oversampled shapers; FTZ/DN on the FPU (as
   [nam-pedal](https://github.com/tone-3000/nam-pedal)).
2. **IR engine.** 1024 taps lose <46 Hz, 2048 reach ~23 Hz - ported bass
   cabs need the longer IRs ([Fractal forum](https://forum.fractalaudio.com/threads/standard-and-ultra-res-ir-lengths.195272/),
   [Line 6 IR analysis](https://alphasonicstudio.com/2022/04/04/line-6-helix-ir-block-analysis/)).
   Competitors: 2048 points on newer Hotone units
   ([Ampero II Stage](https://www.hotone.com/products/multi-effects/Ampero%20II%20Stage)),
   20 user IR slots on the GP-5 ([Valeton](https://www.valeton.net/product/gp-5/)).
   FB200 users complain about 9 slots
   ([Bass Musician Mag](https://bassmusicianmagazine.com/2025/09/review-flamma-fb200-bass-multi-effects-processor/)).
   Plan: WAV import (44.1/48 kHz, 16/24/32-bit), host-side resample and
   minimum-phase trim, up to 4096 taps, low/high cut, dual-IR blend.
   Partitioned convolution at 4096 taps ~5-8 % CPU
   ([STM32H750 example](https://github.com/Hassan-Islam00/stm32h750-partitioned-fft-convolution-reverb)),
   ~32-48 KB RAM, 16 KB flash per IR.
3. **Clean blend + crossover-parallel drive (Darkglass-style)** - the most
   bass-specific feature the FB200 lacks; <1 % CPU
   ([Anagram](https://www.darkglass.com/products/anagram),
   [Entropia](https://www.notreble.com/buzz/2025/09/26/darkglass-entropia-brings-multi-band-compression-and-distortion-to-anagram/)).
4. **5-7 band parametric/graphic EQ + global HPF/LPF** (~1-2 %).
5. **Bass-voiced delay** - the stock has none; int16 buffer, 1 s mono at
   48 kHz = 96 KB, low cut on the repeats (~1 %).
6. **Tuner and drums.** Fast low-B detection (>= 2 periods, ~65 ms), strobe
   on the RGB ring, mute-on-tune; drum level fix - "too loud even at the
   lowest settings" ([SonicMetric](https://sonicmetric.com/flamma-fb200-bass-multi-effects-pedal-with-amp-ir-modeling-review-portable-powerhouse-or-digital-distraction/));
   tap tempo.
7. **Open protocol, web editor, USB MIDI.** App dependence and the 3-char
   display are the main UX complaints
   ([Bass Gear Reviews](https://bassgearreviews.com/flamma-fb200-bass-multi-effects-portable-affordable-pedal-review/)).
   A static WebMIDI page that asks the pedal to describe its blocks
   ([Torvalds GuitarPedal](https://github.com/torvalds/GuitarPedal) pattern;
   also [valeton-gp50](https://github.com/drewmerc302/valeton-gp50),
   [MIDI Commander Custom](https://github.com/Charles5150/midi-commander-custom/issues/128)
   for browser firmware update). JSON presets for sharing (Git/Patchstorage).
8. **Multichannel UAC2:** dry DI + processed + re-amp return
   ([Boss GT-1B](https://www.boss.info/us/products/gt-1b/articles/));
   document FlexASIO/ASIO4ALL for Windows.

## B. Strong differentiators

9. **NAM A2-Lite player (`.namb`)** - the headline. "A2-Lite runs at 50 %
   CPU on an ARM Cortex M7 600MHz" ([T3K](https://www.tone3000.com/guides/nam-a2-the-complete-guide));
   RP2350 demo ~1,871 params ([pico demo](https://github.com/oyama/pico-neural-amp-modeler-demo));
   real time on Daisy Seed ([PedalPCB](https://forum.pedalpcb.com/threads/nam-a2-on-daisy.29507/)).
   Budget here 45-55 % CPU, ~8 KB weights + <=80 KB history; it replaces
   the amp block. MIT stack: [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
   (>= 0.5.2, `NAM_ENABLE_A2_FAST`), [nam-binary-loader](https://github.com/weliveindetail/nam-binary-loader),
   [nam-pedal](https://github.com/tone-3000/nam-pedal),
   [embedded write-up](https://www.tone3000.com/blog/running-nam-on-embedded-hardware);
   [TONE3000 API](https://www.tone3000.com/api) (>700k tones). Valeton
   needs a proprietary conversion ([guide](https://www.tone3000.com/blog/tone3000-valeton-nam-guide));
   NUX MG-30 has NAM slots ([NUX](https://nuxaudio.com/product/mg30/)).
10. **A1 -> A2-Lite distillation tool** on the host (train a student on the
    A1 model's output; no reamp files) - [Slimmable NAM](https://www.neuralampmodeler.com/post/introducing-slimmable-nam-neural-amp-models-with-adjustable-runtime-computational-cost).
11. **Octaver:** analog-style mono sub-octave (~1-2 %), later a poly
    filter-bank octave (~10-15 %, not with NAM)
    ([GP-5 bass review](https://bassgearreviews.com/valeton-gp-5-review-for-bass-2025/),
    [TalkBass](https://www.talkbass.com/threads/poly-octave-stranglehold-or-why-arent-pog-oc3-types-in-multi-effect-pedals.1157462/)).
12. **3-band multiband compressor** (~3-4 %).
13. **Envelope filter / sub-synth** (~2 %).

## C. Nice-to-have

14. **Looper** within RAM: IMA-ADPCM 4-bit mono ~8 s at 48 kHz (~16 s at
    24 kHz) in ~200 KB. Competitors: P1 80 s, B2 Four 60 s, GT-1B 32 s.
    Streaming to QSPI stalls XIP and wears flash (fine only with all code in
    ITCM - which ours is).
15. **AIDA-X / RTNeural LSTM** (LSTM-12 ~25-35 % CPU, estimate); use
    RTNeural (BSD-3) and read the JSON format (the AIDA-X plugin is GPL-3)
    ([overdriven.fr](https://overdriven.fr/overdriven/index.php/aida-x-models/),
    [NeuralSeed](https://github.com/GuitarML/NeuralSeed)).
16. Setlists, battery-save mode (clock/LED dimming; users find ~6 h short),
    metronome, CC0 IR/drum/preset libraries, public preset repository.

## Infeasible on this hardware

XLR DI / ground lift / expression jack (hardware), NAM A2-Full (~7x A2-Lite),
NAM A1 standard/lite or more than one NAM instance, long or stereo looper,
96 kHz full chain, poly octaver + NAM + long reverb at once.
