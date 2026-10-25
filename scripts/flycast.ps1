# Runs the disc image in headless Flycast (docker image q64dc-flycast).
# Usage: scripts\flycast.ps1 [seconds] [screenshot interval in seconds] [-Test] [-ResetVmu] [-Renderer n] [-AudioDump path] [-Disc path]
# -Test runs build\quest64-dc-test.cdi (make test-cdi) instead of build\quest64-dc.cdi; -Disc runs
# another image, e.g. build/eltale-reborn-test.cdi from a modded build. -Renderer
# is Flycast's pvr.rend: 0 OpenGL, 3 OpenGL per-pixel, 4 Vulkan, 5 Vulkan per-pixel. -AudioDump
# records the audio output as raw 44.1 kHz stereo s16 to a path under the repo (e.g. build/audio/run.raw).
# Screenshots go to build\screens, Flycast's VMU images stay in build\flycast-home between runs.
param([int]$Seconds = 60, [int]$ShotInterval = 0, [switch]$Test, [switch]$ResetVmu, [int]$Renderer = 0, [string]$AudioDump = "", [string]$Disc = "")

$repo = Split-Path -Parent $PSScriptRoot
$reset = if ($ResetVmu) { "1" } else { "0" }
$disc = if ($Disc) { $Disc.Replace("\", "/") } elseif ($Test) { "build/quest64-dc-test.cdi" } else { "build/quest64-dc.cdi" }
docker run --rm -e RESET_VMU=$reset -e FLYCAST_REND=$Renderer -e AUDIO_DUMP=$AudioDump -v "${repo}:/src" -w /src q64dc-flycast bash scripts/run_flycast.sh $disc $Seconds $ShotInterval
exit $LASTEXITCODE
