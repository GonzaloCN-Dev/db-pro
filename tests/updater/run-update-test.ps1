param(
    [string]$Old = "",
    [string]$New = ""
)

# Disposable end-to-end check of the update path: install an older version into a temp folder, verify the new
# installer's SHA-256 exactly like the app does, run it with the app's own switches while the old version is
# running, then confirm the version changed, the app was relaunched and user data survived.
# It never touches Program Files and uses a private DBPRO_DATA_DIR.
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Old) { $Old = Join-Path $root "dist\DbPro-Setup-0.2.3.exe" }
if (-not $New) { $New = Join-Path $root "dist\DbPro-Setup-0.2.4.exe" }
foreach ($f in @($Old, $New)) { if (-not (Test-Path $f)) { throw "Installer not found: $f" } }

$uninstKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6E3A8F42-9C1D-4B7E-A5D0-2F8B14C73E96}_is1"
if (Test-Path $uninstKey) {
    $loc = (Get-ItemProperty $uninstKey).InstallLocation
    $un = if ($loc) { Join-Path $loc "unins000.exe" } else { "" }
    if ($un -and (Test-Path $un)) { & $un /VERYSILENT /SUPPRESSMSGBOXES /NORESTART | Out-Null; Start-Sleep 3 }
}

$base = Join-Path $env:TEMP ("dbpro_update_" + [guid]::NewGuid().ToString("N"))
$app = Join-Path $base "app"
$data = Join-Path $base "data"
New-Item $data -ItemType Directory -Force | Out-Null
"keep-me" | Set-Content (Join-Path $data "user-note.txt")
$env:DBPRO_DATA_DIR = $data

function Fail([string]$m) { Write-Output "FAIL: $m (artifacts: $base)"; exit 1 }

$sha = (Get-FileHash $New -Algorithm SHA256).Hash.ToLower()
$p = Start-Process $Old -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/DIR=`"$app`"" -PassThru -Wait
if ($p.ExitCode -ne 0) { Fail "old install exit code $($p.ExitCode)" }
$exe = Join-Path $app "DbPro.exe"
$oldVer = (Get-Item $exe).VersionInfo.ProductVersion
Write-Output "installed old version: $oldVer"

$running = Start-Process $exe -PassThru
Start-Sleep 3
if ($running.HasExited) { Fail "old version did not stay running" }

# Same switches as src/updater.cpp run_installer (per-user flavour), plus /DIR to keep it disposable.
$args = "/SILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CLOSEAPPLICATIONS /RELAUNCH=1 /CURRENTUSER /DIR=`"$app`""
$check = (Get-FileHash $New -Algorithm SHA256).Hash.ToLower()
if ($check -ne $sha) { Fail "installer hash changed" }
$up = Start-Process $New -ArgumentList $args -PassThru
if (-not $up.WaitForExit(120000)) { Stop-Process -Id $up.Id -Force; Fail "update installer timed out" }
if ($up.ExitCode -ne 0) { Fail "update installer exit code $($up.ExitCode)" }
Start-Sleep 4

$newVer = (Get-Item $exe).VersionInfo.ProductVersion
Write-Output "version after update: $newVer"
if ($newVer -eq $oldVer) { Fail "version did not change" }
$relaunched = Get-Process DbPro -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
if (-not $relaunched) { Fail "app was not relaunched after the update" }
if ((Get-Content (Join-Path $data "user-note.txt")) -ne "keep-me") { Fail "user data changed" }

$relaunched | Stop-Process -Force
Start-Sleep 1
& (Join-Path $app "unins000.exe") /VERYSILENT /SUPPRESSMSGBOXES /NORESTART | Out-Null
Start-Sleep 3
if (Test-Path $exe) { Fail "uninstall left the executable behind" }
Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue
Write-Output "PASS: $oldVer -> $newVer, relaunched, data intact, clean uninstall"
