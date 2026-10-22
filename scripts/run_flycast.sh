#!/usr/bin/env bash
# Boots a disc image in headless Flycast for a fixed time and prints the serial console.
# Runs inside the q64dc-flycast image: scripts/flycast.ps1 or scripts/flycast.sh.
# Usage: run_flycast.sh [disc image] [seconds]
set -euo pipefail

DISC="${1:-build/quest64-dc.cdi}"
SECONDS_TO_RUN="${2:-60}"

export HOME=/tmp/flycast-home
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_DATA_HOME="$HOME/.local/share"
export SDL_AUDIODRIVER=dummy
mkdir -p "$XDG_CONFIG_HOME/flycast" "$XDG_DATA_HOME/flycast"

cat > "$XDG_CONFIG_HOME/flycast/emu.cfg" <<'EOF'
[config]
Debug.SerialConsoleEnabled = yes
Dreamcast.ContentPath =
Dynarec.Enabled = yes
pvr.rend = 0
rend.Resolution = 480
audio.backend = null
EOF

status=0
timeout --signal=KILL "$SECONDS_TO_RUN" \
    xvfb-run -a -s "-screen 0 640x480x24" /opt/flycast/AppRun "$DISC" || status=$?
if [ "$status" -eq 137 ]; then
    echo "[run_flycast] stopped after ${SECONDS_TO_RUN}s"
    status=0
fi
exit "$status"
