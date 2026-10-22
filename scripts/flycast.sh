#!/usr/bin/env bash
# Runs the disc image in headless Flycast (docker image q64dc-flycast).
# Usage: scripts/flycast.sh [seconds]
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
docker run --rm -v "$repo:/src" -w /src q64dc-flycast bash scripts/run_flycast.sh build/quest64-dc.cdi "${1:-60}"
