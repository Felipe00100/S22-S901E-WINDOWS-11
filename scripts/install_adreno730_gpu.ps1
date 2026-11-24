<#
.SYNOPSIS
    Installs the complete Adreno 730 GPU Acceleration Suite on Galaxy S22 Windows 11 ARM64:
      - Adreno 730 WDDM 2.0 KMD (v1.0.0.24, StartGpu=1 auto-start with BootInProgress anti-bootloop watchdog)
      - Mesa 26.1.0 Turnip Vulkan 1.4.348 ICD (vulkan_freedreno.dll)
      - Mesa 26.1.0 Zink OpenGL 4.6 (opengl32.dll + libgallium_wgl.dll)
      - Optional: DXVK ARM64 (d3d8/d3d9/d3d10core/d3d11/dxgi, zlib license) downloaded from official MSYS2 clangarm64 repo
      - Optional: Microsoft Edge ANGLE-Vulkan shortcut configuration
.LICENSE
    MIT License - Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
#>
param(
    [string]$DriverDir   = ".\prebuilt\Adreno730-GPU",
    [string]$FirmwareDir = "C:\Windows\System32\drivers\qcom",
    [string]$InstallRoot = "C:\Program Files\S22-Adreno730-GPU",
    [switch]$DisableAutoStart,
    [switch]$InstallDXVK,
    [switch]$ConfigureEdge
)

$ErrorActionPreference = 'Stop'

# 1. Verify required Qualcomm firmware blobs (must be supplied by user from upstream linux-firmware)
$sqePath = @( (Join-Path $FirmwareDir 'a730_sqe.fw'), 'C:\Windows\System32\drivers\a730_sqe.fw' ) | Where-Object { Test-Path $_ } | Select-Object -First 1
$gmuPath = @( (Join-Path $FirmwareDir 'gmu_gen70000.bin'), 'C:\Windows\System32\drivers\gmu_gen70000.bin' ) | Where-Object { Test-Path $_ } | Select-Object -First 1
$expectedSqeSha256 = '4F8B288F340FF5B4881BED8DFEA90BB4A1B967556CA2DF6D07F4ED4E4A28D071'

if (-not $sqePath -or -not $gmuPath) {
    throw "Missing required firmware in $FirmwareDir (or C:\Windows\System32\drivers). Obtain qcom/a730_sqe.fw (v1.81) and qcom/gmu_gen70000.bin from official linux-firmware."
}
$actualSqeSha256 = (Get-FileHash $sqePath -Algorithm SHA256).Hash
if ($actualSqeSha256 -ne $expectedSqeSha256) {
    Write-Warning "a730_sqe.fw SHA-256 ($actualSqeSha256) differs from verified v1.81 ($expectedSqeSha256)."
}

# 2. Install signed WDDM 2.0 driver INF (requires local test-signing + .cat in $DriverDir)
$infPath = Resolve-Path (Join-Path $DriverDir 'adreno730_wddm.inf')
Write-Host "[1/5] Installing Adreno 730 WDDM 2.0 driver package ($infPath)..." -ForegroundColor Green
pnputil /add-driver "$infPath" /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259 -and $LASTEXITCODE -ne 3010) {
    throw "pnputil failed ($LASTEXITCODE). Ensure adreno730_wddm.cat/.sys/.dll are test-signed and your test cert is trusted."
}

$svcParams = 'HKLM:\SYSTEM\CurrentControlSet\Services\adreno730_wddm\Parameters'
New-Item -Path $svcParams -Force | Out-Null
$startVal = if ($DisableAutoStart) { 0 } else { 1 }
Set-ItemProperty -Path $svcParams -Name 'BootInProgress' -Type DWord -Value 0
Set-ItemProperty -Path $svcParams -Name 'StartGpu'       -Type DWord -Value $startVal

# 3. Install Mesa Turnip Vulkan 1.4.348 ICD system-wide
Write-Host "[2/5] Installing Mesa Turnip Vulkan 1.4.348 ICD..." -ForegroundColor Green
$turnipDir = 'C:\Windows\System32\s22_turnip'
New-Item -ItemType Directory -Force -Path $turnipDir | Out-Null
Copy-Item (Join-Path $DriverDir 'vulkan_freedreno.dll')       "$turnipDir\vulkan_freedreno.dll" -Force
Copy-Item (Join-Path $DriverDir 'freedreno_icd.aarch64.json') "$turnipDir\freedreno_icd.aarch64.json" -Force

$vkReg = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
New-Item -Path $vkReg -Force | Out-Null
Set-ItemProperty -Path $vkReg -Name "$turnipDir\freedreno_icd.aarch64.json" -Type DWord -Value 0

$curPath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
if ($curPath -notlike '*s22_turnip*') {
    [Environment]::SetEnvironmentVariable('Path', "$curPath;$turnipDir", 'Machine')
}
[Environment]::SetEnvironmentVariable('GALLIUM_DRIVER', 'zink', 'Machine')

# 4. Install Mesa Zink OpenGL 4.6 drop-in DLLs
Write-Host "[3/5] Installing Mesa Zink OpenGL 4.6 ($InstallRoot\OpenGL)..." -ForegroundColor Green
$glDir = Join-Path $InstallRoot 'OpenGL'
New-Item -ItemType Directory -Force -Path $glDir | Out-Null
if (Test-Path (Join-Path $DriverDir 'opengl32.dll')) {
    Copy-Item (Join-Path $DriverDir 'opengl32.dll')       "$glDir\opengl32.dll" -Force
    Copy-Item (Join-Path $DriverDir 'libgallium_wgl.dll') "$glDir\libgallium_wgl.dll" -Force
}

# 5. Optional: Download & install official MSYS2 ARM64 DXVK (DX8/9/10/11 over Vulkan, zlib license)
if ($InstallDXVK) {
    Write-Host "[4/5] Downloading official MSYS2 ARM64 DXVK (DirectX 8/9/10/11)..." -ForegroundColor Green
    $dxvkDir = Join-Path $InstallRoot 'DXVK'
    New-Item -ItemType Directory -Force -Path $dxvkDir | Out-Null
    $html = (Invoke-WebRequest -UseBasicParsing 'https://repo.msys2.org/mingw/clangarm64/').Content
    $pkg = [regex]::Matches($html, 'mingw-w64-clang-aarch64-dxvk-[0-9][^"<>]+\.pkg\.tar\.zst') |
           ForEach-Object { $_.Value } | Where-Object { $_ -notlike '*.sig' } | Select-Object -Unique | Sort-Object | Select-Object -Last 1
    if (-not $pkg) { throw "Could not locate mingw-w64-clang-aarch64-dxvk in MSYS2 clangarm64 repo." }
    $tmpPkg = Join-Path $env:TEMP $pkg
    $tmpDir = Join-Path $env:TEMP 's22_dxvk_extract'
    Invoke-WebRequest -UseBasicParsing "https://repo.msys2.org/mingw/clangarm64/$pkg" -OutFile $tmpPkg
    Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
    tar --zstd -xf $tmpPkg -C $tmpDir
    Copy-Item "$tmpDir\clangarm64\bin\*.dll" "$dxvkDir\" -Force
    Remove-Item -Recurse -Force $tmpDir, $tmpPkg -ErrorAction SilentlyContinue
    Write-Host "  Installed DXVK ARM64 ($pkg) to $dxvkDir (copy DLLs next to any ARM64 DX8/9/10/11 app)." -ForegroundColor Cyan
} else {
    Write-Host "[4/5] Skipping DXVK download (pass -InstallDXVK to auto-install ARM64 DXVK)." -ForegroundColor DarkGray
}

# 6. Optional: Configure Microsoft Edge shortcuts for Adreno 730 ANGLE-Vulkan
if ($ConfigureEdge) {
    Write-Host "[5/5] Configuring Microsoft Edge shortcuts for Adreno 730 ANGLE-Vulkan..." -ForegroundColor Green
    $flags = '--use-gl=angle --use-angle=vulkan --disable-features=SkiaGraphite,DawnUseD3D12 --ignore-gpu-blocklist --disable-gpu-compositing --disable-gpu-sandbox --disable-direct-composition'
    $wsh = New-Object -ComObject WScript.Shell
    foreach ($lnk in @('C:\Users\Public\Desktop\Microsoft Edge.lnk', 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Microsoft Edge.lnk')) {
        if (Test-Path $lnk) {
            $sc = $wsh.CreateShortcut($lnk)
            $sc.Arguments = $flags
            $sc.Save()
        }
    }
}

# 7. Restart adapter live if StartGpu=1
if ($startVal -eq 1) {
    $dev = Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like '*Adreno 730*' } | Select-Object -First 1
    if ($dev) {
        Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
        Enable-PnpDevice  -InstanceId $dev.InstanceId -Confirm:$false
        Write-Host "Adreno 730 GPU is LIVE (StartGpu=1 with BootInProgress anti-bootloop watchdog active)!" -ForegroundColor Green
    }
}
