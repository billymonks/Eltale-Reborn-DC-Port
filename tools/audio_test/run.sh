#!/usr/bin/env bash
# Builds and runs the audio HLE comparison on the host (in the toolchain container).
# usage: tools/audio_test/run.sh <Quest64-Recomp checkout> [seeds] [first seed]
# The checkout needs rsp/aspMain.cpp, which its RSPRecomp generates from the ROM (aspMain.toml).
set -euo pipefail

qr="$(realpath "$1")"
shift
rt="$qr/lib/N64ModernRuntime"
out=build/audio_test
mkdir -p "$out"

gcc -O2 -c -Iruntime/src runtime/src/audio_hle.c -o "$out/audio_hle.o"
g++ -std=c++20 -O1 -msse4.1 -w \
    -I"$rt/librecomp/include" -I"$rt/librecomp/include/librecomp" -I"$rt/N64Recomp/include" \
    -I"$rt/ultramodern/include" -Iruntime/src \
    tools/audio_test/abi_diff.cpp "$qr/rsp/aspMain.cpp" "$rt/librecomp/src/rsp.cpp" \
    "$out/audio_hle.o" -o "$out/abi_diff"
"$out/abi_diff" quest64.us.z64 "$@"
