# Builds a modded disc: runs the mods' own build on the host (scripts/stage_mods.py), then the
# Dreamcast build in the toolchain container with MOD set.
#   scripts\mod.ps1                          eltale, make -j16 cdi
#   scripts\mod.ps1 -Mod eltale,gameplay make -j24 cdi
#   scripts\mod.ps1 -ModDir D:\mods\Quest-64-Good-Edition make -j16 test-cdi TEST_SCRIPT=tests/x.txt
#   scripts\mod.ps1 -NoBuild ...             stage the mod repo's existing .nrm without rebuilding it
[CmdletBinding(PositionalBinding = $false)]
param(
    [string] $Mod = "eltale",
    [string] $ModDir = "",
    [switch] $NoBuild,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $Make
)

$repo = Split-Path -Parent $PSScriptRoot
if (-not $ModDir) { $ModDir = if ($env:MOD_DIR) { $env:MOD_DIR } else { Join-Path (Split-Path -Parent $repo) "Quest-64-Good-Edition" } }
if (-not $Make) { $Make = @("make", "-j16", "cdi") }

$stage = @("$repo\scripts\stage_mods.py", $Mod, "--mod-dir", $ModDir)
if ($NoBuild) { $stage += "--no-build" }
python @stage
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\dc.ps1" @Make "MOD=$Mod"
exit $LASTEXITCODE
