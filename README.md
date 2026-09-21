# Hothouse NAM Player

Open-source NAM firmware for the Cleveland Music Co. Hothouse / Daisy Seed.
The project has separate build-time backends for the hardware-validated A1
Nano-ReLU and A2-Lite paths. Both run at 48 kHz and expose the same core
amp-player signal chain, controls, reliable bypass, and enclosure-safe firmware
recovery.

## Hardware-validated milestone

The signal path is:

`mono input -> input gain -> NAM -> selected cabinet IR -> 3-band EQ -> reverb -> output level -> stereo output`

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
stable with no deadline fault. The A2 signal path includes a smooth input noise
gate to prevent the Hothouse input-stage noise floor from exciting the model;
quiet notes were verified to ring out naturally.

## Controls

- Knob 1: input gain, logarithmic 0.25x–4x with unity at noon
- Knob 2: reverb wet/dry mix
- Knob 3: output level
- Knob 4: bass, ±10 dB
- Knob 5: middle, ±10 dB
- Knob 6: treble, ±10 dB
- Toggle 1: room / off / hall (up / middle / down)
- Toggle 2: cabinet IR A / off / B (up / middle / down)
- Toggle 3: reserved
- Footswitch 1: overall processed/dry bypass
- Footswitch 2, short press: engage the saved preset or return to the live panel
- Footswitch 2, hold 1.5 seconds: save all six physical knob positions and all
  three toggle positions, then engage the preset
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

After one capture-loader-capable firmware build has been flashed, captures can
be replaced through the pedal's normal USB connection. Keep the pedal in its
ordinary operating mode—do not hold the footswitches and do not enter DFU.

```sh
python3 tools/install_capture.py "/absolute/path/to/capture.nam"
```

Or use the Make target:

```sh
make install-capture CAPTURE="/absolute/path/to/capture.nam"
```

The installer finds the USB serial device, asks the pedal which backend it is
running, validates and prepares the capture, transfers it with per-chunk
acknowledgements, verifies a CRC-32 checksum in firmware, writes it to a
dedicated QSPI slot, and activates it immediately. The installed capture
survives power cycles and replaces the previous user-installed capture. If no
valid installed capture exists, the firmware loads its embedded factory model.

- A2-Lite firmware accepts compatible A2 `.nam` captures and extracts the
  supported 1,871-weight submodel before transfer.
- A1 firmware accepts A1 Nano-ReLU `.nam` or `.namb` captures. For `.nam`, the
  installer automatically runs the locally built `nam2namb` converter. The A1
  runtime payload limit is 64 KiB; real-time DSP compatibility remains limited
  to the Nano-ReLU class validated on this hardware.
- `--port /dev/...` overrides automatic USB-port detection, and
  `--converter /path/to/nam2namb` overrides converter discovery.

The application firmware now runs from SRAM so it can safely erase and write
the user capture area while audio firmware is active. QSPI offsets
`0x007b0000`–`0x007effff` are reserved for the installed capture; the existing
preset sector at `0x007ff000` is not touched.

### A1 Nano-ReLU

Build the NAMB converter:

```sh
cmake -S nam-pedal/nam-binary-loader -B build/nam-binary-loader \
  -DNAM_CORE_PATH="$PWD/nam-pedal/NeuralAmpModelerCore"
cmake --build build/nam-binary-loader
```

Convert a locally licensed A1 Nano-ReLU model to NAMB, then build from the
repository root:

```sh
./build/nam-binary-loader/nam2namb capture.nam capture.namb
make a1 MODEL="$PWD/capture.namb" USE_IR=0
```

For an amp-only capture, embed one or two mono 48 kHz PCM WAV files and build
with the IR bank enabled:

```sh
make a1 MODEL="$PWD/capture.namb" USE_IR=1 \
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

Set up the pinned A2 runtime once, then pass an A2 `.nam` file directly:

```sh
make setup-a2
make a2 MODEL="/absolute/path/to/capture.nam" USE_IR=0
```

For a `SlimmableContainer`, the build selects submodel 0 and rejects it unless
it is the supported 3-channel A2-Lite architecture (1,871 weights, LeakyReLU,
48 kHz). Full A2 is deliberately rejected instead of producing firmware that
cannot meet the Daisy Seed's real-time budget. An amp-only A2-Lite capture can
use the same `USE_IR=1 IR_A=... IR_B=...` options shown above. The
combined A2-Lite + IR + EQ + reverb path must still be cycle-checked on hardware
for the specific capture.

An optional diagnostic build uses footswitch 2 to isolate input and output
noise while keeping the A2 workload active:

```sh
make a2 A2_DIAGNOSTIC=1 MODEL="/absolute/path/to/capture.nam"
```

Build outputs:

- A1: `firmware/build/a1/hothouse_nam.bin`
- A2-Lite: `firmware/build/a2/hothouse_nam_a2.bin`

From the repository root, start the enclosure-safe watcher and reconnect USB:

```sh
./tools/wait_and_flash_hothouse.sh firmware/build/a2/hothouse_nam_a2.bin
```

## Hardware validation still required

The four build variants—A1/A2-Lite, each with and without an IR bank—compile
successfully. The newly unified A1 controls and both switchable-IR paths still
need on-device audio and worst-case cycle validation. See
[MILESTONES.md](MILESTONES.md).

## Licensing

Project firmware is distributed under GPL-3.0, matching HothouseExamples.
NeuralAmpModelerCore, nam-binary-loader, libDaisy and DaisySP retain their own
licenses. The A2 build uses a pinned MIT-licensed runtime from DaisySeedProjects;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). NAM captures and cabinet
IRs are not included; users must supply assets they are licensed to use.
