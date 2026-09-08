# Hothouse NAM Pedal

Open-source NAM firmware for the Cleveland Music Co. Hothouse / Daisy Seed.
The project has separate build-time backends for the hardware-validated A1
Nano-ReLU path and an experimental A2-Lite path. Both run at 48 kHz with
reliable bypass and enclosure-safe firmware recovery.

## Hardware-validated milestone

The v0.1 signal path is:

`mono input -> input gain -> A1 Nano-ReLU NAMB -> output level -> dual mono`

On 2026-09-08, a Fender-style amp-and-cab Nano-ReLU capture ran successfully:
LED 1 remained on, LED 2 remained off, processed audio was stable, and
footswitch 1 toggled bypass. The capture is not included because its T3K
license permits use but prohibits redistribution of the data file.

The firmware can also enable a separate cabinet IR at build time. A tiny NAM
Core test model plus a 1024-tap IR was validated earlier, but ordinary A1 Nano
Tanh captures exceeded the real-time deadline. Use ReLU captures only on this
target until measured otherwise.

## Controls

- Knob 1: input gain, 0.25x–4x
- Knob 6: output level
- Footswitch 1: processed/dry bypass
- LED 1: effect active
- LED 2: model fault or deadline overrun
- Hold both footswitches: persistent Daisy DFU recovery

## Set up

```sh
git clone --recursive https://github.com/breakall/hothouse-nam-pedal
cd hothouse-nam-pedal
./tools/setup_dependencies.sh
```

## Choose a backend at build time

The backend is compiled into the firmware; only one is present in a binary.
Both variants keep the same MVP controls and safety behavior.

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

For an amp-only capture, embed a mono 48 kHz WAV and build with the IR enabled:

```sh
cd firmware
make embed-ir IR=/absolute/path/to/cabinet-48k.wav
cd ..
make a1 MODEL="$PWD/capture.namb" USE_IR=1
```

### A2-Lite

Set up the pinned A2 runtime once, then pass an A2 `.nam` file directly:

```sh
make setup-a2
make a2 MODEL="/absolute/path/to/capture.nam"
```

For a `SlimmableContainer`, the build selects submodel 0 and rejects it unless
it is the supported 3-channel A2-Lite architecture (1,871 weights, LeakyReLU,
48 kHz). Full A2 is deliberately rejected instead of producing firmware that
cannot meet the Daisy Seed's real-time budget. The current A2 MVP expects a
capture with its cabinet baked in; separate IR processing can be evaluated
after hardware cycle measurements.

Build outputs:

- A1: `firmware/build/a1/hothouse_nam.bin`
- A2-Lite: `firmware/build/a2/hothouse_nam_a2.bin`

From the repository root, start the enclosure-safe watcher and reconnect USB:

```sh
./tools/wait_and_flash_hothouse.sh firmware/build/a2/hothouse_nam_a2.bin
```

## Roadmap

The next step is hardware validation of the A2-Lite build, followed by the
Dream-style controls. See [MILESTONES.md](MILESTONES.md).

## Licensing

Project firmware is distributed under GPL-3.0, matching HothouseExamples.
NeuralAmpModelerCore, nam-binary-loader, libDaisy and DaisySP retain their own
licenses. The A2 build uses a pinned MIT-licensed runtime from DaisySeedProjects;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). NAM captures and cabinet
IRs are not included; users must supply assets they are licensed to use.
