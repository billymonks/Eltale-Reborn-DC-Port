#!/usr/bin/env bash
# Builds the Eltale Reborn disc from the .nrm packages in eltale/ (story and gameplay).
#   scripts/mod.sh
#   scripts/mod.sh make -j16 cdi
#   scripts/mod.sh make -j16 test-cdi TEST_SCRIPT=tests/mod_story.txt
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
mod="eltale,gameplay"

if [ $# -eq 0 ]; then
    set -- make -j16 cdi
fi

"$here/dc.sh" python3 scripts/stage_mods.py "$mod"
"$here/dc.sh" "$@" "MOD=$mod"
