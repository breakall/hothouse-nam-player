# Milestones

## v0.1 — A1 Nano-ReLU proof of concept

Status: complete on hardware, 2026-09-08.

- Cleveland Music Co. Hothouse with Daisy Seed boots standalone.
- Embedded A1 Nano-ReLU NAMB runs at 48 kHz with 48-sample blocks.
- Amp-and-cab Fender-style capture runs without deadline overruns.
- Footswitch 1 toggles processed/dry bypass.
- Knob 1 controls input gain; knob 3 controls output level.
- LED 1 indicates active state; LED 2 indicates load/deadline failure.
- Normal USB reconnect supports enclosure-safe firmware updates.
- Downloaded capture data is deliberately excluded from the repository.

## v0.2 — A2-Lite feasibility

Status: A2-Lite audio path validated on hardware, 2026-09-09; detailed cycle
profiling and headroom decision pending.

- Add a separate A2-Lite build target without changing the A1 backend. Done.
- Select and validate A2-Lite explicitly from a packed A2 model. Done.
- Place hot state in DTCM and the 76 KB history arena in D2 SRAM. Done.
- Preserve dry fallback and enclosure-safe recovery. Done.
- Run a baked-cab A2 capture on the Daisy Seed. Done with a Fender Princeton
  full-rig capture.
- Suppress input-stage idle noise without truncating sustained notes. Done.
- Record model-only cycles and worst-case callback cycles.
- Decide whether enough headroom remains for EQ and ambience.

## v0.3 — Dream-style controls

- Bass, middle and treble controls. Done and hardware-validated, 2026-09-09.
- Common control layout: input gain, reverb mix, output, bass, middle, treble.
  Implemented for A1 and A2-Lite; A1 hardware validation pending.
- Room/off/hall reverb on Toggle 1. Implemented for both backends; A2-Lite
  hardware-validated, A1 hardware validation pending.
- Off plus two embedded cabinet IRs selected by Toggle 2, with only one FIR
  processed at a time. Implemented for both backends; hardware validation
  pending.
- Preset storage and recall. Implemented and hardware-validated on A2-Lite.
- Record worst-case cycles for A1 and A2-Lite with IR, EQ, and hall reverb all
  active.

## v0.4 — Capture installation without DFU

Status: implemented and host-tested; on-pedal transfer validation pending.

- Run both application backends from SRAM so normal firmware can write QSPI.
- Detect the active A1/A2 backend over USB and validate the selected capture.
- Convert A1 Nano-ReLU `.nam` files to NAMB automatically; extract validated
  A2-Lite weights directly from `.nam` files.
- Transfer in acknowledged chunks, verify CRC-32, persist one installed
  capture, and activate it without rebooting or entering DFU.
- Reload the installed capture after power cycling and fall back to the
  embedded factory capture if stored data is missing, interrupted, or invalid.
- Hardware-test installation, immediate audio activation, and power-cycle
  persistence on both backends.
