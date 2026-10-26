#!/usr/bin/env bash
# Boots a disc image in headless Flycast for a fixed time and prints the serial console.
# Runs inside the q64dc-flycast image: scripts/flycast.ps1 or scripts/flycast.sh.
# Usage: run_flycast.sh [disc image] [seconds] [screenshot interval in seconds, 0 for none]
#
# Screenshots go to build/screens/: shot_<seconds>.png every interval, and <name>.png whenever the
# console prints "SHOT <name>" (the test scripts' `shot` command). Flycast's settings and VMU
# images are kept in build/flycast-home, so saves survive between runs; RESET_VMU=1 starts with
# blank VMUs. AUDIO_DUMP=<path under the repo> records the audio output as raw PCM.
set -euo pipefail

DISC="${1:-build/quest64-dc.cdi}"
SECONDS_TO_RUN="${2:-60}"
SHOT_INTERVAL="${3:-0}"

export HOME="$PWD/build/flycast-home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_DATA_HOME="$HOME/.local/share"
AUDIO_BACKEND=null
if [ -n "${AUDIO_DUMP:-}" ]; then
    # SDL's disk driver writes Flycast's output (44.1 kHz stereo s16) to a raw file in real time.
    AUDIO_BACKEND=sdl2
    export SDL_AUDIODRIVER=disk
    export SDL_DISKAUDIOFILE="$PWD/$AUDIO_DUMP"
    mkdir -p "$(dirname "$SDL_DISKAUDIOFILE")"
    echo "[run_flycast] recording audio to $AUDIO_DUMP"
else
    export SDL_AUDIODRIVER=dummy
fi
mkdir -p "$XDG_CONFIG_HOME/flycast" "$XDG_DATA_HOME/flycast" build/screens
if [ "${RESET_VMU:-0}" = "1" ]; then
    find "$HOME" -name '*vmu_save_*.bin' -delete
    echo "[run_flycast] VMUs reset"
fi

cat > "$XDG_CONFIG_HOME/flycast/emu.cfg" <<EOF
[config]
Debug.SerialConsoleEnabled = yes
Dynarec.Enabled = yes
pvr.rend = ${FLYCAST_REND:-0}
rend.Resolution = 480
audio.backend = ${AUDIO_BACKEND}

[window]
fullscreen = yes
width = 640
height = 480
EOF
# NO_VMU=1: a controller with nothing in its slots (Flycast's MDT_None is 10).
if [ "${NO_VMU:-0}" = "1" ]; then
    printf '\n[input]\ndevice1.1 = 10\ndevice1.2 = 10\n' >> "$XDG_CONFIG_HOME/flycast/emu.cfg"
    echo "[run_flycast] no VMU"
fi

export DISPLAY=:99
Xvfb :99 -screen 0 640x480x24 -nolisten tcp > /dev/null 2>&1 &
xvfb_pid=$!
sleep 1

rm -f build/screens/shot_*.png

# The console is line-buffered so SHOT lines are seen as they happen.
stdbuf -oL -eL /opt/flycast/AppRun "$DISC" 2>&1 | while IFS= read -r line; do
    line="${line%$'\r'}"
    printf '%s\n' "$line"
    case "$line" in
        *"[q64dc] SHOT "*)
            name="${line##*SHOT }"
            name="${name//[^A-Za-z0-9_-]/_}"
            import -window root "build/screens/$name.png" 2> /dev/null || true
            ;;
    esac
done &
flycast_pid=$!

if [ "$SHOT_INTERVAL" -gt 0 ]; then
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
pkill -f flycast 2> /dev/null || true
sleep 1
kill "$xvfb_pid" 2> /dev/null || true
wait 2> /dev/null || true
