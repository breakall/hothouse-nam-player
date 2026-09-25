# Hothouse NAM Pedal Design

The product runs one fixed A2-Lite evaluator and stores three compatible
captures in QSPI. Toggle 3 selects the active capture; only one evaluator is
active at a time.

Audio path:

`mono input -> input gain -> A2-Lite model -> optional cabinet IR -> 3-band EQ -> reverb -> output level -> dual mono`

Core constraints:

- 48 kHz sample rate and 48-sample audio blocks.
- A compatible capture contains exactly 1,871 A2-Lite weights.
- The application runs from SRAM so it can update capture slots in QSPI.
- Capture changes mute immediately, debounce in silence, then fade in.
- USB logging never blocks audio or startup.
- Both-footswitch DFU recovery remains available.
