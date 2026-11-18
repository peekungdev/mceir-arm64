<#
.SYNOPSIS
    Validates and test-signs the mceir driver package.

.DESCRIPTION
    The package is only an INF and a catalog -- the driver binary is the
    inbox winusb.sys, which Microsoft already signed. What needs signing is
    the catalog, so that PnP will accept our INF as the thing that binds
    winusb to this hardware ID.

    This script does NOT install the certificate into any trust store.
    Signing is one decision; deciding a self-signed certificate may authorise
    a driver install is a separate and larger one, it needs elevation, and it
    should be made explicitly. install.ps1 does that part.
#>

[CmdletBinding()]
param(
    [string]$CertSubject = 'CN=mceir-arm64 Test Signing',

    # Inf2Cat rejects a bare "10_ARM64"; these are the two newest ARM64 targets
    # it accepts.
    [string]$CatalogOs = '10_NI_ARM64,10_GE_ARM64',

    [switch]$Timestamp
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$OutDir      = Join-Path $ProjectRoot 'build\ARM64\Release'
$PackageDir  = Join-Path $OutDir 'package'
$KitRoot     = 'C:\Program Files (x86)\Windows Kits\10'

$infSource = Join-Path $ProjectRoot 'mceir.inf'
$exeSource = Join-Path $OutDir 'mceir.exe'

if (-not (Test-Path $infSource)) { throw "Missing $infSource" }
if (-not (Test-Path $exeSource)) { throw "Missing $exeSource. Run build.ps1 first." }

# ---------------------------------------------------------------------------
# Tools. Pick the newest kit that actually has each tool.
# ---------------------------------------------------------------------------

function Find-KitTool([string]$relative) {
    Get-ChildItem (Join-Path $KitRoot 'bin') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName $relative } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
}

$signtool = Find-KitTool 'arm64\signtool.exe'
if (-not $signtool) { $signtool = Find-KitTool 'x64\signtool.exe' }
if (-not $signtool) { throw 'signtool.exe not found.' }

# Inf2Cat ships x86-only and runs emulated here; that is fine.
$inf2cat = Find-KitTool 'x86\Inf2Cat.exe'
if (-not $inf2cat) { throw 'Inf2Cat.exe not found.' }

# infverif lives under Tools\<kit>\<arch>, not bin\.
$infverif = Get-ChildItem (Join-Path $KitRoot 'Tools') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'arm64\infverif.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1

Write-Host "signtool : $signtool" -ForegroundColor Cyan
Write-Host "Inf2Cat  : $inf2cat"  -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# Certificate
# ---------------------------------------------------------------------------

$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if ($cert) {
    Write-Host "`nReusing certificate $($cert.Thumbprint)" -ForegroundColor Cyan
} else {
    Write-Host "`nCreating test signing certificate..." -ForegroundColor Yellow
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $CertSubject `
        -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature `
        -KeyExportPolicy Exportable `
        -KeyLength 2048 `
        -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(5)
    Write-Host "  Created $($cert.Thumbprint)" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Stage the package. Inf2Cat catalogues whatever is in the directory, so it
# must hold the driver package and nothing else.
# ---------------------------------------------------------------------------

if (Test-Path $PackageDir) { Remove-Item $PackageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null

Copy-Item $infSource $PackageDir

$infPath = Join-Path $PackageDir 'mceir.inf'
$catPath = Join-Path $PackageDir 'mceir.cat'

# ---------------------------------------------------------------------------
# 1. Validate the INF before anything is signed
# ---------------------------------------------------------------------------

if ($infverif) {
    Write-Host "`n--- Validating INF ---" -ForegroundColor Yellow
    $ivOutput = & $infverif /v /w $infPath 2>&1
    $ivOutput | Where-Object { $_ -match 'ERROR|WARNING|NOT VALID|is valid' } |
        ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) { throw 'infverif rejected the INF; fix it before signing.' }
    Write-Host '    INF is valid' -ForegroundColor Green
} else {
    Write-Host "`n  infverif not found; skipping INF validation" -ForegroundColor DarkYellow
}

# ---------------------------------------------------------------------------
# 2. Build the catalog
# ---------------------------------------------------------------------------

Write-Host "`n--- Building catalog ---" -ForegroundColor Yellow
$catOutput = & $inf2cat /driver:$PackageDir /os:$CatalogOs /verbose 2>&1
$catOutput | ForEach-Object { Write-Host "    $_" }
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed.' }
if (-not (Test-Path $catPath)) { throw "Inf2Cat did not produce $catPath" }

# ---------------------------------------------------------------------------
# 3. Sign the catalog, and the daemon while we are here
# ---------------------------------------------------------------------------

$signArgs = @('sign', '/v', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, '/s', 'My')
if ($Timestamp) { $signArgs += @('/tr', 'http://timestamp.digicert.com', '/td', 'SHA256') }

Write-Host "`n--- Signing catalog ---" -ForegroundColor Yellow
& $signtool @signArgs $catPath
if ($LASTEXITCODE -ne 0) { throw 'Failed to sign the catalog.' }

Copy-Item $exeSource $PackageDir
$exePath = Join-Path $PackageDir 'mceir.exe'

$confSample = Join-Path $ProjectRoot 'mceir.conf.sample'
if (Test-Path $confSample) { Copy-Item $confSample $PackageDir }

Write-Host "`n--- Signing daemon ---" -ForegroundColor Yellow
& $signtool @signArgs $exePath
if ($LASTEXITCODE -ne 0) { throw 'Failed to sign mceir.exe.' }

# ---------------------------------------------------------------------------
# Export the public certificate so install.ps1 can place it in the trust stores
# ---------------------------------------------------------------------------

$cerPath = Join-Path $PackageDir 'mceir-test-cert.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

Write-Host "`n--- Signature check ---" -ForegroundColor Yellow
foreach ($f in @($catPath, $exePath)) {
    $sig = Get-AuthenticodeSignature $f
    Write-Host ("  {0,-16} {1}  [{2}]" -f (Split-Path $f -Leaf), $sig.Status, $sig.SignerCertificate.Subject)
}

Write-Host "`nPackage: $PackageDir" -ForegroundColor Green
Get-ChildItem $PackageDir | ForEach-Object { Write-Host ("  {0,-24} {1,8} bytes" -f $_.Name, $_.Length) }

Write-Host @"

A status of 'UnknownError' above means only that the self-signed certificate is
not trusted yet -- the signature itself is present and well formed. Trusting it
and installing the package is install.ps1, which needs an elevated prompt.
"@ -ForegroundColor Gray
