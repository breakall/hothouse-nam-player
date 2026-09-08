#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
from typing import Any


KERNEL_SIZES = [6] * 14 + [15, 15] + [6] * 7
DILATIONS = [
    1, 3, 7, 17, 41, 101, 239,
    1, 3, 7, 17, 41, 101, 239,
    1, 13,
    1, 3, 7, 17, 41, 101, 239,
]
WEIGHT_COUNT = 1871


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def inactive_film(value: Any) -> bool:
    return isinstance(value, dict) and value.get("active") is False


def select_a2_lite(document: dict[str, Any]) -> tuple[dict[str, Any], int]:
    if document.get("architecture") == "SlimmableContainer":
        submodels = document.get("config", {}).get("submodels", [])
        require(bool(submodels), "SlimmableContainer has no submodels")
        model = submodels[0].get("model")
        require(isinstance(model, dict), "Submodel 0 is missing its model")
        return model, 0
    return document, -1


def validate_model(model: dict[str, Any]) -> list[float]:
    require(model.get("architecture") == "WaveNet", "A2-Lite submodel must be WaveNet")
    require(float(model.get("sample_rate", 0.0)) == 48000.0, "A2-Lite model must be 48 kHz")

    layers = model.get("config", {}).get("layers")
    require(isinstance(layers, list) and len(layers) == 1, "Expected one WaveNet layer stack")
    config = layers[0]

    require(config.get("input_size") == 1, "Expected input_size=1")
    require(config.get("condition_size") == 1, "Expected condition_size=1")
    require(config.get("channels") == 3, "Expected 3-channel A2-Lite model")
    require(config.get("bottleneck") == 3, "Expected bottleneck=3")
    require(config.get("kernel_sizes") == KERNEL_SIZES, "Unsupported A2 kernel layout")
    require(config.get("dilations") == DILATIONS, "Unsupported A2 dilation layout")

    head = config.get("head", {})
    require(
        head.get("out_channels") == 1
        and head.get("kernel_size") == 16
        and head.get("bias") is True,
        "Expected a biased 16-tap mono head",
    )
    require(config.get("layer1x1") == {"active": True, "groups": 1}, "Unsupported layer1x1")
    require(config.get("head1x1", {}).get("active") is False, "head1x1 must be inactive")
    require(config.get("groups_input") == 1, "groups_input must be 1")
    require(config.get("groups_input_mixin") == 1, "groups_input_mixin must be 1")

    activations = config.get("activation", [])
    require(len(activations) == 23, "Expected 23 activations")
    require(
        all(
            activation.get("type") == "LeakyReLU"
            and float(activation.get("negative_slope", -1.0)) == 0.01
            for activation in activations
        ),
        "A2-Lite runtime requires LeakyReLU with slope 0.01",
    )
    require(config.get("gating_mode") == ["none"] * 23, "Gating is unsupported")
    require(config.get("secondary_activation") == [None] * 23, "Secondary activations are unsupported")

    film_keys = [
        "conv_pre_film",
        "conv_post_film",
        "input_mixin_pre_film",
        "input_mixin_post_film",
        "activation_pre_film",
        "activation_post_film",
        "layer1x1_post_film",
        "head1x1_post_film",
    ]
    require(all(inactive_film(config.get(key)) for key in film_keys), "FiLM layers are unsupported")

    weights = model.get("weights")
    require(isinstance(weights, list) and len(weights) == WEIGHT_COUNT,
            f"Expected exactly {WEIGHT_COUNT} A2-Lite weights")
    values = [float(value) for value in weights]
    require(all(math.isfinite(value) for value in values), "Weights must all be finite")
    return values


def cpp_float(value: float) -> str:
    text = format(value, ".9g")
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def emit_header(path: pathlib.Path) -> str:
    document = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(document, dict), "NAM file root must be an object")
    model, submodel_index = select_a2_lite(document)
    weights = validate_model(model)

    metadata = document.get("metadata", {})
    model_metadata = model.get("metadata", {})
    name = metadata.get("name") or model_metadata.get("name") or path.stem
    quoted_name = json.dumps(str(name), ensure_ascii=True)
    rows = []
    for index in range(0, len(weights), 6):
        rows.append("  " + ", ".join(cpp_float(value) for value in weights[index:index + 6]) + ",")

    return f"""#pragma once

#include <cstddef>

namespace embedded_a2_model
{{
inline constexpr char kName[] = {quoted_name};
inline constexpr int kSourceSubmodel = {submodel_index};
alignas(32) inline constexpr float kWeights[] = {{
{chr(10).join(rows)}
}};
inline constexpr std::size_t kWeightCount = sizeof(kWeights) / sizeof(kWeights[0]);
static_assert(kWeightCount == {WEIGHT_COUNT}, "Unexpected A2-Lite weight count");
}} // namespace embedded_a2_model
"""


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate and embed the 3-channel A2-Lite submodel from a NAM file."
    )
    parser.add_argument("model", type=pathlib.Path, help="A2 .nam model")
    parser.add_argument("-o", "--output", type=pathlib.Path, help="Output header; defaults to stdout")
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
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"A2 model rejected: {error}", file=sys.stderr)
        raise SystemExit(2)
