#!/usr/bin/env bash
# Runs a command in the Dreamcast toolchain container with this repo mounted at /src.
#   scripts/dc.sh make -j16
#   scripts/dc.sh bash
# Uses q64dc-toolchain-arm (docker/arm: the same plus the AICA's ARM compiler) when it exists,
# otherwise q64dc-toolchain. Q64DC_IMAGE picks another.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -eq 0 ] && set -- make

image="${Q64DC_IMAGE:-}"
if [ -z "$image" ]; then
    image=q64dc-toolchain
    docker image inspect q64dc-toolchain-arm > /dev/null 2>&1 && image=q64dc-toolchain-arm
fi
tty=()
[ -t 0 ] && tty=(-it)
exec docker run --rm "${tty[@]}" -v "$repo:/src" -w /src "$image" \
    bash -c "source /opt/toolchains/dc/kos/environ.sh && $*"
