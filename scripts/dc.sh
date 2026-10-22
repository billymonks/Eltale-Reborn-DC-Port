#!/usr/bin/env bash
# Runs a command in the Dreamcast toolchain container with this repo mounted at /src.
#   scripts/dc.sh make -j16
#   scripts/dc.sh bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -eq 0 ] && set -- make

tty=()
[ -t 0 ] && tty=(-it)
exec docker run --rm "${tty[@]}" -v "$repo:/src" -w /src q64dc-toolchain \
    bash -c "source /opt/toolchains/dc/kos/environ.sh && $*"
