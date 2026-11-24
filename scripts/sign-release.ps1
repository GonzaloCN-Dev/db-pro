param(
    [Parameter(Mandatory = $true)][string]$Thumbprint,
    [Parameter(Mandatory = $true)][string]$File,
    [ValidateSet("CurrentUser", "LocalMachine")][string]$Store = "CurrentUser"
)

$ErrorActionPreference = "Stop"
$thumb = ($Thumbprint -replace '\s', '').ToUpperInvariant()
$cert = Get-ChildItem "Cert:\$Store\My" | Where-Object { $_.Thumbprint -eq $thumb } | Select-Object -First 1
if (-not $cert) { throw "The requested signing certificate was not found in Cert:\$Store\My." }
if (-not $cert.HasPrivateKey) { throw "The signing certificate has no accessible private key." }
if ($cert.NotAfter -le (Get-Date) -or $cert.NotBefore -gt (Get-Date)) { throw "The signing certificate is not currently valid." }
if ($cert.EnhancedKeyUsageList.ObjectId -notcontains "1.3.6.1.5.5.7.3.3") { throw "The certificate is not valid for code signing." }
if (-not (Test-Path $File -PathType Leaf)) { throw "File not found: $File" }

$chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
$chain.ChainPolicy.RevocationMode = [System.Security.Cryptography.X509Certificates.X509RevocationMode]::Online
if (-not $chain.Build($cert)) {
    $status = ($chain.ChainStatus | ForEach-Object { $_.Status.ToString() + ": " + $_.StatusInformation.Trim() }) -join "; "
    throw "Certificate chain validation failed: $status"
}

$kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
$signTool = Get-ChildItem $kits -Directory -ErrorAction SilentlyContinue |
    Sort-Object { try { [version]$_.Name } catch { [version]"0.0" } } -Descending |
    ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $signTool) { throw "Windows SDK signtool.exe was not found." }

& $signTool sign /sha1 $thumb /fd SHA256 /tr "https://timestamp.digicert.com" /td SHA256 $File
if ($LASTEXITCODE -ne 0) { throw "signtool failed with exit code $LASTEXITCODE." }
$signature = Get-AuthenticodeSignature $File
if ($signature.Status -ne "Valid" -or $signature.SignerCertificate.Thumbprint -ne $thumb) {
    throw "Authenticode verification failed after signing: $($signature.Status) $($signature.StatusMessage)"
}
Write-Output "Signed and verified: $File"
