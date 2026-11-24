<#
.SYNOPSIS
    Installs the Adreno 730 WDDM 2.0 driver (adreno730_wddm.sys + adreno730_d3d12.dll)
    and Mesa Turnip Vulkan 1.4.348 ICD (vulkan_freedreno.dll) on Galaxy S22 Windows 11 ARM64.
.DESCRIPTION
    - Verifies required official linux-firmware blobs (a730_sqe.fw + gmu_gen70000.bin) in System32\drivers\qcom.
    - Installs the signed WDDM driver package with boot-safe StartGpu=0 default.
    - Copies vulkan_freedreno.dll + freedreno_icd.aarch64.json to C:\Windows\System32\s22_turnip\
      and registers the ICD in HKLM\SOFTWARE\Khronos\Vulkan\Drivers.
    - If -EnableGpuNow is passed, starts the Adreno 730 live and immediately resets StartGpu=0
      so future boots remain 100% safe.
.LICENSE
    MIT License - Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
#>
param(
    [string]$DriverDir  = ".\prebuilt\Adreno730-GPU",
    [string]$FirmwareDir = "C:\Windows\System32\drivers\qcom",
    [switch]$EnableGpuNow
)

$ErrorActionPreference = 'Stop'

# 1. Verify required Qualcomm firmware blobs (must be supplied by user from linux-firmware)
$sqePath = Join-Path $FirmwareDir 'a730_sqe.fw'
$gmuPath = Join-Path $FirmwareDir 'gmu_gen70000.bin'
$expectedSqeSha256 = '4F8B288F340FF5B4881BED8DFEA90BB4A1B967556CA2DF6D07F4ED4E4A28D071'

if (-not (Test-Path $sqePath) -or -not (Test-Path $gmuPath)) {
    throw "Missing required firmware in $FirmwareDir. Obtain qcom/a730_sqe.fw (v1.81) and qcom/gmu_gen70000.bin from upstream linux-firmware."
}
$actualSqeSha256 = (Get-FileHash $sqePath -Algorithm SHA256).Hash
if ($actualSqeSha256 -ne $expectedSqeSha256) {
    Write-Warning "a730_sqe.fw SHA-256 ($actualSqeSha256) differs from verified v1.81 ($expectedSqeSha256)."
}

# 2. Install signed WDDM 2.0 driver INF (requires local test-signing + .cat in $DriverDir)
$infPath = Resolve-Path (Join-Path $DriverDir 'adreno730_wddm.inf')
Write-Host "[1/3] Installing WDDM 2.0 driver package ($infPath)..." -ForegroundColor Green
pnputil /add-driver "$infPath" /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259 -and $LASTEXITCODE -ne 3010) {
    throw "pnputil failed ($LASTEXITCODE). Ensure adreno730_wddm.cat/.sys/.dll are test-signed and your test cert is trusted."
}

# Enforce boot-safe StartGpu=0
$svcParams = 'HKLM:\SYSTEM\CurrentControlSet\Services\adreno730_wddm\Parameters'
New-Item -Path $svcParams -Force | Out-Null
Set-ItemProperty -Path $svcParams -Name 'StartGpu' -Type DWord -Value 0

# 3. Install Mesa Turnip Vulkan 1.4.348 ICD system-wide
Write-Host "[2/3] Installing Mesa Turnip Vulkan 1.4.348 ICD..." -ForegroundColor Green
$turnipDir = 'C:\Windows\System32\s22_turnip'
New-Item -ItemType Directory -Force -Path $turnipDir | Out-Null
Copy-Item (Join-Path $DriverDir 'vulkan_freedreno.dll')       "$turnipDir\vulkan_freedreno.dll" -Force
Copy-Item (Join-Path $DriverDir 'freedreno_icd.aarch64.json') "$turnipDir\freedreno_icd.aarch64.json" -Force

$vkReg = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
New-Item -Path $vkReg -Force | Out-Null
Set-ItemProperty -Path $vkReg -Name "$turnipDir\freedreno_icd.aarch64.json" -Type DWord -Value 0

# 4. Optional live activation (immediately restores boot-safe StartGpu=0 afterwards!)
if ($EnableGpuNow) {
    Write-Host "[3/3] Starting Adreno 730 live (and restoring boot-safe StartGpu=0 immediately)..." -ForegroundColor Yellow
    $dev = Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like '*Adreno 730*' } | Select-Object -First 1
    if (-not $dev) {
        throw "Adreno 730 WDDM device not found. Create Root\Adreno730Wddm first (e.g. devcon install adreno730_wddm.inf Root\Adreno730Wddm)."
    }
    try {
        Set-ItemProperty -Path $svcParams -Name 'StartGpu' -Type DWord -Value 1
        Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
        Enable-PnpDevice  -InstanceId $dev.InstanceId -Confirm:$false
    }
    finally {
        Set-ItemProperty -Path $svcParams -Name 'StartGpu' -Type DWord -Value 0
    }
    Write-Host "Adreno 730 GPU is LIVE and StartGpu has been safely reset to 0 for next boot." -ForegroundColor Green
} else {
    Write-Host "[3/3] Installed with boot-safe StartGpu=0. Re-run with -EnableGpuNow to activate GPU live." -ForegroundColor Cyan
}
