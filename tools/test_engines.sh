#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
dependency=external/tone3000-embedded
engine="$dependency/engines/cortex-m7/a2lite"
runtime=external/DaisySeedProjects/Software/GuitarPedal/Effect-Modules/Nam
build=firmware/build/engine-golden
mkdir -p "$build"
"${CC:-cc}" -O2 -DNAM_A2LITE_MAX_BUFFER_SIZE=48 -I"$engine" \
  -c "$engine/nam_a2lite.c" -o "$build/nam_a2lite.o"
"${CC:-cc}" -O2 -I"$dependency/namb" -c "$dependency/namb/namb_reader.c" \
  -o "$build/namb_reader.o"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  -DNAM_A2LITE_MAX_BUFFER_SIZE=48 -I"$runtime" -I"$engine" -I"$dependency/namb" \
  firmware/tests/engine_golden_test.cpp "$build/nam_a2lite.o" \
  "$build/namb_reader.o" -o "$build/engine_golden_test"
"$build/engine_golden_test" "$dependency/weights/a2lite.namb" \
  "$dependency/test/golden_input.raw" "$dependency/test/golden_a2lite_output.raw"
