# Runs a command in the Dreamcast toolchain container with this repo mounted at /src.
#   scripts\dc.ps1 make -j16
#   scripts\dc.ps1 bash
param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $Command)

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Command) { $Command = @("make") }
$inner = "source /opt/toolchains/dc/kos/environ.sh && " + ($Command -join " ")

$tty = if ([Console]::IsInputRedirected) { @() } else { @("-it") }
docker run --rm @tty -v "${repo}:/src" -w /src q64dc-toolchain bash -c $inner
exit $LASTEXITCODE
