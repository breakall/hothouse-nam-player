#!/bin/sh
set -u

usage() {
  echo "Usage: $0 INPUT_DIR [OUTPUT_DIR]" >&2
  echo "Recursively converts every .nam file to .namb." >&2
  exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
input_dir=$1
output_dir=${2:-"$input_dir/namb"}
loader_dir="$project_root/nam-pedal/nam-binary-loader"
build_dir="$loader_dir/build"
converter="$build_dir/nam2namb"

[ -d "$input_dir" ] || {
  echo "Input directory does not exist: $input_dir" >&2
  exit 2
}

if [ ! -x "$converter" ]; then
  echo "Building nam2namb..."
  cmake -S "$loader_dir" -B "$build_dir" \
    -DNAM_CORE_PATH="$project_root/nam-pedal/NeuralAmpModelerCore"
  cmake --build "$build_dir" --target nam2namb --parallel
fi

mkdir -p "$output_dir"

manifest=$(mktemp "${TMPDIR:-/tmp}/hothouse-nam-files.XXXXXX")
trap 'rm -f "$manifest"' EXIT HUP INT TERM
find "$input_dir" -type f \( -iname '*.nam' -o -iname '*.json' \) \
  ! -path "$output_dir/*" -print0 > "$manifest"

if [ ! -s "$manifest" ]; then
  echo "No .nam or .json models found under: $input_dir" >&2
  exit 1
fi

converted=0
failed=0

while IFS= read -r -d '' source; do
  relative=${source#"$input_dir"/}
  relative_no_ext=${relative%.*}
  destination="$output_dir/$relative_no_ext.namb"
  mkdir -p "$(dirname -- "$destination")"

  echo "Converting: $relative"
  if "$converter" "$source" "$destination"; then
    size=$(wc -c < "$destination" | tr -d ' ')
    echo "  -> ${destination#"$output_dir"/} ($size bytes)"
    converted=$((converted + 1))
  else
    echo "  FAILED: $relative" >&2
    rm -f "$destination"
    failed=$((failed + 1))
  fi
done < "$manifest"

echo "Converted: $converted; failed: $failed; output: $output_dir"
[ "$failed" -eq 0 ]
