#!/usr/bin/env python3
"""Request the deterministic engine-only benchmark from experimental firmware."""
import argparse
import json

from install_capture import PedalPort


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    port = PedalPort(args.port)
    try:
        result = port.request("HNAM BENCH", timeout=15.0)
    finally:
        port.close()
    if not result or result[0] != "BENCH":
        raise RuntimeError(f"Unexpected benchmark response: {result}")
    fields = dict(value.split("=", 1) for value in result[1:])
    budget = int(fields["hz"]) * int(fields["frames"]) / 48000
    for key in ("mean", "p99", "max"):
        fields[key + "_cpu_percent"] = round(int(fields[key]) * 100 / budget, 3)
    fields["scope"] = "engine only, warm cache, IRQ excluded; not total audio callback"
    fields["stimulus"] = "xorshift32-v1 seed=0x48534e41, amplitude +/-0.1, warmup=32"
    print(json.dumps(fields, indent=2))


if __name__ == "__main__":
    main()
