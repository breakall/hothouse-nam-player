#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import sys


def format_bytes(data: bytes) -> str:
    lines: list[str] = []
    for index in range(0, len(data), 12):
        chunk = data[index : index + 12]
        values = ", ".join(f"0x{byte:02x}" for byte in chunk)
        lines.append(f"  {values},")
    return "\n".join(lines)


def choose_raw_delimiter(text: str) -> str:
    delimiter = "json"
    while f'){delimiter}\"' in text:
        delimiter += "_x"
    return delimiter


def emit_header(model_path: pathlib.Path) -> str:
    suffix = model_path.suffix.lower()
    name = model_path.stem

    if suffix == ".namb":
        data = model_path.read_bytes()
        payload = format_bytes(data)
        return f"""#pragma once

#include <cstddef>
#include <cstdint>

namespace embedded_model
{{
constexpr bool kIsNamb = true;
constexpr const char kName[] = "{name}";
constexpr const char kNamJson[] = "";
constexpr uint8_t kNambData[] = {{
{payload}
}};
constexpr size_t kNambSize = sizeof(kNambData);
}} // namespace embedded_model
"""

    if suffix in {".nam", ".json"}:
        text = model_path.read_text(encoding="utf-8")
        delimiter = choose_raw_delimiter(text)
        return f"""#pragma once

#include <cstddef>
#include <cstdint>

namespace embedded_model
{{
constexpr bool kIsNamb = false;
constexpr const char kName[] = "{name}";
constexpr const char kNamJson[] = R"{delimiter}({text}){delimiter}";
constexpr uint8_t kNambData[] = {{0}};
constexpr size_t kNambSize = 0;
}} // namespace embedded_model
"""

    raise ValueError(f"Unsupported model type: {model_path.name}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate HothouseNAM embedded_model.h from a .namb or .nam file."
    )
    parser.add_argument("model", type=pathlib.Path, help="Path to .namb or .nam model")
    parser.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        help="Optional output path. Defaults to stdout.",
    )
    args = parser.parse_args()

    if not args.model.is_file():
        raise FileNotFoundError(args.model)

    header = emit_header(args.model)
    if args.output:
        args.output.write_text(header, encoding="utf-8")
    else:
        sys.stdout.write(header)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
