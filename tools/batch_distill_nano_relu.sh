#!/bin/sh
set -eu

usage() {
  echo "Usage: $0 TEACHERS_DIR WORK_DIR [EPOCHS] [LIMIT]" >&2
  exit 2
}

[ "$#" -ge 2 ] && [ "$#" -le 4 ] || usage

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
teachers_dir=$1
work_dir=$2
epochs=${3:-100}
limit=${4:-}
nam_source="$work_dir/neural-amp-modeler"
venv="$work_dir/.venv"
input_wav="$work_dir/assets/input.wav"
loader="$project_root/nam-pedal/nam-binary-loader"
loader_build="$loader/build-release"
# NAM 0.10.0 exports file format 0.5.4, matching the proven Daisy runtime.
nam_commit=28f9e2e94c2b50101c2a2a24cc0cf8a7693f2267
input_md5=36cd1af62985c2fac3e654333e36431e

[ -d "$teachers_dir" ] || { echo "Missing teachers directory: $teachers_dir" >&2; exit 2; }
mkdir -p "$work_dir/assets"

if [ ! -d "$nam_source/.git" ]; then
  git clone https://github.com/sdatkinson/neural-amp-modeler.git "$nam_source"
fi
git -C "$nam_source" fetch origin "$nam_commit"
git -C "$nam_source" checkout --detach "$nam_commit"

if [ ! -f "$input_wav" ]; then
  curl -L \
    'https://drive.usercontent.google.com/download?id=1KbaS4oXXNEuh2aCPLwKrPdf5KFOjda8G&export=download&confirm=t' \
    -o "$input_wav"
fi
if command -v md5 >/dev/null 2>&1; then
  actual_md5=$(md5 -q "$input_wav")
else
  actual_md5=$(md5sum "$input_wav" | awk '{print $1}')
fi
[ "$actual_md5" = "$input_md5" ] || {
  echo "Training input checksum mismatch: $actual_md5" >&2
  exit 1
}

cmake -S "$loader" -B "$loader_build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNAM_CORE_PATH="$project_root/nam-pedal/NeuralAmpModelerCore"
cmake --build "$loader_build" --target nam2namb render_namb --parallel

if [ ! -x "$venv/bin/python" ]; then
  uv venv --python 3.11 "$venv"
fi
if [ ! -f "$venv/.hothouse-nam-$nam_commit" ]; then
  uv pip install --python "$venv/bin/python" "$nam_source" 'pytorch-lightning<=2.6.1' 'setuptools<81' requests
  touch "$venv/.hothouse-nam-$nam_commit"
fi
if ! "$venv/bin/python" -c 'import pkg_resources' >/dev/null 2>&1; then
  uv pip install --python "$venv/bin/python" 'setuptools<81'
fi
if ! "$venv/bin/python" -c 'import requests' >/dev/null 2>&1; then
  uv pip install --python "$venv/bin/python" requests
fi

set -- \
  "$venv/bin/python" "$project_root/tools/distill_nano_relu.py" \
  --teachers "$teachers_dir" \
  --workspace "$work_dir" \
  --input-wav "$input_wav" \
  --python "$venv/bin/python" \
  --trainer "$project_root/tools/run_nam_full_headless.py" \
  --nam2namb "$loader_build/nam2namb" \
  --renderer "$loader_build/render_namb" \
  --epochs "$epochs"

if [ -n "$limit" ]; then
  set -- "$@" --limit "$limit"
fi

export PYTORCH_ENABLE_MPS_FALLBACK=1
exec "$@"
