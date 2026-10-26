# Runs a command in the Dreamcast toolchain container with this repo mounted at /src.
#   scripts\dc.ps1 make -j16
#   scripts\dc.ps1 bash
# Uses q64dc-toolchain-arm (docker/arm: the same plus the AICA's ARM compiler) when it exists,
# otherwise q64dc-toolchain. $env:Q64DC_IMAGE picks another.
param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $Command)

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Command) { $Command = @("make") }
$inner = "source /opt/toolchains/dc/kos/environ.sh && " + ($Command -join " ")

$image = $env:Q64DC_IMAGE
if (-not $image) {
    docker image inspect q64dc-toolchain-arm *> $null
    $image = if ($LASTEXITCODE -eq 0) { "q64dc-toolchain-arm" } else { "q64dc-toolchain" }
}
$tty = if ([Console]::IsInputRedirected) { @() } else { @("-it") }
docker run --rm @tty -v "${repo}:/src" -w /src $image bash -c $inner
exit $LASTEXITCODE
