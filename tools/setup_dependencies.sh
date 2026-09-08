#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

git -C "$project_root" submodule update --init --recursive

apply_once() {
  repo=$1
  patch=$2
  if git -C "$repo" apply --check "$patch" 2>/dev/null; then
    git -C "$repo" apply "$patch"
  elif git -C "$repo" apply --reverse --check "$patch" 2>/dev/null; then
    echo "Already applied: $patch"
  else
    echo "Patch does not apply cleanly: $patch" >&2
    exit 1
  fi
}

apply_once \
  "$project_root/nam-pedal/NeuralAmpModelerCore" \
  "$project_root/patches/neural-amp-modeler-core-embedded.patch"
apply_once \
  "$project_root/nam-pedal/nam-binary-loader" \
  "$project_root/patches/nam-binary-loader-embedded.patch"
