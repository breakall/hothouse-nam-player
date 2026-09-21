#!/usr/bin/env python3
"""Convert a 48 kHz PCM WAV cabinet IR into a C++ header."""

import argparse
import struct
import wave
from pathlib import Path


def decode_sample(data: bytes, width: int) -> float:
    if width == 1:
        return (data[0] - 128) / 128.0
    if width == 2:
        return struct.unpack("<h", data)[0] / 32768.0
    if width == 3:
        value = int.from_bytes(data, "little", signed=False)
        if value & 0x800000:
            value -= 1 << 24
        return value / 8388608.0
    if width == 4:
        return struct.unpack("<i", data)[0] / 2147483648.0
    raise ValueError(f"unsupported PCM sample width: {width * 8} bits")


def load_wav(path: Path, max_taps: int) -> tuple[list[float], int]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_rate = wav.getframerate()
        width = wav.getsampwidth()
        compression = wav.getcomptype()
        if compression != "NONE":
            raise ValueError("compressed WAV files are not supported")
        if sample_rate != 48000:
            raise ValueError(f"IR must be 48000 Hz, got {sample_rate} Hz")
        if channels < 1:
            raise ValueError("IR has no audio channels")

        frame_count = min(wav.getnframes(), max_taps)
        raw = wav.readframes(frame_count)

    stride = channels * width
    samples = []
    for offset in range(0, len(raw), stride):
        frame = raw[offset : offset + stride]
        if len(frame) != stride:
            break
        channel_values = [
            decode_sample(frame[channel * width : (channel + 1) * width], width)
            for channel in range(channels)
        ]
        samples.append(sum(channel_values) / channels)

    if not samples:
        raise ValueError("IR contains no samples")
    return samples, sample_rate


def cpp_float(value: float) -> str:
    text = f"{value:.9g}"
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def write_header(path: Path, name: str, samples: list[float], sample_rate: int) -> None:
    values = ",\n    ".join(cpp_float(value) for value in samples)
    text = f"""#pragma once

#include <cstddef>

namespace embedded_ir
{{
inline constexpr char kName[] = \"{name}\";
inline constexpr unsigned kSampleRate = {sample_rate};
inline constexpr float kData[] = {{
    {values}
}};
inline constexpr std::size_t kLength = sizeof(kData) / sizeof(kData[0]);
}} // namespace embedded_ir
"""
    path.write_text(text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path, help="48 kHz PCM WAV impulse response")
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--max-taps", type=int, default=1024)
    args = parser.parse_args()
    if args.max_taps < 1 or args.max_taps > 1024:
        parser.error("--max-taps must be between 1 and 1024")

    try:
        samples, sample_rate = load_wav(args.wav, args.max_taps)
    except (OSError, ValueError, wave.Error) as error:
        parser.error(str(error))
    write_header(args.output, args.wav.stem, samples, sample_rate)
    print(f"Embedded {len(samples)} taps from {args.wav} into {args.output}")


if __name__ == "__main__":
    main()
