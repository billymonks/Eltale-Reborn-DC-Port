#!/usr/bin/env bash
# Builds the host N64Recomp from the pinned submodule (N64Recomp-DC, which has this project's generator changes:
# gpr_32bit, return hooks, the [codegen] options).
# usage: build_n64recomp.sh <build dir>
set -euo pipefail

build_dir="$(realpath -m "$1")"
src=tools/N64Recomp

git config --global --add safe.directory '*' 2>/dev/null || true

if [ ! -f "$src/CMakeLists.txt" ] || [ ! -e "$src/lib/rabbitizer/.git" ]; then
    git submodule update --init --recursive tools/N64Recomp
fi
if ! grep -q fpr_abi_sync "$src/src/config.cpp"; then
    echo "tools/N64Recomp is older than the pinned commit (no [codegen] options): git submodule update --init --recursive" >&2
    exit 1
fi

cmake -S "$src" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target N64Recomp
