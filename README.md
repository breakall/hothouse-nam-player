# Hothouse NAM Player

Open-source NAM firmware for the Cleveland Music Co. Hothouse / Daisy Seed.
The project has one firmware host with separate build-time adapters for the
hardware-validated A1 Nano-ReLU and A2-Lite engines. Both run at 48 kHz and
therefore share the signal chain, controls, presets, capture slots, USB
protocol, deadline protection, and enclosure-safe firmware recovery.

## Hardware-validated milestone

The signal path is:

`mono input -> input gain -> optional NAM -> selected cabinet IR -> 3-band EQ -> reverb -> output level -> stereo output`

The cabinet stage is optional for full-rig captures. A build can contain one to
two mono 48 kHz PCM WAV IRs, each capped at 1024 taps. Toggle 2 selects IR A,
Off, or IR B; only the selected FIR is processed in the audio callback.

On 2026-09-08, a Fender-style amp-and-cab Nano-ReLU capture ran successfully:
LED 1 remained on, LED 2 remained off, processed audio was stable, and
footswitch 1 toggled bypass. The capture is not included because its T3K
license permits use but prohibits redistribution of the data file.

The firmware can also enable a separate cabinet IR at build time. A tiny NAM
Core test model plus a 1024-tap IR was validated earlier, but ordinary A1 Nano
Tanh captures exceeded the real-time deadline. Use ReLU captures only on this
target until measured otherwise.

On 2026-09-09, the A2-Lite backend was hardware-validated with a Fender
Princeton full-rig capture. Model processing, bypass, LEDs and controls were
stable with no deadline fault. The shared signal path includes a smooth input
noise gate to prevent the Hothouse input-stage noise floor from exciting a
model; quiet notes were verified to ring out naturally.

## Architecture

`firmware/hothouse_nam_host.cpp` owns all product behavior. It talks only to
the small `firmware/nam_engine.h` contract: identify the backend and payload,
load or clear a model, and process one 48-sample block. A build links exactly
one adapter:

- `nam_engine_a1.cpp` parses a variable-size NAMB payload, constructs the NAM
  Core WaveNet object, resets/prewarms its state once, and invokes its block
  processor.
- `nam_engine_a2.cpp` copies the already-selected 1,871 floating-point weights
  into the fixed A2-Lite runtime and invokes its specialized 48-sample kernel.

Those loading and evaluator details are inherent to the two runtime formats;
they no longer create parallel implementations of the pedal. The USB installer
follows the same boundary: it discovers the running backend, asks that
backend's adapter to validate and prepare the download, then uses one transfer
and activation flow.

## Controls

- Knob 1: input gain, logarithmic 0.25x–4x with unity at noon
- Knob 2: reverb wet/dry mix
- Knob 3: output level
- Knob 4: bass, ±10 dB
- Knob 5: middle, ±10 dB
- Knob 6: treble, ±10 dB
- Toggle 1: selected UP reverb / off / selected DOWN reverb
- Toggle 2: cabinet IR A / off / B (up / middle / down)
- Toggle 3: capture slot A / B / C (up / middle / down)
- Footswitch 1: overall processed/dry bypass
- Footswitch 2, short press: engage the saved preset or return to the live panel
- Footswitch 2, hold 1.5 seconds: save all six physical knob positions and the
  reverb/IR toggle positions, then engage the preset
- LED 1: effect active
- LED 2: preset engaged; it flashes briefly when saving and fast-blinks for a
  model fault or deadline overrun
- Hold both footswitches: persistent Daisy DFU recovery

The preset is stored in a dedicated QSPI flash sector and survives power loss.
When first engaged, the saved values control the sound. Moving an individual
knob or toggle wakes up only that control for temporary editing; the remaining
controls continue using their saved values. These edits do not alter the saved
preset. Knob edits are relative to the recalled value, so touching a physically
mismatched knob does not cause an abrupt parameter jump. Disengaging and
re-engaging recalls the saved values again. Holding
Footswitch 2 for 1.5 seconds is the explicit action that overwrites the preset.
Toggle 3 always follows its physical position and is not changed by preset
recall, because changing it may reload a model.

## Set up

```sh
git clone --recursive https://github.com/breakall/hothouse-nam-pedal hothouse-nam-player
cd hothouse-nam-player
./tools/setup_dependencies.sh
```

## Choose a backend at build time

The backend is compiled into the firmware; only one is present in a binary.
Both variants keep the same MVP controls and safety behavior.

## Install captures over USB (no DFU)

After one capture-loader-capable firmware build has been flashed, up to three
captures can be stored through the pedal's normal USB connection. Keep it in its
ordinary operating mode—do not hold the footswitches and do not enter DFU.

```sh
python3 tools/install_capture.py "/absolute/path/to/capture-a.nam" --slot A
python3 tools/install_capture.py "/absolute/path/to/capture-b.nam" --slot B
python3 tools/install_capture.py --list
python3 tools/install_capture.py --delete-slot C
```

Or use the Make target:

```sh
make install-capture CAPTURE="/absolute/path/to/capture.nam" SLOT=A
make list-captures
```

The installer finds the USB serial device, asks the pedal which backend it is
running, validates and prepares the capture, transfers it with per-chunk
acknowledgements, verifies a CRC-32 checksum in firmware, writes it to a
dedicated QSPI slot, and activates it immediately when that slot is selected.
Captures survive power cycles. Toggle 3 selects slot A, B, or C; only that
capture is instantiated and processed. Selecting an empty slot bypasses only
the NAM stage, leaving input gain, cabinet IR, EQ, reverb, and output level
active. Corrupt capture data is treated as a fault rather than as an empty slot.
Moving Toggle 3 mutes the final output in the first audio block, then waits 60
ms in silence for the switch position to settle. Only the final slot is loaded;
its output fades in over 20 ms. This avoids both model-reset discontinuities
and an unnecessary middle-slot load when moving between the outer positions.

- A2-Lite firmware accepts compatible A2 `.nam` captures, searches all models
  in a slimmable download, and extracts the supported 1,871-weight submodel
  before transfer.
- A1 firmware accepts A1 Nano-ReLU `.nam` or `.namb` captures. For `.nam`, the
  installer automatically runs the locally built `nam2namb` converter. The A1
  runtime payload limit is 64 KiB; real-time DSP compatibility remains limited
  to the Nano-ReLU class validated on this hardware.
- `--port /dev/...` overrides automatic USB-port detection, and
  `--converter /path/to/nam2namb` overrides converter discovery. Slot A begins
  at the former single-capture address, so an existing installed capture
  migrates as slot A after updating the firmware.

The application firmware now runs from SRAM so it can safely erase and write
the user capture area while audio firmware is active. QSPI offsets
`0x007b0000`–`0x007eefff` contain the three independently erasable capture
slots. Reverb configuration at `0x007f0000` and the preset sector at
`0x007ff000` are not touched.

## Select reverb engines over USB

The firmware includes four reverb engines: `reverbsc`, `dattorro`, `fdn16`,
and `hybrid`. Only the two engines mapped to Toggle 1's UP and DOWN positions
are instantiated in SDRAM and processed at runtime. The middle position is
always off. The mapping is stored independently of captures and presets, so it
survives power cycles and changing it does not require a firmware re-upload.
The factory mapping is `hybrid` on UP for an articulate room and `dattorro` on
DOWN for a dense, modulated hall.

With the pedal in its ordinary USB operating mode:

```sh
python3 tools/configure_reverbs.py list
python3 tools/configure_reverbs.py info
python3 tools/configure_reverbs.py map up hybrid
python3 tools/configure_reverbs.py map down dattorro
```

The same commands can be sent by any serial terminal:

```text
HNAM REVERB LIST
HNAM REVERB INFO
HNAM REVERB MAP UP hybrid
HNAM REVERB MAP DOWN dattorro
```

Mappings are saved immediately. The two positions must be different. Reverb
changes briefly stop and restart audio to construct the selected engine states;
they intentionally clear existing reverb tails.

### A1 Nano-ReLU

Build the NAMB converter:

```sh
cmake -S nam-pedal/nam-binary-loader -B build/nam-binary-loader \
  -DNAM_CORE_PATH="$PWD/nam-pedal/NeuralAmpModelerCore"
cmake --build build/nam-binary-loader
```

Build the A1 firmware from the repository root, then upload locally licensed
captures into the desired slots:

```sh
make a1 USE_IR=0
python3 tools/install_capture.py capture.nam --slot A
```

For an amp-only capture, embed one or two mono 48 kHz PCM WAV files and build
with the IR bank enabled:

```sh
make a1 USE_IR=1 \
  IR_A="/absolute/path/to/cab-a.wav" \
  IR_B="/absolute/path/to/cab-b.wav"
```

`IR_A` is required when `USE_IR=1`; `IR_B` is optional. When only IR A is
supplied, both outer switch positions select it. The center position always
bypasses the cabinet stage, which supports full-rig captures with a baked-in
cab. The legacy `IR=...` spelling remains an alias for `IR_A`.

For a broadly useful two-IR bank, use:

- **IR A — American:** an open-back 1x12 with a Jensen C12N/Oxford-style
  speaker. This is the clear, full-range blackface voice for Fender-style
  captures and a useful clean platform generally.
- **IR B — British:** an open-back 2x12 with Celestion Alnico Blue-style
  speakers. This supplies the warmer compression, upper-mid character, and
  chime that complements Vox and Matchless-style captures.

A balanced mono cap-edge capture or an already phase-aligned 57/ribbon blend
is more generally useful than an extreme on-axis mic position. If Dumble tones
matter more than vintage Fender tones, an EVM12L-style open-back 1x12 is the
best substitute for IR A.

### A2-Lite

Set up the pinned A2 runtime once, build the firmware, then upload A2 captures:

```sh
make setup-a2
make a2 USE_IR=0
python3 tools/install_capture.py "/absolute/path/to/capture.nam" --slot A
```

For a `SlimmableContainer`, the installer examines every submodel and selects
the first supported 3-channel A2-Lite architecture (1,871 weights, LeakyReLU,
48 kHz). If none match, it reports the incompatibility found in each candidate.
Full A2 is deliberately rejected instead of loading a model that cannot meet
the Daisy Seed's real-time budget. An amp-only A2-Lite capture can
use the same `USE_IR=1 IR_A=... IR_B=...` options shown above. The
combined A2-Lite + IR + EQ + reverb path must still be cycle-checked on hardware
for the specific capture.

An optional diagnostic build uses footswitch 2 to isolate input and output
noise while keeping the A2 workload active:

```sh
make a2 A2_DIAGNOSTIC=1
```

Build outputs:

- A1: `firmware/build/a1/hothouse_nam.bin`
- A2-Lite: `firmware/build/a2/hothouse_nam_a2.bin`

From the repository root, start the enclosure-safe watcher and reconnect USB:

```sh
./tools/wait_and_flash_hothouse.sh firmware/build/a2/hothouse_nam_a2.bin
```

On macOS, the normal application enumerates as Electrosmith `Daisy Seed Built
In` (`0483:5740`); DFU mode enumerates as `0483:df11`. Start the watcher before
holding both footswitches for two seconds. The three alternating LED flashes
confirm the recovery gesture, and the watcher must then report both `Download
done` and `File downloaded successfully`.

USB serial and DFU access must be run with permission to access host USB
devices. Sandboxed development environments may show
`/dev/cu.usbmodem...` while still preventing the serial port or `dfu-util` from
opening the hardware. In that case, rerun the watcher with direct hardware
access rather than treating an empty `dfu-util -l` result as evidence that the
pedal did not enter DFU.

## Tests

Run the hardware-independent regression suite from the repository root:

```sh
make test
```

It exercises capture command parsing and flash-store failure cases, the
mute/settle/load/fade capture-transition controller at sample level, all three
reverb engines, A1/A2 capture validation, A2 submodel selection, and installer
transfer/cancellation behavior. The A1 and A2 cross-builds remain separate
integration checks because their model engines depend on the embedded runtime.

## Hardware validation still required

The four build variants—A1/A2-Lite, each with and without an IR bank—need
on-device audio and worst-case cycle validation. The new Dattorro, 16-line FDN,
and hybrid-space engines must be cycle-profiled against representative A1 and
A2 captures before they are treated as pedal-safe. See
[MILESTONES.md](MILESTONES.md).

## Licensing

Project firmware is distributed under GPL-3.0, matching HothouseExamples.
NeuralAmpModelerCore, nam-binary-loader, libDaisy and DaisySP retain their own
licenses. The A2 build uses a pinned MIT-licensed runtime from DaisySeedProjects;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). NAM captures and cabinet
IRs are not included; users must supply assets they are licensed to use.
