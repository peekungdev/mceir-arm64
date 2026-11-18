<#
.SYNOPSIS
    Registers mceir to start automatically at logon, as a Scheduled Task.

.DESCRIPTION
    Run from an ELEVATED prompt.

    The alternative is install-service.ps1, a Windows service that starts a
    per-user agent. Use one or the other: two copies race for the device, so
    this refuses to install while the service is.

    This replaces the Startup-folder launcher, which is removed if present, for
    the same reason.

.PARAMETER Elevated
    Run mceir with the highest available privileges.

    Worth understanding before you use it. Windows blocks input injection from
    a normal-integrity process into a higher-integrity window (UIPI), so
    without this the remote does nothing while Task Manager, an admin console
    or a UAC prompt has focus. With it, the remote works everywhere.

    The cost is that mceir then runs elevated all the time. It is a small
    program that reads a USB endpoint and calls SendInput, but it is still a
    permanently elevated process, so this is off by default.

.PARAMETER Uninstall
    Removes the task and the Startup-folder launcher.
#>

[CmdletBinding()]
param(
    [switch]$Elevated,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$TaskName   = 'mceir'
$PackageDir = Join-Path $PSScriptRoot 'build\ARM64\Release\package'
$Exe        = Join-Path $PackageDir 'mceir.exe'
$StartupVbs = Join-Path ([Environment]::GetFolderPath('Startup')) 'mceir.vbs'

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    throw 'This script needs an elevated prompt (Run as Administrator). Registering a Scheduled Task requires it.'
}

# The service runs its own copy of mceir; stopping or racing that is not ours
# to do.
$serviceInstalled = [bool](Get-Service -Name 'mceir' -ErrorAction SilentlyContinue)

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------

if ($Uninstall) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "  removed scheduled task '$TaskName'" -ForegroundColor Green
    } else {
        Write-Host "  no scheduled task '$TaskName'" -ForegroundColor DarkYellow
    }

    if (Test-Path $StartupVbs) {
        Remove-Item $StartupVbs -Force
        Write-Host "  removed $StartupVbs" -ForegroundColor Green
    }

    if (-not $serviceInstalled) {
        Get-Process mceir -ErrorAction SilentlyContinue | ForEach-Object {
            Stop-Process -Id $_.Id -Force
            Write-Host "  stopped running mceir (pid $($_.Id))" -ForegroundColor Green
        }
    }
    return
}

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

if ($serviceInstalled) {
    throw 'The mceir service is installed, and a task beside it would race it for the device. Remove it first: .\install-service.ps1 -Uninstall'
}

if (-not (Test-Path $Exe)) { throw "Missing $Exe. Run build.ps1 then sign.ps1 first." }

# The task runs as whoever is installing it, in their own session.
$user = "$env:USERDOMAIN\$env:USERNAME"
Write-Host "Registering '$TaskName' for $user" -ForegroundColor Cyan
Write-Host ("  privileges : {0}" -f $(if ($Elevated) { 'Highest (UIPI-proof)' } else { 'Limited' }))

# A logon trigger only. The task runs in the user's interactive session, so
# there is nothing for it to do before someone logs on.
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$trigger.Delay = 'PT10S'   # let the USB stack finish enumerating first

# mceir.exe is a Windows-subsystem binary, so the task gives it no console and
# no window ever appears. That also means there is nowhere for it to report a
# problem, so point it at a log file.
$LogDir = Join-Path $env:LOCALAPPDATA 'mceir'
$LogFile = Join-Path $LogDir 'mceir.log'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
Write-Host ("  log        : {0}" -f $LogFile)

$action = New-ScheduledTaskAction -Execute $Exe `
    -Argument "--log `"$LogFile`"" -WorkingDirectory $PackageDir

$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit ([TimeSpan]::Zero)   # never kill it; it is a daemon

$principal = New-ScheduledTaskPrincipal `
    -UserId $user `
    -LogonType Interactive `
    -RunLevel $(if ($Elevated) { 'Highest' } else { 'Limited' })

Register-ScheduledTask -TaskName $TaskName `
    -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
    -Description 'MCE infrared remote receiver. Runs in the user session because a service in session 0 cannot inject input.' `
    -Force | Out-Null

Write-Host "  task registered" -ForegroundColor Green

# The Startup launcher would now be a second copy fighting for the device.
if (Test-Path $StartupVbs) {
    Remove-Item $StartupVbs -Force
    Write-Host "  removed the old Startup-folder launcher" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Start it now and prove it took
# ---------------------------------------------------------------------------

Get-Process mceir -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force }
Start-Sleep -Milliseconds 500

Start-ScheduledTask -TaskName $TaskName
Start-Sleep -Seconds 3

$task = Get-ScheduledTask -TaskName $TaskName
$info = Get-ScheduledTaskInfo -TaskName $TaskName
Write-Host "`n--- Result ---" -ForegroundColor Yellow
Write-Host ("  task state    : {0}" -f $task.State)
Write-Host ("  last result   : 0x{0:X}" -f $info.LastTaskResult)

$proc = Get-Process mceir -ErrorAction SilentlyContinue
if ($proc) {
    foreach ($p in $proc) {
        Write-Host ("  running       : pid {0}, session {1}" -f $p.Id, $p.SessionId) -ForegroundColor Green
    }
    Write-Host "`nThe remote should respond now, and again after every logon." -ForegroundColor Green
} else {
    Write-Host "  mceir is NOT running" -ForegroundColor Red
    Write-Host "  Check the task history in Task Scheduler, and try running the exe"
    Write-Host "  directly to see what it says:  $Exe -v"
}
