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
rom_dfu_warned=0
while [ "$attempt" -lt 600 ]; do
    # The STM32 ROM bootloader and the Daisy bootloader share VID:PID
    # 0483:df11. Only the Daisy bootloader exposes the external QSPI address
    # range beginning at 0x90000000; ROM DFU exposes internal flash only.
    dfu_listing=$(dfu-util -l 2>&1 || :)
    if printf '%s\n' "$dfu_listing" | grep -q '\[0483:df11\]' \
        && printf '%s\n' "$dfu_listing" | grep -q '0x900'; then
        echo "Daisy bootloader found; flashing $firmware"
        exec dfu-util -a 0 -s "$flash_address:leave" -D "$firmware" -d ,0483:df11
    fi
    if [ "$rom_dfu_warned" -eq 0 ] \
        && printf '%s\n' "$dfu_listing" | grep -q '\[0483:df11\]' \
        && printf '%s\n' "$dfu_listing" | grep -q 'Internal Flash'; then
        echo "STM32 ROM DFU found, but it cannot write Daisy QSPI; install or enter the Daisy bootloader." >&2
        rom_dfu_warned=1
    fi
    attempt=$((attempt + 1))
    sleep 0.2
done

echo "Timed out waiting for the Daisy bootloader" >&2
exit 1
