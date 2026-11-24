<#
.SYNOPSIS
    Builds Qualcomm Adreno 730 WDDM 2.0 KMD (adreno730_wddm.sys) and D3D12 UMD (adreno730_d3d12.dll)
    for Windows 11 ARM64 from source using Visual Studio MSVC ARM64 + Windows WDK 10.0.26100.0.
.LICENSE
    MIT License - Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
#>
param(
    [string]$MsvcRoot = "",
    [string]$WdkRoot  = "C:\ADK",
    [string]$WdkVer   = "10.0.26100.0",
    [switch]$UpdatePrebuilt
)

$ErrorActionPreference = 'Stop'
$SrcDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent (Split-Path -Parent $SrcDir)
$BuildDir = Join-Path $SrcDir 'build\arm64'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Auto-detect MSVC ARM64 toolchain if not specified
if (-not $MsvcRoot) {
    $candidates = @(
        'C:\VS2022\VC\Tools\MSVC\*',
        'C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\*'
    )
    foreach ($pat in $candidates) {
        $found = Get-ChildItem -Path $pat -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
        if ($found -and (Test-Path (Join-Path $found.FullName 'bin\Hostx64\arm64\cl.exe'))) {
            $MsvcRoot = $found.FullName
            break
        }
    }
}
if (-not $MsvcRoot -or -not (Test-Path (Join-Path $MsvcRoot 'bin\Hostx64\arm64\cl.exe'))) {
    throw "MSVC ARM64 toolchain not found. Pass -MsvcRoot <path-to-VC\Tools\MSVC\version>."
}
if (-not (Test-Path (Join-Path $WdkRoot "Include\$WdkVer\km\ntddk.h"))) {
    throw "Windows WDK $WdkVer not found under $WdkRoot. Pass -WdkRoot and -WdkVer."
}

$CL     = Join-Path $MsvcRoot 'bin\Hostx64\arm64\cl.exe'
$LINK   = Join-Path $MsvcRoot 'bin\Hostx64\arm64\link.exe'
$ARMASM = Join-Path $MsvcRoot 'bin\Hostx64\arm64\armasm64.exe'

Write-Host "Using MSVC: $MsvcRoot" -ForegroundColor Cyan
Write-Host "Using WDK : $WdkRoot ($WdkVer)" -ForegroundColor Cyan

Push-Location $BuildDir
try {
    # 1. Assemble ARM64 SMC helper
    Write-Host "[1/3] Assembling scm_smc_arm64.asm..." -ForegroundColor Green
    & $ARMASM -nologo (Join-Path $SrcDir 'scm_smc_arm64.asm') -o (Join-Path $BuildDir 'scm_smc_arm64.obj')
    if ($LASTEXITCODE -ne 0) { throw "armasm64 failed ($LASTEXITCODE)" }

    # 2. Build WDDM 2.0 KMD (adreno730_wddm.sys)
    Write-Host "[2/3] Building adreno730_wddm.sys (ARM64 WDDM 2.0)..." -ForegroundColor Green
    $env:INCLUDE = "$WdkRoot\Include\$WdkVer\km;$WdkRoot\Include\$WdkVer\shared;$WdkRoot\Include\$WdkVer\ucrt;$WdkRoot\Include\$WdkVer\um;$MsvcRoot\include;$SrcDir\include;$SrcDir"
    $env:LIB     = "$WdkRoot\Lib\$WdkVer\km\arm64;$WdkRoot\Lib\$WdkVer\ucrt\arm64;$WdkRoot\Lib\$WdkVer\um\arm64;$MsvcRoot\lib\arm64"

    $kmdFlags = @(
        '/nologo','/kernel','/W4','/WX-','/O2','/GS-',
        '/D_ARM64_=1','/DWIN32_LEAN_AND_MEAN=1','/DPOOL_NX_OPTIN=1',
        '/D_KERNEL_MODE=1','/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0A000000',
        '/DNTSTRSAFE_LIB=1','/wd4324','/wd4100','/c'
    )
    $kmdSources = @(
        "$SrcDir\adreno730_wddm.c",
        "$SrcDir\adreno730_hw.c",
        "$SrcDir\adreno730_display.c",
        "$SrcDir\common\wddm.c",
        "$SrcDir\common\gpu.c",
        "$SrcDir\common\pm4.c",
        "$SrcDir\common\ring.c",
        "$SrcDir\common\ir3_ref.c",
        "$SrcDir\common\fw.c",
        "$SrcDir\common\hfi.c",
        "$SrcDir\common\a730.c",
        "$SrcDir\common\smmu.c"
    )
    & $CL @kmdFlags @kmdSources
    if ($LASTEXITCODE -ne 0) { throw "KMD compile failed ($LASTEXITCODE)" }

    & $LINK /nologo /DRIVER /KERNEL /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:ARM64 /OPT:REF /OPT:ICF /INTEGRITYCHECK `
        adreno730_wddm.obj adreno730_hw.obj adreno730_display.obj wddm.obj scm_smc_arm64.obj `
        gpu.obj pm4.obj ring.obj ir3_ref.obj fw.obj hfi.obj a730.obj smmu.obj `
        displib.lib ntoskrnl.lib hal.lib bufferoverflowfastfailk.lib wdmsec.lib ntstrsafe.lib libcntpr.lib `
        /OUT:adreno730_wddm.sys
    if ($LASTEXITCODE -ne 0) { throw "KMD link failed ($LASTEXITCODE)" }

    # 3. Build D3D12 UMD (adreno730_d3d12.dll)
    Write-Host "[3/3] Building adreno730_d3d12.dll (ARM64 D3D12 UMD)..." -ForegroundColor Green
    $env:INCLUDE = "$WdkRoot\Include\$WdkVer\ucrt;$WdkRoot\Include\$WdkVer\um;$WdkRoot\Include\$WdkVer\shared;$MsvcRoot\include;$SrcDir\include;$SrcDir"
    $env:LIB     = "$WdkRoot\Lib\$WdkVer\ucrt\arm64;$WdkRoot\Lib\$WdkVer\um\arm64;$MsvcRoot\lib\arm64"

    & $CL /nologo /W4 /WX- /O2 /D_ARM64_=1 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /LD `
        "$SrcDir\adreno730_d3d12.c" `
        "$SrcDir\common\pm4.c" `
        "$SrcDir\common\d3d12.c" `
        "$SrcDir\common\ir3_ref.c" `
        /Fe:adreno730_d3d12.dll /link /MACHINE:ARM64 gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw "UMD build failed ($LASTEXITCODE)" }

    Copy-Item "$SrcDir\adreno730_wddm.inf" "$BuildDir\adreno730_wddm.inf" -Force

    if ($UpdatePrebuilt) {
        $PreDir = Join-Path $RepoRoot 'prebuilt\Adreno730-GPU'
        New-Item -ItemType Directory -Force -Path $PreDir | Out-Null
        Copy-Item "$BuildDir\adreno730_wddm.sys"  "$PreDir\adreno730_wddm.sys" -Force
        Copy-Item "$BuildDir\adreno730_d3d12.dll" "$PreDir\adreno730_d3d12.dll" -Force
        Copy-Item "$SrcDir\adreno730_wddm.inf"    "$PreDir\adreno730_wddm.inf" -Force
        Write-Host "Updated $PreDir" -ForegroundColor Yellow
    }

    Write-Host "Build complete:" -ForegroundColor Green
    Get-Item "$BuildDir\adreno730_wddm.sys", "$BuildDir\adreno730_d3d12.dll", "$BuildDir\adreno730_wddm.inf" | Select-Object Name, Length
}
finally {
    Pop-Location
}
