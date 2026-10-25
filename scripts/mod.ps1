# Builds the Eltale Reborn disc from the .nrm packages in eltale/ (story and gameplay).
#   scripts\mod.ps1                                              build/eltale-reborn.cdi
#   scripts\mod.ps1 make -j16 cdi
#   scripts\mod.ps1 make -j16 test-cdi TEST_SCRIPT=tests/mod_story.txt
[CmdletBinding(PositionalBinding = $false)]
param(
    [string] $Mod = "eltale,gameplay",
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $Make
)

if (-not $Make) { $Make = @("make", "-j16", "cdi") }

& "$PSScriptRoot\dc.ps1" python3 scripts/stage_mods.py $Mod
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\dc.ps1" @Make "MOD=$Mod"
exit $LASTEXITCODE
