# Runs the disc image in headless Flycast (docker image q64dc-flycast).
# Usage: scripts\flycast.ps1 [seconds] [screenshot interval in seconds] [-ResetVmu]
# Screenshots go to build\screens, Flycast's VMU images stay in build\flycast-home between runs.
param([int]$Seconds = 60, [int]$ShotInterval = 0, [switch]$ResetVmu)

$repo = Split-Path -Parent $PSScriptRoot
$reset = if ($ResetVmu) { "1" } else { "0" }
docker run --rm -e RESET_VMU=$reset -v "${repo}:/src" -w /src q64dc-flycast bash scripts/run_flycast.sh build/quest64-dc.cdi $Seconds $ShotInterval
exit $LASTEXITCODE
