<#
.SYNOPSIS
    Trusts the test certificate and installs the mceir driver package.

.DESCRIPTION
    Run from an ELEVATED prompt.

    Signing a package is not the same as Windows accepting it as signed. The
    catalog is signed with a self-signed certificate, and until that
    certificate is in the machine's Root and TrustedPublisher stores the chain
    does not validate -- the package installs, but the driver store records it
    as "Signer Name: Unknown, Attributes: Legacy".

    Worse, that record sticks. Trusting the certificate afterwards does not
    re-evaluate an already-imported package, so this script deletes any
    existing copy and re-imports it once the certificate is trusted.

    Test signing is NOT required here. The package installs no kernel binary
    of its own -- it binds the inbox, Microsoft-signed winusb.sys -- so only
    the catalog's trust matters, not the code-signing policy for new drivers.

.PARAMETER Uninstall
    Removes the driver package. The device then returns to Code 28, which on
    this build of Windows is the honest state.

.PARAMETER KeepCertificate
    With -Uninstall, leaves the test certificate in the trust stores.
#>

[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$KeepCertificate,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$HardwareId = 'USB\VID_0471&PID_060C'
$PackageDir = Join-Path $PSScriptRoot 'build\ARM64\Release\package'
$InfPath    = Join-Path $PackageDir 'mceir.inf'
$CatPath    = Join-Path $PackageDir 'mceir.cat'
$CerPath    = Join-Path $PackageDir 'mceir-test-cert.cer'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    throw 'This script needs an elevated prompt (Run as Administrator).'
}

function Get-TargetDevice {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like "$HardwareId*" } |
        Select-Object -First 1
}

# Every oem*.inf in the driver store that came from our mceir.inf.
function Get-InstalledCopies {
    $text = (pnputil /enum-drivers) -join "`n"
    [regex]::Matches($text,
        'Published Name:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name:\s*mceir\.inf') |
        ForEach-Object { $_.Groups[1].Value }
}

function Show-DriverStoreEntry {
    $text = (pnputil /enum-drivers) -join "`n"
    $blocks = $text -split "(?m)^\s*$" | Where-Object { $_ -match 'mceir\.inf' }
    if (-not $blocks) {
        Write-Host '  (not in the driver store)' -ForegroundColor DarkYellow
        return
    }
    foreach ($b in $blocks) {
        foreach ($line in ($b -split "`r?`n")) {
            if ($line -match 'Published Name|Original Name|Signer Name|Attributes') {
                $colour = 'Gray'
                if ($line -match 'Signer Name') {
                    $colour = if ($line -match 'Unknown') { 'Red' } else { 'Green' }
                }
                Write-Host "    $($line.Trim())" -ForegroundColor $colour
            }
        }
    }
}

function Show-Device {
    $dev = Get-TargetDevice
    if (-not $dev) {
        Write-Host '  device not present' -ForegroundColor DarkYellow
        return $null
    }
    $svc = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)" `
              -ErrorAction SilentlyContinue).Service
    $colour = if ($svc -eq 'WINUSB') { 'Green' } else { 'DarkYellow' }
    Write-Host ("  status={0} problem={1} service={2}" -f `
        $dev.Status, $dev.Problem, $(if ($svc) { $svc } else { '<none>' })) -ForegroundColor $colour
    return $dev
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------

if ($Uninstall) {
    Write-Host 'Removing the mceir driver package...' -ForegroundColor Yellow

    $copies = Get-InstalledCopies
    if (-not $copies) {
        Write-Host '  package is not installed' -ForegroundColor DarkYellow
    } else {
        foreach ($c in $copies) {
            Write-Host "  deleting $c"
            pnputil /delete-driver $c /uninstall /force
        }
    }

    if (-not $KeepCertificate -and (Test-Path $CerPath)) {
        $thumb = (Get-PfxCertificate $CerPath).Thumbprint
        foreach ($store in 'Root', 'TrustedPublisher') {
            $p = "Cert:\LocalMachine\$store\$thumb"
            if (Test-Path $p) {
                Write-Host "  removing certificate from LocalMachine\$store"
                Remove-Item $p -Force
            }
        }
    }

    Write-Host "`nDevice now:" -ForegroundColor Yellow
    Show-Device | Out-Null
    Write-Host @'

The transceiver falls back to Code 28, because Windows 11 ARM64 ships no driver
for its USB\MS_COMP_USBCIR compatible ID. That is the honest state, not a fault.
'@ -ForegroundColor Gray
    return
}

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

foreach ($f in @($InfPath, $CatPath, $CerPath)) {
    if (-not (Test-Path $f)) { throw "Missing $f. Run build.ps1 then sign.ps1 first." }
}

Write-Host '=== Before ===' -ForegroundColor Yellow
Show-DriverStoreEntry
Show-Device | Out-Null

# --- 1. Trust the certificate ----------------------------------------------
#
# Both stores are needed and they do different jobs: Root makes the chain
# validate at all, TrustedPublisher stops the "Would you like to install this
# device software?" prompt and lets an unattended install proceed.

Write-Host "`n=== Trusting the test certificate ===" -ForegroundColor Yellow
$cert  = Get-PfxCertificate $CerPath
$thumb = $cert.Thumbprint
Write-Host "  $($cert.Subject)"
Write-Host "  thumbprint $thumb"

foreach ($store in 'Root', 'TrustedPublisher') {
    $target = "Cert:\LocalMachine\$store"
    if (Test-Path "$target\$thumb") {
        Write-Host "  already in LocalMachine\$store" -ForegroundColor DarkGray
    } else {
        Import-Certificate -FilePath $CerPath -CertStoreLocation $target | Out-Null
        Write-Host "  added to LocalMachine\$store" -ForegroundColor Green
    }
}

# The catalog signature should now actually validate.
$sig = Get-AuthenticodeSignature $CatPath
Write-Host ("  catalog signature: {0}" -f $sig.Status) `
    -ForegroundColor $(if ($sig.Status -eq 'Valid') { 'Green' } else { 'Red' })
if ($sig.Status -ne 'Valid' -and -not $Force) {
    throw "The catalog still does not validate ($($sig.Status)). Re-run sign.ps1, or pass -Force."
}

# --- 2. Drop any copy imported before the certificate was trusted -----------
#
# pnputil records the signer at import time and never revisits it, so a package
# added while the certificate was untrusted stays "Signer Name: Unknown,
# Attributes: Legacy" forever. Only a delete and re-import clears that.

Write-Host "`n=== Removing copies imported before the certificate was trusted ===" -ForegroundColor Yellow
$copies = Get-InstalledCopies
if (-not $copies) {
    Write-Host '  none installed' -ForegroundColor DarkGray
} else {
    foreach ($c in $copies) {
        Write-Host "  deleting $c"
        pnputil /delete-driver $c /uninstall /force
    }
}

# --- 3. Import and install --------------------------------------------------

Write-Host "`n=== Installing ===" -ForegroundColor Yellow
pnputil /add-driver $InfPath /install
if ($LASTEXITCODE -ne 0) { throw "pnputil failed with exit code $LASTEXITCODE." }

Start-Sleep -Seconds 2

# --- 4. Verify --------------------------------------------------------------

Write-Host "`n=== After ===" -ForegroundColor Yellow
Show-DriverStoreEntry
$dev = Show-Device

$svc = if ($dev) {
    (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)" `
        -ErrorAction SilentlyContinue).Service
}

if ($svc -ne 'WINUSB' -and $dev) {
    # A force-installed driver node ranks 0xFFFFFFFF so any real match beats it,
    # but nothing re-triggers installation on that devnode until it is disturbed.
    Write-Host "`nThe devnode did not move to WinUSB; re-enumerating it." -ForegroundColor DarkYellow
    pnputil /remove-device $dev.InstanceId
    pnputil /scan-devices
    Start-Sleep -Seconds 3
    $dev = Show-Device
    $svc = if ($dev) {
        (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)" `
            -ErrorAction SilentlyContinue).Service
    }
}

if ($svc -eq 'WINUSB') {
    Write-Host "`nInstalled and bound. Start the daemon with:" -ForegroundColor Green
    Write-Host "    $PackageDir\mceir.exe --print" -ForegroundColor Green
} else {
    Write-Host @'

Not bound. If Device Manager shows Code 52 the certificate is still not trusted
for driver installs; Code 28 means the INF was not matched at all -- confirm the
hardware ID really is USB\VID_0471&PID_060C.
'@ -ForegroundColor Red
}
