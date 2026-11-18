<#
.SYNOPSIS
    Installs mceir as a Windows service. Run from an elevated prompt.

.DESCRIPTION
    The service starts at boot as LocalSystem, but does not handle the remote
    itself: a service runs in session 0, where SendInput reaches no desktop.
    It keeps a copy of mceir.exe -- the agent -- running as the user logged on
    at the console, with that user's own rights, starts it at logon, moves it
    on a user switch and restarts it if it dies. See README.md.

    Because the service runs as SYSTEM, its binary goes to Program Files, where
    only administrators can replace it, and its log to %ProgramData%\mceir,
    which ordinary users can read but not write. The agent still logs to
    %LOCALAPPDATA%\mceir\mceir.log.

    Replaces the logon Scheduled Task from install-autostart.ps1, so two copies
    do not race for the transceiver. Re-run after a rebuild to update.

.PARAMETER Uninstall
    Stops and removes the service and its binary. Logs are kept. Run
    install-autostart.ps1 afterwards to go back to the logon task.
#>

[CmdletBinding()]
param(
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$ServiceName = 'mceir'
$InstallDir  = Join-Path $env:ProgramFiles 'mceir'
$Exe         = Join-Path $InstallDir 'mceir.exe'
$LogDir      = Join-Path $env:ProgramData 'mceir'
$BuiltExe    = Join-Path $PSScriptRoot 'build\ARM64\Release\mceir.exe'
$StartupVbs  = Join-Path ([Environment]::GetFolderPath('Startup')) 'mceir.vbs'

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    throw 'This script needs an elevated prompt (Run as Administrator).'
}

function Remove-MceirService {
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if (-not $svc) { return }

    if ($svc.Status -ne 'Stopped') {
        Stop-Service -Name $ServiceName -Force
        $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20))
    }
    sc.exe delete $ServiceName | Out-Null

    # Deletion completes only once nothing holds the service open (an open
    # Services console does), and creating it again before then fails.
    for ($i = 0; $i -lt 20 -and (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 500
    }
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        throw "Service '$ServiceName' is marked for deletion but still exists; close the Services console and re-run."
    }
    Write-Host "  removed service '$ServiceName'" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------

if ($Uninstall) {
    Remove-MceirService
    if (Test-Path $InstallDir) {
        Remove-Item $InstallDir -Recurse -Force
        Write-Host "  removed $InstallDir" -ForegroundColor Green
    }
    Write-Host "  logs kept in $LogDir"
    Write-Host '  run .\install-autostart.ps1 to go back to the logon task'
    return
}

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

if (-not (Test-Path $BuiltExe)) {
    throw "No build at $BuiltExe; run .\build.ps1 first."
}

Write-Host "`n--- Removing what this replaces ---" -ForegroundColor Yellow

# Only one process can hold the transceiver.
if (Get-ScheduledTask -TaskName $ServiceName -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $ServiceName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $ServiceName -Confirm:$false
    Write-Host "  removed scheduled task '$ServiceName'" -ForegroundColor Green
}
if (Test-Path $StartupVbs) {
    Remove-Item $StartupVbs -Force
    Write-Host "  removed $StartupVbs" -ForegroundColor Green
}

Remove-MceirService

Get-Process -Name mceir -ErrorAction SilentlyContinue | ForEach-Object {
    Stop-Process -Id $_.Id -Force
    Write-Host "  stopped mceir.exe (pid $($_.Id))" -ForegroundColor Green
}

Write-Host "`n--- Installing files ---" -ForegroundColor Yellow

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item $BuiltExe $Exe -Force
Write-Host "  $Exe"

# Key map overrides are read from beside the exe. A mceir.conf kept in the
# project directory, next to mceir.conf.sample, is deployed with it; without
# one, an already-installed conf is left alone. (This used to look in
# package\, which sign.ps1 rebuilds without one, so nothing was ever copied.)
$conf = Join-Path $PSScriptRoot 'mceir.conf'
if (Test-Path $conf) {
    Copy-Item $conf $InstallDir -Force
    Write-Host "  $(Join-Path $InstallDir 'mceir.conf')"
}

# The service is SYSTEM and rotates its log by renaming, so the log directory
# must not be writable -- or owned -- by ordinary users. %ProgramData% would
# hand down an ACL that lets any user create files and links in it, and anyone
# could have created the directory first.
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
icacls $LogDir /setowner '*S-1-5-32-544' /T /C /Q | Out-Null
icacls $LogDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' /Q | Out-Null
if (Get-ChildItem $LogDir -Force) {
    icacls (Join-Path $LogDir '*') /reset /T /C /Q | Out-Null
}
Write-Host "  $LogDir (SYSTEM and Administrators write, Users read)"

Write-Host "`n--- Registering the service ---" -ForegroundColor Yellow

New-Service -Name $ServiceName `
    -BinaryPathName "`"$Exe`" --service" `
    -DisplayName 'MCE IR Remote (mceir)' `
    -Description 'Runs the mceir remote-control agent for the user logged on at the console.' `
    -StartupType Automatic | Out-Null

# Restart the service if it dies: 5 s, 5 s, then every 60 s; reset after a day.
sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/60000 | Out-Null

Start-Service -Name $ServiceName
(Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(10))
Write-Host "  service '$ServiceName' running" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

Start-Sleep -Seconds 3
Write-Host "`n--- Agent ---" -ForegroundColor Yellow
$agents = Get-CimInstance Win32_Process -Filter "Name='mceir.exe'" |
    Where-Object CommandLine -match '--agent'
if ($agents) {
    foreach ($a in $agents) {
        $owner = Invoke-CimMethod -InputObject $a -MethodName GetOwner
        Write-Host ("  pid {0}  session {1}  user {2}\{3}" -f $a.ProcessId, $a.SessionId, $owner.Domain, $owner.User)
    }
} else {
    Write-Host '  no agent yet -- normal if nobody is logged on at the console' -ForegroundColor DarkYellow
}

$svcLog = Join-Path $LogDir 'service.log'
if (Test-Path $svcLog) {
    Write-Host "`n--- $svcLog ---" -ForegroundColor Yellow
    Get-Content $svcLog -Tail 5 | ForEach-Object { Write-Host "  $_" }
}
