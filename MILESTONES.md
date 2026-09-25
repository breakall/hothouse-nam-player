# Milestones

## A2-Lite evaluator

Hardware-validated on 2026-09-09 with a Fender Princeton full-rig capture.
The fixed evaluator, memory placement, bypass, controls, and input noise gate
operate within the Daisy Seed real-time budget.

## Pedal signal chain

Implemented: input gain, cabinet IR selection, 3-band EQ, selectable reverb,
output level, bypass, persistent presets, and deadline protection.

## Capture library

Implemented: three persistent slots, Tone3000 slimmable-container selection,
acknowledged USB transfer, CRC verification, corruption detection, immediate
mute, switch settling, and fade-in after a capture change.

## Release validation

Still required: worst-case cycle profiling for supported IR/reverb
combinations and end-to-end power-cycle testing of all three slots.
