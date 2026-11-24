#!/usr/bin/env python3
"""Measure real full-chain callbacks using a STRESS_TEST=1, USE_IR=1 build."""

import argparse
import json
import math
import pathlib
import struct
import wave

from install_capture import ProtocolError, connect


def make_ir(path):
    """Dense maximum-length CPU fixture, not a cabinet sound or listening IR."""
    rng = 0x49525354
    values = []
    for i in range(1024):
        rng ^= (rng << 13) & 0xFFFFFFFF
        rng ^= rng >> 17
        rng ^= (rng << 5) & 0xFFFFFFFF
        values.append((1 if rng & 1 else -1) * math.exp(-i / 256))
    scale = 0.8 / sum(abs(value) for value in values)
    with wave.open(str(path), "wb") as wav:
        wav.setparams((1, 4, 48000, 0, "NONE", "not compressed"))
        wav.writeframes(b"".join(struct.pack("<i", round(value * scale * 2147483647))
                                for value in values))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--make-ir", type=pathlib.Path, help="write test WAV and exit")
    parser.add_argument("--port")
    parser.add_argument("--blocks", type=int, default=10000)
    parser.add_argument("--mode", choices=["live", "offline", "warm"], default="live")
    parser.add_argument("--reverbs", nargs="+", default=["reverbsc", "dattorro", "fdn16", "hybrid"],
                        choices=["reverbsc", "dattorro", "fdn16", "hybrid"])
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.make_ir:
        make_ir(args.make_ir)
        return
    if not 1000 <= args.blocks <= 60000 or args.output is None:
        parser.error("provide --output and --blocks between 1000 and 60000")
    port, info = connect(args.port)
    record = {"info": info, "runs": [], "mode": args.mode, "scope":
              "Full AudioCallback, 48 kHz/48 frames, NAM + dense 1024-tap FIR + EQ + stereo reverb. "
              "Includes stimulus generation, output checks and muting. IRQs enabled; "
              "excludes libDaisy DMA conversion and IRQ entry/exit. 2000 warmup blocks; "
              "xorshift32 seed 0x48534e41, +/-0.1; unity gains, flat EQ, 50% reverb mix; "
              "factory reverb parameters decay=.62/damping=.42/size=.72/modulation=.25. "
              "Synthetic IR measures CPU cost, not cabinet audio quality."}
    if args.mode == "offline":
        record["scope"] += " OFFLINE: audio stopped; callback called synchronously, IRQs enabled; not a real-time deadline test."
    elif args.mode == "warm":
        record["scope"] += " WARM: real SAI IRQ, entire chain primed for 2000 blocks before SAI starts; prime records the priming peak."
    else:
        record["scope"] += " LIVE: real SAI callback IRQ, including cold startup."
    try:
        record["engine"] = port.request("HNAM ENGINE")[1]
        record["reverb_before"] = port.request("HNAM REVERB INFO")
        record["slots_before"] = [port.request(f"HNAM SLOT {slot}") for slot in "ABC"]
        clock = port.request("HNAM BENCH", timeout=20)
        record["engine_reference"] = dict(field.split("=", 1) for field in clock[1:])
        hz = int(record["engine_reference"]["hz"])
        budget = hz * 48 / 48000
        for name in args.reverbs:
            print(f"Testing {name}: {args.blocks} blocks after 2000 warmup blocks", flush=True)
            command = {"live": "STRESS", "offline": "STRESSOFF", "warm": "STRESSWARM"}[args.mode]
            reply = port.request(f"HNAM {command} {name} {args.blocks}",
                                 timeout=(args.blocks + 2000) / 500 + 15)
            if reply[0] != "STRESS":
                raise ProtocolError(f"Unexpected response: {reply}")
            fields = dict(field.split("=", 1) for field in reply[1:])
            for key in ("n", "mean", "p99", "max", "warm", "over", "bad", "ms", "restore"):
                fields[key] = int(fields[key])
            if "prime" in fields:
                fields["prime"] = int(fields["prime"])
            fields["peak_output"] = struct.unpack("!f", bytes.fromhex(fields["peak"]))[0]
            fields["hz"] = hz
            fields["budget_cycles"] = budget
            for key in ("mean", "p99", "max", "warm"):
                fields[key + "_cpu_percent"] = fields[key] * 100 / budget
            fields["worst_headroom_percent"] = 100 - max(fields["max"], fields["warm"]) * 100 / budget
            fields["passed"] = (fields["n"] == args.blocks and fields["over"] == 0
                                and fields["bad"] == 0 and fields["restore"] == 1
                                and fields["peak_output"] > 0
                                and fields["crc"] == record["engine_reference"]["crc"])
            record["runs"].append(fields)
            args.output.write_text(json.dumps(record, indent=2) + "\n")
            print(json.dumps(fields), flush=True)
        record["reverb_after"] = port.request("HNAM REVERB INFO")
        record["slots_after"] = [port.request(f"HNAM SLOT {slot}") for slot in "ABC"]
        record["settings_restored"] = (record["reverb_before"] == record["reverb_after"]
                                       and record["slots_before"] == record["slots_after"])
        args.output.write_text(json.dumps(record, indent=2) + "\n")
        if not record["settings_restored"] or not all(run["passed"] for run in record["runs"]):
            raise SystemExit("Stress test failed; see saved results")
    finally:
        port.close()


if __name__ == "__main__":
    main()
