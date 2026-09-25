# Implementation plan

## Complete

- Fixed A2-Lite evaluator at 48 kHz with 48-sample blocks.
- Three CRC-verified QSPI capture slots selectable from the panel.
- Browser and command-line capture validation and transfer.
- Optional cabinet IR bank, 3-band EQ, selectable reverb, presets, and bypass.
- Immediate mute, switch settling, capture load, and click-free fade-in.
- Host regression tests and enclosure-safe DFU workflow.

## Remaining hardware validation

- Record worst-case cycles with representative captures, IR, EQ, and each
  reverb engine enabled.
- Verify power-cycle persistence and all three capture slots after production
  firmware changes.
- Confirm the heaviest supported signal path remains below the 1 ms deadline.
