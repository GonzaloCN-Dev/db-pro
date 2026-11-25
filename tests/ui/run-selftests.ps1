param(
    [string]$Exe = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) "build\DbPro.exe"),
    [string]$Drivers = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) "build\drivers")
)

$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not (Test-Path $Exe)) { throw "Db Pro executable not found: $Exe" }
if (-not (Test-Path (Join-Path $root "build\dbpro_seed.exe"))) { throw "Build dbpro_seed first." }

function Invoke-UiScenario([string]$Name, [string]$ScriptName, [bool]$BadPath = $false) {
    $dir = Join-Path $env:TEMP ("dbpro_ui_" + $Name + "_" + [guid]::NewGuid().ToString("N"))
    New-Item $dir -ItemType Directory | Out-Null
    $db = Join-Path $dir "sample.db"
    if (-not $BadPath) { & (Join-Path $root "build\dbpro_seed.exe") $db 200 | Out-Null }
    $path = if ($BadPath) { Join-Path (Join-Path $dir "missing-parent") "missing.db" } else { $db }
    $profile = @{
        version = 1
        profiles = @(@{
            id = "uitest" + [guid]::NewGuid().ToString("N").Substring(0, 8)
            name = "Disposable SQLite"
            engine = "sqlite"
            path = $path
            database = ""
            host = ""
            port = 0
            user = ""
            env = 0
            read_only = $false
            tls = $true
            trust_server_cert = $false
        })
    }
    $profile | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $dir "connections.json") -Encoding utf8
    '{"dark":true,"lang":"es","page_size":100,"monitor_interval":1}' | Set-Content (Join-Path $dir "settings.json") -Encoding utf8
    $script = Join-Path $PSScriptRoot $ScriptName
    $env:DBPRO_SELFTEST = $script
    $env:DBPRO_SELFTEST_OUT = $dir
    $env:DBPRO_DATA_DIR = $dir
    $env:DBPRO_DRIVERS_DIR = $Drivers
    $proc = Start-Process -FilePath $Exe -PassThru
    if (-not $proc.WaitForExit(45000)) {
        Stop-Process -Id $proc.Id -Force
        throw "$Name timed out; artifacts left at $dir"
    }
    $result = Join-Path $dir "selftest-result.txt"
    if (-not (Test-Path $result) -or (Get-Content $result -Raw).Trim() -ne "PASS") {
        throw "$Name failed; inspect screenshots and harness output at $dir"
    }
    if ($Name -eq "core") {
        $env:DBPRO_SELFTEST = Join-Path $PSScriptRoot "settings-persistence.txt"
        $proc = Start-Process -FilePath $Exe -PassThru
        if (-not $proc.WaitForExit(15000)) {
            Stop-Process -Id $proc.Id -Force
            throw "settings-persistence timed out; artifacts left at $dir"
        }
        if (-not (Test-Path $result) -or (Get-Content $result -Raw).Trim() -ne "PASS") {
            throw "settings-persistence failed; inspect artifacts at $dir"
        }
        Write-Output "settings-persistence PASS ($dir)"
    }
    Write-Output "$Name PASS ($dir)"
}

Invoke-UiScenario "core" "core-flow.txt"
Invoke-UiScenario "connection-error" "connection-error.txt" $true
Invoke-UiScenario "sql-history" "sql-history.txt"
