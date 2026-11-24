# Hothouse NAM Player

Open-source A2-Lite NAM firmware for the Cleveland Music Co. Hothouse / Daisy
Seed. The firmware runs at 48 kHz with 48-sample blocks and supports three
user-installable capture slots.

## Signal path

`mono input -> input gain -> optional A2-Lite model -> optional cabinet IR -> 3-band EQ -> reverb -> output level -> stereo output`

The A2-Lite evaluator was hardware-validated with a Fender Princeton full-rig
capture. Model processing, bypass, LEDs, controls, and capture switching were
stable without a deadline fault. A smooth input noise gate prevents the
Hothouse input-stage noise floor from exciting the model while preserving
quiet note decay.

`firmware/hothouse_nam_host.cpp` owns product behavior and uses the small
`firmware/nam_engine.h` interface. `firmware/nam_engine.cpp` loads the
1,871 selected floating-point weights into the fixed A2-Lite runtime and
invokes its specialized 48-sample kernel. The host tool and web editor search
every model in a Tone3000 slimmable download, select a compatible A2-Lite
submodel, and transfer the same tagged weight payload used by all three slots.

## Controls

- Knob 1: input gain, logarithmic 0.25x–4x with unity at noon
- Knob 2: reverb wet/dry mix
- Knob 3: output level
- Knob 4: bass, ±10 dB
- Knob 5: middle, ±10 dB
- Knob 6: treble, ±10 dB
- Toggle 1: selected UP reverb / off / selected DOWN reverb
- Toggle 2: cabinet IR A / off / B
- Toggle 3: capture slot A / B / C
- Footswitch 1: processed/dry bypass
- Footswitch 2 short press: engage the saved preset or return to the panel
- Footswitch 2 hold: save the panel settings and engage the preset
- Hold both footswitches: persistent Daisy DFU recovery

LED 1 shows the effect state. LED 2 shows the preset state and fast-blinks for
a model fault or deadline overrun. Presets survive power loss. Toggle 3 always
follows its physical position because changing it may reload a model.

## Set up and build

Requirements:

- Git, Python 3, and GNU Make
- The [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  for the `arm-none-eabi` target, including its Newlib headers and libraries
- [`dfu-util`](https://dfu-util.sourceforge.net/) for flashing (`brew install
  dfu-util` on macOS)

```sh
git clone --recursive https://github.com/breakall/hothouse-nam-player.git
cd hothouse-nam-player
make setup-a2
make firmware USE_IR=0
```

The build automatically discovers Arm's macOS installer under
`/Applications/ArmGNUToolchain`. On another platform or with a custom
installation, set `GCC_PATH` to the toolchain's `bin` directory.

The firmware is written to `firmware/build/firmware/hothouse_nam.bin`.

To build with one or two mono, 48 kHz cabinet IRs capped at 1,024 taps:

```sh
make firmware USE_IR=1 \
  IR_A="/absolute/path/to/cab-a.wav" \
  IR_B="/absolute/path/to/cab-b.wav"
```

`IR_A` is required when `USE_IR=1`; `IR_B` is optional. With only IR A,
both outer Toggle 2 positions select it. The middle position bypasses the
cabinet stage for full-rig captures.

## Install captures over USB

The browser-based [Hothouse NAM Editor](https://github.com/breakall/hothouse-nam-pedal-editor)
is a graphical alternative to the command-line tools below. In desktop Chrome
or Edge, it connects directly to the pedal over Web Serial to install, replace,
or delete captures and configure the UP and DOWN reverb assignments.

For command-line use, the pedal stores up to three captures through its normal
USB connection:

```sh
python3 tools/install_capture.py "/absolute/path/to/capture-a.nam" --slot A
python3 tools/install_capture.py "/absolute/path/to/capture-b.nam" --slot B
python3 tools/install_capture.py --list
python3 tools/install_capture.py --delete-slot C
```

The installer requires an A2 `.nam` file containing a compatible 48 kHz,
3-channel A2-Lite model with 1,871 weights. For a `SlimmableContainer`, it
examines every submodel and selects the first compatible one. Full A2 models
are rejected because they cannot meet the Daisy Seed real-time budget.

Transfers use acknowledged 128-byte chunks and CRC-32 verification. Captures
survive power cycles. Selecting an empty slot bypasses only the model stage.
Selecting a new slot cuts audio immediately, waits 60 ms for the switch to
settle, loads only the final slot, and fades audio in over 20 ms.

Capture slots occupy QSPI offsets `0x007b0000`–`0x007eefff`. Reverb
configuration at `0x007f0000` and presets at `0x007ff000` are separate.

## Reverb configuration

The firmware includes `reverbsc`, `dattorro`, `fdn16`, and `hybrid`.
Only the two engines assigned to Toggle 1 are instantiated. Configure them:

```sh
python3 tools/configure_reverbs.py list
python3 tools/configure_reverbs.py info
python3 tools/configure_reverbs.py map up hybrid
python3 tools/configure_reverbs.py map down dattorro
```

## Flash

Build first, start the watcher with direct USB access, and then hold both
footswitches for two seconds:

```sh
./tools/wait_and_flash_hothouse.sh firmware/build/firmware/hothouse_nam.bin
```

Three alternating LED flashes confirm the recovery gesture. A successful
upload reports both `Download done` and `File downloaded successfully`.

## Tests

```sh
make test
```

The suite covers capture parsing and flash persistence, mute/settle/load/fade
transitions, reverb engines, A2 model selection and validation, and installer
transfer cancellation. Hardware cycle profiling is still required for the
heaviest IR and reverb combinations.

## Experimental engine comparison

The default engine remains the existing DaisySeedProjects runtime. To try the
TONE3000 Cortex-M7 A2-Lite engine behind the same `nam_engine.h` interface:

```sh
make setup-tone3000
make test-engines
make firmware NAM_ENGINE=legacy USE_IR=0
make firmware NAM_ENGINE=tone3000 USE_IR=0
```

The two binaries are kept separately at
`firmware/build/firmware/hothouse_nam.bin` and
`firmware/build/firmware-tone3000/hothouse_nam.bin`. Both use BOOT_SRAM, the same
48 kHz / 48-frame host, gain/effects code, capture slots, `a2_lite` USB identity
and finite-weight validation. No capture conversion or reinstall is needed.
The candidate runs its code in ITCM, hot weights/instance in DTCM and histories
in cached AXI SRAM; the legacy runtime keeps its existing placement (code in
AXI SRAM and histories in D2 SRAM). This compares the deployed configurations,
not just isolated kernel instruction counts.

`make test-engines` compares both kernels with the same 96,000-frame NAMCore
golden input/output and capture at 48-frame blocks. Host verification on
2026-11-24 passed: legacy max absolute error `2.0862e-7`, ESR `-136.02 dB`;
TONE3000 `2.3842e-7`, `-135.07 dB`; candidate versus legacy `-134.51 dB`.
These verify numerical agreement; they do not measure target CPU load.

Both complete firmware variants also cross-compiled with Arm GNU 15.2.1,
`-O3`, `USE_IR=0`, based on `2456ab2`. The resulting binary sizes are
147,332 bytes (legacy) and 168,916 bytes (TONE3000). Linker-region usage:

| Region | Legacy | TONE3000 |
| --- | ---: | ---: |
| AXI SRAM | 147,332 B | 271,424 B |
| DTCM | 40,864 B | 38,336 B |
| D2 SRAM | 76,640 B | 0 B |
| ITCM | 0 B | 28,136 B |

Candidate AXI SRAM includes its 102,472-byte history and the ITCM load image;
the ITCM figure is runtime placement, so it must not be added again to the
binary size. These are memory/build results, not a speed comparison.

For target timing, flash each built binary using the procedure above, keeping
the same capture installed and the capture toggle at the same position. Then
run with direct hardware access:

```sh
python3 tools/benchmark_engine.py --port /dev/cu.usbmodem3466376232331
```

`HNAM ENGINE` reports `legacy` or `tone3000` without changing the protocol's
model-family identity. `HNAM BENCH` temporarily stops audio, reloads/prewarms
the current slot, warms 32 blocks, and times 256 deterministic xorshift32
blocks (seed `0x48534e41`, +/-0.1 input). It reports capture CRC, clock,
mean/p99/max cycles and an output checksum. IRQs are excluded from each timed
engine call; stimulus generation, output checksum and USB printing are outside
the timing window. The checksum is for repeatability within a backend, not
bit equality across engines with different accumulation order. The model is
reloaded/prewarmed and faded in before live audio resumes. No flash data is
modified by benchmarking.

Compare only runs with the same capture CRC, 480 MHz clock and build settings.
Repeat at least three times. Engine-only load is cycles / 480,000 per block;
it does not include IR, EQ, reverb, IRQ overhead or transition work. The normal
callback's existing cycle/deadline diagnostics still need to be checked for
the desired complete effect chain before selecting a production default.
Hardware timings are pending a connected pedal; no speedup has been measured.

## Licensing

Project firmware is GPL-3.0, matching HothouseExamples. libDaisy and DaisySP
retain their licenses. The A2 evaluator uses a pinned MIT-licensed runtime
from DaisySeedProjects; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Captures and cabinet IRs are not included; users must supply licensed assets.
