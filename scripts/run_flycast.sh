#!/usr/bin/env bash
# Boots a disc image in headless Flycast for a fixed time and prints the serial console.
# Runs inside the q64dc-flycast image: scripts/flycast.ps1 or scripts/flycast.sh.
# Usage: run_flycast.sh [disc image] [seconds] [screenshot interval in seconds, 0 for none]
# Screenshots go to build/screens/.
set -euo pipefail

DISC="${1:-build/quest64-dc.cdi}"
SECONDS_TO_RUN="${2:-60}"
SHOT_INTERVAL="${3:-0}"

export HOME=/tmp/flycast-home
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_DATA_HOME="$HOME/.local/share"
export SDL_AUDIODRIVER=dummy
mkdir -p "$XDG_CONFIG_HOME/flycast" "$XDG_DATA_HOME/flycast"

cat > "$XDG_CONFIG_HOME/flycast/emu.cfg" <<'EOF'
[config]
Debug.SerialConsoleEnabled = yes
Dynarec.Enabled = yes
pvr.rend = 0
rend.Resolution = 480
audio.backend = null

[window]
fullscreen = yes
width = 640
height = 480
EOF

export DISPLAY=:99
Xvfb :99 -screen 0 640x480x24 -nolisten tcp > /dev/null 2>&1 &
xvfb_pid=$!
sleep 1

/opt/flycast/AppRun "$DISC" &
flycast_pid=$!

if [ "$SHOT_INTERVAL" -gt 0 ]; then
    mkdir -p build/screens
    rm -f build/screens/shot_*.png
    (
        n=0
        while sleep "$SHOT_INTERVAL"; do
            n=$((n + SHOT_INTERVAL))
            import -window root "build/screens/shot_$(printf %04d "$n").png" 2> /dev/null || true
        done
    ) &
    shots_pid=$!
fi

sleep "$SECONDS_TO_RUN"
echo "[run_flycast] stopped after ${SECONDS_TO_RUN}s"
[ -n "${shots_pid:-}" ] && kill "$shots_pid" 2> /dev/null || true
kill -9 "$flycast_pid" 2> /dev/null || true
kill "$xvfb_pid" 2> /dev/null || true
wait 2> /dev/null || true
