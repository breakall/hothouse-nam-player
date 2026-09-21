# Hothouse NAM + IR MVP Plan

## Implemented

- Hothouse/Daisy Seed target at 48 kHz with 48-sample blocks.
- Embedded NAMB loading without an SD card.
- Embedded 1024-tap FIR cabinet IR using DaisySP/CMSIS-DSP.
- Mono input, dual-mono output, input gain, output level, and dry bypass.
- Non-blocking diagnostics and callback cycle measurement.
- Host tools to embed a `.namb` and a 48 kHz PCM WAV.
- Successful QSPI firmware build.

## Exit Criteria

The MVP is complete after an on-hardware test confirms:

1. Standalone boot and dry bypass work.
2. The active path audibly runs both an amp-only NAM and a cabinet IR.
3. No glitches occur under sustained playing.
4. The maximum NAM + IR callback time remains below the 1 ms block deadline.
5. Gain, output, bypass, dual-mono output, and the DFU gesture behave correctly.

The bundled small NAM Core Darkglass test model validates the execution path,
but it is not the final guitar-amp tone. Replace it with a known-good Nano/ReLU
amp-only NAMB before declaring the hardware MVP complete.
