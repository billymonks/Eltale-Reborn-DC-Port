# Builds build/eltale-reborn.cdi from the mod files in eltale/, starting Docker Desktop first if it isn't running.
#   scripts\eltale.ps1
[CmdletBinding()]
param()

$root = Resolve-Path (Join-Path $PSScriptRoot "..") -ErrorAction Stop

docker info *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Starting Docker Desktop..."
    Start-Process "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep 5
        docker info *> $null
        if ($LASTEXITCODE -eq 0) { break }
    }
    if ($LASTEXITCODE -ne 0) { throw "Docker Desktop didn't start" }
}

Push-Location $root
try {
    & "$PSScriptRoot\mod.ps1"
    if ($LASTEXITCODE -ne 0) { throw "disc build failed" }
} finally {
    Pop-Location
}

$cdi = Get-Item (Join-Path $root "build\eltale-reborn.cdi")
Write-Host ("Built {0} ({1:N1} MB, {2})" -f $cdi.FullName, ($cdi.Length / 1MB), $cdi.LastWriteTime)
