<#
.SYNOPSIS
    Organiza todos os drivers do Galaxy S22 (SM-S901E / r0q) na pasta "S901E Drivers"
    e opcionalmente injeta todos eles (Waipio + Touchscreen FTS2BA61Y + Bateria MAX77705)
    em uma partição Windows 11 ARM64 montada (ex: W:\) com testsigning habilitado.

.EXAMPLE
    .\stage_s901e_drivers.ps1
    .\stage_s901e_drivers.ps1 -WindowsMount W: -EfiMount S:
#>
param(
    [string]$WindowsMount = "",
    [string]$EfiMount = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot   = Split-Path -Parent $PSScriptRoot
$OutDrivers = Join-Path $RepoRoot "S901E Drivers"

Write-Host "[1/3] Coletando drivers compilados para: $OutDrivers" -ForegroundColor Cyan

# Touchscreen STM FTS2BA61Y
$TouchOut = Join-Path $RepoRoot "drivers\fts2ba61y-touch\ARM64\Release\fts2ba61y"
$TouchDst = Join-Path $OutDrivers "Touchscreen-FTS2BA61Y"
New-Item -ItemType Directory -Force $TouchDst | Out-Null
if (Test-Path "$TouchOut\fts2ba61y.sys") {
    Copy-Item "$TouchOut\*" $TouchDst -Recurse -Force
    Write-Host "  [OK] fts2ba61y.sys copiado para S901E Drivers\Touchscreen-FTS2BA61Y" -ForegroundColor Green
} else {
    Write-Host "  [PENDENTE] fts2ba61y.sys ainda nao compilado (execute drivers\fts2ba61y-touch\build_arm64.bat no VS 2026 + WDK ARM64)." -ForegroundColor Yellow
}

# Bateria / Carregador Maxim MAX77705
$BattOut = Join-Path $RepoRoot "drivers\max77705-battery\ARM64\Release\max77705batt"
$BattDst = Join-Path $OutDrivers "Battery-MAX77705"
New-Item -ItemType Directory -Force $BattDst | Out-Null
if (Test-Path "$BattOut\max77705batt.sys") {
    Copy-Item "$BattOut\*" $BattDst -Recurse -Force
    Write-Host "  [OK] max77705batt.sys copiado para S901E Drivers\Battery-MAX77705" -ForegroundColor Green
}

# USB-C / USB 3.0 SuperSpeed / Dual-Role / USB-PD / DP Alt-Mode Maxim MAX77705
$UsbCOut = Join-Path $RepoRoot "drivers\max77705-usbc\ARM64\Release\max77705usbc"
$UsbCDst = Join-Path $OutDrivers "USB-C-MAX77705"
New-Item -ItemType Directory -Force $UsbCDst | Out-Null
if (Test-Path "$UsbCOut\max77705usbc.sys") {
    Copy-Item "$UsbCOut\*" $UsbCDst -Recurse -Force
    Write-Host "  [OK] max77705usbc.sys copiado para S901E Drivers\USB-C-MAX77705" -ForegroundColor Green
}

# Wi-Fi WCN6855/QCA6490 2.4GHz PA (AOSS QMP s3e.e) Enabler
$WlanOut = Join-Path $RepoRoot "drivers\s22-wlan-pdc\ARM64\Release\s22wlanpdc"
$WlanDst = Join-Path $OutDrivers "WiFi-WCN6855-PDC"
New-Item -ItemType Directory -Force $WlanDst | Out-Null
if (Test-Path "$WlanOut\s22wlanpdc.sys") {
    Copy-Item "$WlanOut\*" $WlanDst -Recurse -Force
    Write-Host "  [OK] s22wlanpdc.sys copiado para S901E Drivers\WiFi-WCN6855-PDC" -ForegroundColor Green
}

# Drivers base Waipio (SM8450) clonados no WSL (nao versionados no Git por direitos autorais)
$WaipioWsl = "\\wsl$\Ubuntu-26.04\home\netobr\windows_silicon_qcom_waipio\Drivers"
$WaipioDst = Join-Path $OutDrivers "Waipio-Base-Drivers"
if (Test-Path $WaipioWsl) {
    Write-Host "[2/3] Sincronizando drivers base Waipio (SM8450) do WSL..." -ForegroundColor Cyan
    Copy-Item "$WaipioWsl\*" $WaipioDst -Recurse -Force
    Write-Host "  [OK] Drivers base Waipio sincronizados em S901E Drivers\Waipio-Base-Drivers" -ForegroundColor Green
} else {
    Write-Host "  [AVISO] Repo windows_silicon_qcom_waipio nao encontrado em $WaipioWsl" -ForegroundColor Yellow
}

if ($WindowsMount -ne "") {
    Write-Host "[3/3] Injetando TODOS os drivers de 'S901E Drivers' na imagem Windows em $WindowsMount ..." -ForegroundColor Cyan
    dism /Image:$WindowsMount /Add-Driver /Driver:"$OutDrivers" /Recurse /ForceUnsigned

    # Garantir copia do certificado de teste (se existir) para facilitar confianca
    $Cert = Join-Path $RepoRoot "drivers\fts2ba61y-touch\S22TestCert.cer"
    if (Test-Path $Cert) {
        Copy-Item $Cert "$WindowsMount\Windows\System32\S22TestCert.cer" -Force
    }

    if ($EfiMount -ne "") {
        Write-Host "Habilitando testsigning e nointegritychecks no BCD em $EfiMount ..." -ForegroundColor Cyan
        bcdedit /store "$EfiMount\EFI\Microsoft\Boot\BCD" /set "{default}" testsigning on
        bcdedit /store "$EfiMount\EFI\Microsoft\Boot\BCD" /set "{default}" nointegritychecks on
    }
    Write-Host "Injecao concluida!" -ForegroundColor Green
} else {
    Write-Host "[3/3] Pasta 'S901E Drivers' pronta. Para injetar no Windows, rode novamente passando -WindowsMount W: -EfiMount S:" -ForegroundColor Gray
}
