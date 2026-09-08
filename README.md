# Hothouse NAM Pedal

Open-source NAM firmware for the Cleveland Music Co. Hothouse / Daisy Seed.
The first hardware milestone runs an embedded A1 Nano-ReLU amp-and-cab capture
at 48 kHz with reliable bypass and enclosure-safe firmware recovery.

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

Build the NAMB converter:

```sh
cmake -S nam-pedal/nam-binary-loader -B build/nam-binary-loader \
  -DNAM_CORE_PATH="$PWD/nam-pedal/NeuralAmpModelerCore"
cmake --build build/nam-binary-loader
```

Convert and embed a locally licensed A1 Nano-ReLU model:

```sh
./build/nam-binary-loader/nam2namb capture.nam capture.namb
cd firmware
make embed-model MODEL="$PWD/../capture.namb"
make clean
make USE_IR=0
```

For an amp-only capture, embed a mono 48 kHz WAV and build with the IR enabled:

```sh
make embed-ir IR=/absolute/path/to/cabinet-48k.wav
make clean
make USE_IR=1
```

From the repository root, start the enclosure-safe watcher and reconnect USB:

```sh
./tools/wait_and_flash_hothouse.sh
```

## Roadmap

The next milestone is A2-Lite feasibility on the same Daisy Seed. See
[MILESTONES.md](MILESTONES.md).

## Licensing

Project firmware is distributed under GPL-3.0, matching HothouseExamples.
NeuralAmpModelerCore, nam-binary-loader, libDaisy and DaisySP retain their own
licenses. NAM captures and cabinet IRs are not included; users must supply
assets they are licensed to use.
