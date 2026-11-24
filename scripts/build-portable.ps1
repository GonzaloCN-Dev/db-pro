param([string]$Out = "dist\portable")

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(DbPro VERSION ([0-9.]+)') { throw "Cannot read the version from CMakeLists.txt" }
$version = $Matches[1]
$payload = Join-Path $root "installer\payload"
if (-not (Test-Path (Join-Path $payload "DbPro.exe"))) { throw "Build the installer first so installer\payload is populated." }

$dest = Join-Path $root (Join-Path $Out "DbPro-Portable-$version")
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item $dest -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $payload "*") $dest -Recurse
New-Item (Join-Path $dest "data") -ItemType Directory -Force | Out-Null
@'
@echo off
setlocal
set "DBPRO_DATA_DIR=%~dp0data"
if not exist "%DBPRO_DATA_DIR%" mkdir "%DBPRO_DATA_DIR%"
"%~dp0DbPro.exe" %*
'@ | Set-Content (Join-Path $dest "DbPro-Portable.cmd") -Encoding ascii
@"
Db Pro $version — portable folder

Run DbPro-Portable.cmd from a writable folder. Connections and settings are kept in .\data.
Passwords remain in the current Windows user's Credential Manager and are not portable or included in this folder.
On another Windows user or PC, re-enter passwords after copying the folder.
Drivers included here are the same optional native drivers as the installer.
"@ | Set-Content (Join-Path $dest "PORTABLE-README.txt") -Encoding utf8

$zip = Join-Path (Join-Path $root $Out) "DbPro-Portable-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dest "*") -DestinationPath $zip -CompressionLevel Optimal
Get-Item $zip | Select-Object Name,Length
