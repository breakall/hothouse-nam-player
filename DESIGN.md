# Hothouse NAM Pedal Design

## MVP

The first milestone is deliberately narrow: prove that a Cleveland Music Co.
Hothouse / Daisy Seed can run a NAM amplifier capture followed by a cabinet IR
as a standalone pedal.

Audio path:

`mono input -> input gain -> NAMB model -> 1024-tap cabinet IR -> output level -> dual mono`

Requirements:

- 48 kHz sample rate and 48-sample (1 ms) audio blocks.
- One embedded, pedal-safe `.namb` capture.
- One embedded, mono 48 kHz PCM WAV cabinet IR, capped at 1024 taps.
- Dry DSP bypass on footswitch 1.
- Standalone boot; USB logging must never block startup.
- Runtime cycle logging for the complete NAM + IR stage.
- Both-footswitch Hothouse DFU gesture remains available.

## MVP Controls

- Knob 1: NAM input gain, 0.25x to 4x.
- Knob 6: output level.
- Footswitch 1: active/bypass.
- LED 1: active state.
- LED 2: lit on model-load failure or a NAM + IR callback overrun.
- Knobs 2-5, all toggles, and footswitch 2: reserved.

Use an amp/head/direct capture when a separate cabinet IR is enabled. An
amp-and-cab NAM followed by another cabinet IR will apply cabinet coloration
twice.

## Deferred

EQ, presets, multiple models/IRs, loading without a firmware rebuild, reverb,
tremolo, and a more Dream-like control surface are post-MVP work. See
`LATER.md`.
