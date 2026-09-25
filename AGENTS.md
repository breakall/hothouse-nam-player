# Hothouse hardware workflow

These instructions apply whenever working with the connected Hothouse / Daisy
Seed from this repository.

## USB identity

- Normal Hothouse application mode is an Electrosmith `Daisy Seed Built In`
  CDC device with VID:PID `0483:5740`. The development pedal currently has USB
  serial number `346637623233` and normally appears as
  `/dev/cu.usbmodem3466376232331` on macOS.
- DFU mode uses VID:PID `0483:df11`. The Daisy bootloader and STM32 ROM
  bootloader share this VID:PID; use `dfu-util -l` target information to tell
  them apart. The Daisy bootloader exposes QSPI near `0x90000000`; application
  binaries are written at `0x90040000`.

## Required device permissions

- Opening `/dev/cu.usbmodem*` and enumerating or flashing with `dfu-util`
  requires direct hardware access outside the workspace sandbox. A device node
  may be visible while sandboxed even though opening it fails or `dfu-util -l`
  sees no device.
- Run serial tools and `tools/wait_and_flash_hothouse.sh` with escalated/direct
  device permission. If a sandboxed command reports no DFU device, do not infer
  that the pedal failed to enter DFU.

## Reliable flashing procedure

1. Build the requested firmware before asking the user to enter DFU.
2. Start `tools/wait_and_flash_hothouse.sh <binary>` with direct USB permission
   and keep the returned live process/session handle.
3. Only after the watcher is live, ask the user to hold both footswitches for at
   least two seconds. Three alternating LED flashes confirm the gesture.
4. Poll the same live process/session until `dfu-util` completes. Do not launch
   a detached watcher, discard its process handle, or use a private log file as
   proof of USB state.
5. Verify `Download done` and `File downloaded successfully`, then query the
   rebooted firmware over serial to confirm its reported backend.

The combined A1/A2 command is:

```sh
./tools/wait_and_flash_hothouse.sh firmware/build/combined/hothouse_nam.bin
```

Do not attribute a missed flash to the bootloader timeout without first running
an authorized/raw USB enumeration. On 2026-09-25, repeated misses were caused
by sandboxed `dfu-util`; the same footswitch gesture succeeded immediately once
the watcher had direct USB access.
