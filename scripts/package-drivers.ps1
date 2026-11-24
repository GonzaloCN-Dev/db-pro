# Builds the release assets for optional drivers: the DLLs plus drivers.json (size + SHA-256).
# Usage: .\scripts\package-drivers.ps1 [-Out dist\drivers]
# Then upload everything in -Out to the GitHub release tagged v<version> (see docs/DRIVERS.md).
param([string]$Out = "dist\drivers")

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$src = Join-Path $root "build\drivers"
if (-not (Test-Path $src)) { throw "Nothing to package. Build the target 'dbpro_drivers' first." }

$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(DbPro VERSION ([0-9.]+)') { throw "Cannot read the version from CMakeLists.txt" }
$version = $Matches[1]

$dest = Join-Path $root $Out
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $dest -File | Remove-Item

$drivers = [ordered]@{}
foreach ($f in Get-ChildItem $src -Filter "dbpro_driver_*.dll") {
    $id = $f.BaseName -replace '^dbpro_driver_', ''
    Copy-Item $f.FullName $dest
    $drivers[$id] = [ordered]@{
        file   = $f.Name
        size   = $f.Length
        sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()
    }
}
if ($drivers.Count -eq 0) { throw "No dbpro_driver_*.dll found in $src" }

$manifest = [ordered]@{ app_version = $version; drivers = $drivers }
[IO.File]::WriteAllText((Join-Path $dest "drivers.json"), ($manifest | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))
"Packaged $($drivers.Count) driver(s) for Db Pro $version in $dest"
$drivers.Keys | ForEach-Object { "  $_  $($drivers[$_].size) bytes  $($drivers[$_].sha256)" }
