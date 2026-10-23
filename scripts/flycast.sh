#!/usr/bin/env bash
# Runs the disc image in headless Flycast (docker image q64dc-flycast).
# Usage: [TEST=1] [RESET_VMU=1] [FLYCAST_REND=n] [AUDIO_DUMP=path] [DISC=path] scripts/flycast.sh [seconds] [screenshot interval in seconds]
# TEST=1 runs build/quest64-dc-test.cdi (make test-cdi) instead of build/quest64-dc.cdi; DISC runs
# another image, e.g. build/quest64-eltale-test.cdi from a modded build. FLYCAST_REND
# is Flycast's pvr.rend: 0 OpenGL, 3 OpenGL per-pixel, 4 Vulkan, 5 Vulkan per-pixel. AUDIO_DUMP
# records the audio output as raw 44.1 kHz stereo s16 to a path under the repo (e.g. build/audio/run.raw).
# Screenshots go to build/screens, Flycast's VMU images stay in build/flycast-home between runs.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
disc=build/quest64-dc.cdi
[ "${TEST:-0}" = "1" ] && disc=build/quest64-dc-test.cdi
[ -n "${DISC:-}" ] && disc="$DISC"
docker run --rm -e RESET_VMU="${RESET_VMU:-0}" -e FLYCAST_REND="${FLYCAST_REND:-0}" -e AUDIO_DUMP="${AUDIO_DUMP:-}" \
    -v "$repo:/src" -w /src q64dc-flycast bash scripts/run_flycast.sh "$disc" "${1:-60}" "${2:-0}"
