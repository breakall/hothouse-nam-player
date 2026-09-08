#!/bin/sh
set -eu

firmware=${1:-firmware/build/hothouse_nam.bin}
if [ ! -f "$firmware" ]; then
    echo "Firmware not found: $firmware" >&2
    exit 2
fi

echo "Waiting up to 120 seconds for the Daisy bootloader..."
attempt=0
while [ "$attempt" -lt 600 ]; do
    if dfu-util -l 2>&1 | grep -q '\[0483:df11\].*@Flash '; then
        echo "Daisy bootloader found; flashing $firmware"
        exec dfu-util -a 0 -s 0x90040000:leave -D "$firmware" -d ,0483:df11
    fi
    attempt=$((attempt + 1))
    sleep 0.2
done

echo "Timed out waiting for the Daisy bootloader" >&2
exit 1
