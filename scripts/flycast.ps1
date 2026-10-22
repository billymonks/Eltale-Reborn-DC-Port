# Runs the disc image in headless Flycast (docker image q64dc-flycast).
# Usage: scripts\flycast.ps1 [seconds]
param([int]$Seconds = 60)

$repo = Split-Path -Parent $PSScriptRoot
docker run --rm -v "${repo}:/src" -w /src q64dc-flycast bash scripts/run_flycast.sh build/quest64-dc.cdi $Seconds
exit $LASTEXITCODE
