#!/usr/bin/env bash
# Runs the disc image in headless Flycast (docker image q64dc-flycast).
# Usage: [RESET_VMU=1] scripts/flycast.sh [seconds] [screenshot interval in seconds]
# Screenshots go to build/screens, Flycast's VMU images stay in build/flycast-home between runs.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
docker run --rm -e RESET_VMU="${RESET_VMU:-0}" -v "$repo:/src" -w /src q64dc-flycast \
    bash scripts/run_flycast.sh build/quest64-dc.cdi "${1:-60}" "${2:-0}"
