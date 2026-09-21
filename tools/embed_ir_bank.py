#!/usr/bin/env python3
"""Convert one or two 48 kHz PCM WAV cabinet IRs into a C++ bank."""

import argparse
import json
import wave
from pathlib import Path

from embed_ir import cpp_float, load_wav


def write_header(
    output: Path, entries: list[tuple[str, list[float]]], sample_rate: int
) -> None:
    arrays = []
    bank_entries = []
    for index, (name, samples) in enumerate(entries):
        values = ",\n    ".join(cpp_float(value) for value in samples)
        arrays.append(
            f"inline constexpr float kData{index}[] = {{\n    {values}\n}};"
        )
        quoted_name = json.dumps(name, ensure_ascii=True)
        bank_entries.append(
            f"    {{{quoted_name}, kData{index}, sizeof(kData{index}) / sizeof(kData{index}[0])}}"
        )

    text = f"""#pragma once

#include <cstddef>

namespace embedded_ir_bank
{{
inline constexpr unsigned kSampleRate = {sample_rate};
inline constexpr std::size_t kMaximumCount = 2;

struct Entry
{{
    const char* name;
    const float* data;
    std::size_t length;
}};

{chr(10).join(arrays)}

inline constexpr Entry kEntries[] = {{
{',\n'.join(bank_entries)}
}};
inline constexpr std::size_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);
}} // namespace embedded_ir_bank
"""
    output.write_text(text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wavs", nargs="+", type=Path, help="one or two IR WAVs")
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--max-taps", type=int, default=1024)
    args = parser.parse_args()
    if not 1 <= len(args.wavs) <= 2:
        parser.error("provide one or two IR WAV files")
    if not 1 <= args.max_taps <= 1024:
        parser.error("--max-taps must be between 1 and 1024")

    entries: list[tuple[str, list[float]]] = []
    sample_rate = 48000
    try:
        for wav in args.wavs:
            samples, sample_rate = load_wav(wav, args.max_taps)
            entries.append((wav.stem.replace('"', "'"), samples))
    except (OSError, ValueError, wave.Error) as error:
        parser.error(str(error))

    write_header(args.output, entries, sample_rate)
    print(f"Embedded {len(entries)} cabinet IR(s) into {args.output}")


if __name__ == "__main__":
    main()
