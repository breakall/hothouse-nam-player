# Milestones

## v0.1 — A1 Nano-ReLU proof of concept

Status: complete on hardware, 2026-09-08.

- Cleveland Music Co. Hothouse with Daisy Seed boots standalone.
- Embedded A1 Nano-ReLU NAMB runs at 48 kHz with 48-sample blocks.
- Amp-and-cab Fender-style capture runs without deadline overruns.
- Footswitch 1 toggles processed/dry bypass.
- Knob 1 controls input gain; knob 6 controls output level.
- LED 1 indicates active state; LED 2 indicates load/deadline failure.
- Normal USB reconnect supports enclosure-safe firmware updates.
- Downloaded capture data is deliberately excluded from the repository.

## v0.2 — A2-Lite feasibility

Status: next.

- Upgrade NeuralAmpModelerCore and binary loading to A2-capable versions.
- Select A2-Lite explicitly from a packed A2 model.
- Run a baked-cab A2 capture on the Daisy Seed.
- Record model-only cycles and worst-case callback cycles.
- Preserve dry fallback and enclosure-safe recovery.
- Decide whether enough headroom remains for EQ and ambience.

## v0.3 — Dream-style controls

- Bass, middle and treble controls.
- Speaker/voice selection.
- Boost behavior and preset storage.
- Reverb if the measured CPU budget permits it.
