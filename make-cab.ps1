<#
.SYNOPSIS
    Builds the .cab to submit for Microsoft attestation signing.

.DESCRIPTION
    A self-signed driver package cannot load while Secure Boot is on. Windows
    requires a third-party driver package's signature to chain to the Microsoft
    Code Verification Root, and no combination of Root / TrustedPublisher
    entries satisfies that -- those only get the package *installed*, not
    *started*. Test signing would allow it, but test signing is ignored while
    Secure Boot is enabled, so the two are mutually exclusive by design.

    The only way to have Secure Boot on and this device working is to have
    Microsoft sign the package. This script produces the submission archive.

    What you still need, which this script cannot provide:

      1. A Microsoft Partner Center (Hardware Dev Center) account.
      2. An EV (Extended Validation) code-signing certificate, which is what
         Partner Center requires to register the account and sign this cab.
         These are issued on hardware tokens and cost roughly USD 250-400/year.

    Then: sign this cab with the EV certificate, upload it to Partner Center as
    a driver submission, request attestation signing, select Windows 11 ARM64 as
    the target, and download the Microsoft-signed package that comes back.
    Install that with install.ps1 and it will start with Secure Boot on.

    Verify the current target-OS list in Partner Center before submitting --
    the available attestation targets change over time.
#>

[CmdletBinding()]
param(
    [string]$Name = 'mceir'
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$PackageDir  = Join-Path $ProjectRoot 'build\ARM64\Release\package'
$OutDir      = Join-Path $ProjectRoot 'build\ARM64\Release\submission'

$inf = Join-Path $PackageDir 'mceir.inf'
$cat = Join-Path $PackageDir 'mceir.cat'

foreach ($f in @($inf, $cat)) {
    if (-not (Test-Path $f)) { throw "Missing $f. Run build.ps1 then sign.ps1 first." }
}

if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---------------------------------------------------------------------------
# Directive file
#
# The cab holds the package inside a folder rather than at the root, which is
# what Partner Center expects. Only the INF and catalog go in: this package
# ships no binary of its own, it binds the inbox winusb.sys.
# ---------------------------------------------------------------------------

$ddf = Join-Path $OutDir "$Name.ddf"

@"
.OPTION EXPLICIT
.Set CabinetFileCountThreshold=0
.Set FolderFileCountThreshold=0
.Set FolderSizeThreshold=0
.Set MaxCabinetSize=0
.Set MaxDiskFileCount=0
.Set MaxDiskSize=0
.Set CompressionType=MSZIP
.Set Cabinet=on
.Set Compress=on
.Set CabinetNameTemplate=$Name.cab
.Set DiskDirectoryTemplate=$OutDir
.Set DestinationDir=$Name
"$inf"
"$cat"
"@ | Set-Content -Path $ddf -Encoding ASCII

Write-Host "--- Building cab ---" -ForegroundColor Yellow
$output = & makecab.exe /f $ddf 2>&1
if ($LASTEXITCODE -ne 0) {
    $output | ForEach-Object { Write-Host "    $_" }
    throw "makecab failed with exit code $LASTEXITCODE."
}

$cab = Join-Path $OutDir "$Name.cab"
if (-not (Test-Path $cab)) { throw "makecab did not produce $cab" }

# makecab leaves these behind next to the cab.
Get-ChildItem $OutDir -Include 'setup.inf', 'setup.rpt' -Recurse -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host "`n--- Contents ---" -ForegroundColor Yellow
& expand.exe -D $cab | Where-Object { $_ -match ':' } | ForEach-Object { Write-Host "    $_" }

$info = Get-Item $cab
Write-Host "`nSubmission cab: $($info.FullName) ($($info.Length) bytes)" -ForegroundColor Green

Write-Host @"

This cab is NOT signed. Before uploading it you must sign it with an EV
code-signing certificate:

    signtool sign /v /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 "$cab"

Then upload to Partner Center, request attestation signing, and target
Windows 11 ARM64. Everything up to the EV certificate is done; that part cannot
be automated and cannot be self-signed.
"@ -ForegroundColor Gray
