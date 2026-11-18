<#
.SYNOPSIS
    Builds mceir.exe for ARM64.

.DESCRIPTION
    Drives cl.exe and link.exe directly rather than going through MSBuild, and
    locates the toolset by enumerating every Visual Studio install and keeping
    only those that actually ship a C++ toolset -- `vswhere -latest` on this
    machine reports SQL Server Management Studio, which has no compiler.
#>

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [switch]$Clean,

    # Also build and run the decoder tests, which exercise the whole receive
    # chain against synthetic frames and need no hardware.
    [switch]$Test
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$SrcDir      = Join-Path $ProjectRoot 'src'
$OutDir      = Join-Path $ProjectRoot "build\ARM64\$Configuration"
$ObjDir      = Join-Path $OutDir 'obj'

if ($Clean -and (Test-Path (Join-Path $ProjectRoot 'build'))) {
    Remove-Item (Join-Path $ProjectRoot 'build') -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

# ---------------------------------------------------------------------------
# Toolset
# ---------------------------------------------------------------------------

$vsRoots = @(
    "$env:ProgramFiles\Microsoft Visual Studio",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
) | Where-Object { Test-Path $_ }

$toolsets = foreach ($root in $vsRoots) {
    Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        Get-ChildItem -Path $_.FullName -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'VC\Tools\MSVC' }
    }
}

$msvc = $toolsets |
    Where-Object { Test-Path $_ } |
    ForEach-Object { Get-ChildItem $_ -Directory } |
    Where-Object { Test-Path (Join-Path $_.FullName 'bin\Hostarm64\arm64\cl.exe') } |
    Sort-Object Name -Descending |
    Select-Object -First 1

if (-not $msvc) { throw 'No MSVC toolset with a native ARM64 compiler was found.' }

$KitRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdk = Get-ChildItem (Join-Path $KitRoot 'Include') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'um\winusb.h') } |
    Sort-Object Name -Descending |
    Select-Object -First 1

if (-not $sdk) { throw "No Windows SDK with winusb.h was found under $KitRoot." }

$cl   = Join-Path $msvc.FullName 'bin\Hostarm64\arm64\cl.exe'
$link = Join-Path $msvc.FullName 'bin\Hostarm64\arm64\link.exe'

Write-Host "MSVC : $($msvc.Name)"  -ForegroundColor Cyan
Write-Host "SDK  : $($sdk.Name)"   -ForegroundColor Cyan

$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $sdk.FullName 'ucrt')
    (Join-Path $sdk.FullName 'shared')
    (Join-Path $sdk.FullName 'um')
    (Join-Path $sdk.FullName 'winrt')
) -join ';'

$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\arm64')
    (Join-Path $KitRoot "Lib\$($sdk.Name)\ucrt\arm64")
    (Join-Path $KitRoot "Lib\$($sdk.Name)\um\arm64")
) -join ';'

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------

$sources = @('main.cpp', 'device.cpp', 'mce_stream.cpp', 'rc6.cpp', 'keymap.cpp',
             'audio.cpp', 'service.cpp')

$cflags = @(
    '/nologo', '/c', '/W4', '/WX', '/EHsc', '/std:c++17', '/permissive-',
    '/Zc:preprocessor', '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN',
    "/Fo$ObjDir\"
)

if ($Configuration -eq 'Release') {
    # Static CRT so the daemon can run from a startup task on a machine with
    # no redistributable installed.
    $cflags += @('/O2', '/MT', '/DNDEBUG', '/GL')
} else {
    $cflags += @('/Od', '/MTd', '/Zi', '/D_DEBUG', "/Fd$ObjDir\mceir.pdb")
}

Write-Host "`n--- Compiling ---" -ForegroundColor Yellow
$objs = @()
foreach ($s in $sources) {
    $path = Join-Path $SrcDir $s
    if (-not (Test-Path $path)) { throw "Missing source file: $path" }

    Write-Host "  $s"
    & $cl @cflags $path
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $s." }

    $objs += Join-Path $ObjDir ([IO.Path]::ChangeExtension($s, 'obj'))
}

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------

$exe = Join-Path $OutDir 'mceir.exe'

$ldflags = @(
    '/nologo', '/MACHINE:ARM64', '/SUBSYSTEM:WINDOWS', '/ENTRY:wmainCRTStartup',
    "/OUT:$exe",
    'winusb.lib', 'cfgmgr32.lib', 'user32.lib', 'ole32.lib',
    'advapi32.lib', 'wtsapi32.lib', 'userenv.lib'
)

if ($Configuration -eq 'Release') { $ldflags += '/LTCG' } else { $ldflags += '/DEBUG' }

Write-Host "`n--- Linking ---" -ForegroundColor Yellow
& $link @ldflags @objs
if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }

$info = Get-Item $exe
Write-Host "`nBuilt $($info.FullName) ($($info.Length) bytes)" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if ($Test) {
    Write-Host "`n--- Building tests ---" -ForegroundColor Yellow

    $testDir = Join-Path $ProjectRoot 'build\ARM64\test'
    New-Item -ItemType Directory -Force -Path $testDir | Out-Null

    $testExe = Join-Path $testDir 'rc6_test.exe'
    $testSrc = @(
        (Join-Path $ProjectRoot 'test\rc6_test.cpp')
        (Join-Path $SrcDir 'rc6.cpp')
        (Join-Path $SrcDir 'mce_stream.cpp')
    )

    & $cl '/nologo' '/W4' '/WX' '/EHsc' '/std:c++17' '/permissive-' '/Zc:preprocessor' `
        '/DUNICODE' '/D_UNICODE' '/DWIN32_LEAN_AND_MEAN' '/O2' '/MT' `
        "/Fo$testDir\" "/Fe$testExe" @testSrc
    if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }

    $keymapTestExe = Join-Path $testDir 'keymap_test.exe'
    & $cl '/nologo' '/W4' '/WX' '/EHsc' '/std:c++17' '/permissive-' '/Zc:preprocessor' `
        '/DUNICODE' '/D_UNICODE' '/DWIN32_LEAN_AND_MEAN' '/O2' '/MT' `
        "/Fo$testDir\" "/Fe$keymapTestExe" `
        (Join-Path $ProjectRoot 'test\keymap_test.cpp') (Join-Path $SrcDir 'keymap.cpp') `
        /link user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Key map test build failed.' }

    Write-Host "`n--- Running tests ---" -ForegroundColor Yellow
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    Write-Host ''
    & $keymapTestExe
    if ($LASTEXITCODE -ne 0) { throw 'Key map tests failed.' }

    # The injection probe is a manual diagnostic rather than a test -- it reads
    # and moves the real system volume, so it is built but never run here. It
    # lives with the tests so that a -Clean build does not silently remove it.
    Write-Host "`n--- Building injection probe ---" -ForegroundColor Yellow
    & $cl '/nologo' '/W4' '/WX' '/EHsc' '/std:c++17' '/permissive-' '/O2' '/MT' `
        '/D_CRT_SECURE_NO_WARNINGS' `
        "/Fo$testDir\" "/Fe$testDir\inject_probe.exe" `
        (Join-Path $ProjectRoot 'test\inject_probe.cpp') /link ole32.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Probe build failed.' }
    Write-Host "  inject_probe.exe  (run: inject_probe.exe watch <seconds>)"
}
