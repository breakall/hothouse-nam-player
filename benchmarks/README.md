# TONE3000 benchmark report — 2026-11-24

Run the commands below from the repository root. Measurements describe the
recorded builds; later source cleanup does not change the archived identities.

## Engine comparison

TONE3000 is the production default. The original DaisySeedProjects runtime
remains available as `NAM_ENGINE=legacy` for comparisons and fallback. Both
implement the same `nam_engine.h` interface. Set up both dependencies to
reproduce the comparison:

```sh
make setup-tone3000
make setup-a2
make test-engines
make firmware NAM_ENGINE=legacy USE_IR=0
make firmware NAM_ENGINE=tone3000 USE_IR=0
```

Current builds write TONE3000 to `firmware/build/firmware/hothouse_nam.bin`
and legacy to `firmware/build/firmware-legacy/hothouse_nam.bin`. The historical
JSON records retain the paths used during the experiment. Both use BOOT_SRAM,
the same 48 kHz / 48-frame host, gain/effects code, capture slots, `a2_lite` USB identity
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

For target timing, flash each built binary using the [flashing procedure](../README.md#flash), keeping
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
the desired complete effect chain; the engine-only improvement does not
guarantee every IR/reverb combination meets the deadline.
Hardware comparison on 2026-11-24 used the same Hothouse / Daisy Seed at
480 MHz, slot B's `Hiwatt DR 103 Edgy Bridged` capture (CRC `e66f25f3`),
`USE_IR=0`, and five benchmark runs per engine:

| Engine-only metric, 48-frame block | Legacy | TONE3000 |
| --- | ---: | ---: |
| Median of five run means | 320,712 cycles | 191,182 cycles |
| Mean block time | 668.15 us | 398.30 us |
| Block budget used | 66.82% | 39.83% |
| Highest reported p99 | 321,097 cycles | 193,119 cycles |
| Highest observed block | 321,163 cycles | 193,739 cycles |

TONE3000 used **40.39% fewer engine cycles**, a **1.68x throughput ratio**,
freeing 26.99 percentage points of the 1 ms block budget. Each backend's output
checksum repeated across all five runs. All three capture-slot metadata
records, level matching, and reverb assignments matched before and after the
firmware swap; no capture reinstall was needed.

Brief live-input callback observations at the existing panel settings peaked
at 328,764 cycles (68.49%) for legacy and 199,859 cycles (41.64%) for TONE3000,
with the model ready. These are observations with `USE_IR=0`, not a stress test
of every reverb/IR combination. This compares the deployed memory placements
described above; it does not isolate the effect of kernel changes from ITCM
and SRAM placement. TONE3000 was left running on the pedal.

The [raw runs and build identities](2026-11-24-engine-comparison.json)
include binary SHA-256 hashes, capture CRCs, configuration, checksums, and live
callback samples.

### Full-chain stress diagnostic

An optional `STRESS_TEST=1` build exercises the unchanged product
`AudioCallback` with NAM, a dense 1,024-tap FIR, EQ and each stereo reverb.
The signal is deterministic +/-0.1 noise; gains are unity, EQ is flat, reverb
mix is 50%, and reverb parameters retain their factory values. Outputs are
muted during each test. Stimulus generation, output validation and muting are
included in the measured duration; libDaisy DMA conversion and IRQ entry/exit
are outside it. This synthetic IR measures CPU load, not cabinet sound quality.
The command temporarily replaces the reverb mapping in RAM and restores it
afterward without writing capture, reverb, level or preset flash data.

```sh
python3 tools/benchmark_chain.py --make-ir /tmp/stress-ir-1024.wav
make firmware NAM_ENGINE=tone3000 USE_IR=1 STRESS_TEST=1 \
  FIRMWARE_BUILD_DIR=build/firmware-tone3000-stress \
  IR_A=/tmp/stress-ir-1024.wav
# Start with direct USB access, then perform the two-footswitch gesture:
./tools/wait_and_flash_hothouse.sh firmware/build/firmware-tone3000-stress/hothouse_nam.bin
python3 tools/benchmark_chain.py --output /tmp/chain-cold.json
```

The initial test and one repeat on 2026-11-24 both tripped the deadline guard
on the first callback, before the 2,000-block warmup completed. Maximum cold
callback durations across those two passes were:

| Reverb with NAM + 1,024-tap IR | Cold callback | Budget used |
| --- | ---: | ---: |
| reverbsc | 498,353 cycles / 1,038 us | 103.82% |
| dattorro | 532,901 cycles / 1,110 us | 111.02% |
| fdn16 | 596,471 cycles / 1,243 us | 124.26% |
| hybrid | 494,449 cycles / 1,030 us | 103.01% |

These establish cold-start deadline failures, not steady-state means. The
guard bypasses the effect after an overrun; zero timed blocks in the result
means warmup failed, not that the chain costs zero cycles. Raw data is in
[the initial pass](2026-11-24-tone3000-chain-stress.json) and
[the repeat](2026-11-24-tone3000-chain-cold-repeat.json).

The diagnostic also supports `--mode offline` to time an overloaded chain
synchronously while audio is stopped, and `--mode warm` to prime the entire
chain before testing it in the real SAI interrupt. For example:

```sh
python3 tools/benchmark_chain.py --mode offline --output /tmp/chain-offline.json
python3 tools/benchmark_chain.py --mode warm --reverbs fdn16 --blocks 60000 \
  --output /tmp/chain-warm.json
```

Offline timing alone cannot establish real-time safety. Restore the normal
Tone3000 binary after diagnostics, since the test IR is not a listening asset.

The completed warmed offline pass measured 10,000 blocks per reverb after
2,000 warmup blocks, with the same Hiwatt capture and 480 MHz clock:

| Reverb with NAM + 1,024-tap IR | Mean budget used | Maximum budget used |
| --- | ---: | ---: |
| reverbsc | 104.29% | 106.94% |
| dattorro | 111.09% | 114.13% |
| fdn16 | 124.63% | 127.05% |
| hybrid | 102.99% | 104.13% |

Every callback in that pass, including warmup, exceeded 480,000 cycles.
FDN16 was the heaviest: mean 598,207 cycles (1,246 us), p99 605,126 cycles
(1,261 us), maximum 609,818 cycles (1,270 us). Thus there is no spare callback
budget at maximum IR length; its measured worst-case deficit is 27.05% of
the 1 ms deadline. These durations include the diagnostic wrapper overhead.

A separate prewarmed real-SAI-IRQ test requested 60,000 blocks but the guard
stopped it after its first measured block: 594,198 cycles (1,238 us, 123.79%).
The one-minute endurance test therefore did not run to completion. No invalid
floating-point outputs were observed, and capture/reverb settings matched
before and after every test. The normal `USE_IR=0`, `STRESS_TEST=0` Tone3000
binary was restored and verified over USB afterward.

See [offline results](2026-11-24-tone3000-chain-offline.json),
[live warmed confirmation](2026-11-24-tone3000-chain-warm.json), and
[build/restoration identities](2026-11-24-tone3000-stress-build.json).
