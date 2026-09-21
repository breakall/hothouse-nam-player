#!/bin/sh
set -eu

firmware=${1:-firmware/build/hothouse_nam.bin}
flash_address=${2:-0x90040000}
if [ ! -f "$firmware" ]; then
    echo "Firmware not found: $firmware" >&2
    exit 2
fi

echo "Waiting up to 120 seconds for the Daisy bootloader..."
attempt=0
while [ "$attempt" -lt 600 ]; do
    # Match the STM32 ROM bootloader itself.  The alternate target names vary
    # between dfu-util/macOS versions (for example, "@Internal Flash"), so do
    # not require a particular target-name spelling here.
    if dfu-util -l 2>&1 | grep -q '\[0483:df11\]'; then
        echo "Daisy bootloader found; flashing $firmware"
        exec dfu-util -a 0 -s "$flash_address:leave" -D "$firmware" -d ,0483:df11
    fi
    attempt=$((attempt + 1))
    sleep 0.2
done

echo "Timed out waiting for the Daisy bootloader" >&2
exit 1
