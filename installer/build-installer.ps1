# Builds the Windows installer. Prerequisites: a Release build of DbPro and of the target dbpro_drivers
# (.\build.bat; cmake --build build --target dbpro_drivers), and Inno Setup 6 (winget install JRSoftware.InnoSetup).
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$payload = Join-Path $PSScriptRoot "payload"

$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(DbPro VERSION ([0-9.]+)') { throw "Cannot read the version from CMakeLists.txt" }
$version = $Matches[1]

$exe = Join-Path $root "build\DbPro.exe"
$dll = Join-Path $root "build\slint_cpp.dll"
foreach ($f in @($exe, $dll)) { if (-not (Test-Path $f)) { throw "Missing: $f (build first)" } }
$crt = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\Community\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory |
    Where-Object { $_.FullName -notmatch "debug" } | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "Visual C++ redistributable folder not found" }

Remove-Item $payload -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $payload | Out-Null
Copy-Item $exe, $dll, (Join-Path $root "README.md"), (Join-Path $root "LICENSE") $payload
Get-ChildItem $crt.FullName -Filter *.dll | Where-Object { $_.Name -ne "vccorlib140.dll" -and $_.Name -ne "concrt140.dll" } | Copy-Item -Destination $payload

$drv = Join-Path $root "build\drivers"
if (Test-Path $drv) {
    New-Item -ItemType Directory (Join-Path $payload "drivers") | Out-Null
    Copy-Item (Join-Path $drv "dbpro_driver_*.dll") (Join-Path $payload "drivers")
}

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 is not installed (winget install JRSoftware.InnoSetup)" }

& $iscc "/DAppVersion=$version" (Join-Path $PSScriptRoot "DbPro.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
Get-ChildItem (Join-Path $root "dist") -Filter "DbPro-Setup-*.exe" | Select-Object Name, @{n="MB";e={[math]::Round($_.Length/1MB,1)}}