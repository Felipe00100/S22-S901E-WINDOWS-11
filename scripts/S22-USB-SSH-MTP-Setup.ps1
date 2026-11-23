<#
.SYNOPSIS
    Configura USB Device Mode (MTP + USB NCM/RNDIS para SSH via USB), OpenSSH Server,
    PCIe ASPM Wi-Fi powersave e limite de carga no Galaxy S22 (SM-S901E) com Windows 11 ARM64.
#>
param(
    [string]$UsbStaticIp = "172.16.42.1",
    [int]$ChargeLimitPercent = 100
)

$ErrorActionPreference = "Continue"
Write-Host "=== Galaxy S22 (SM-S901E) Windows 11 ARM64 USB-SSH / MTP / Power Setup ===" -ForegroundColor Cyan

# 1. Configurar USB Function Class (USBFN: Composite MTP + USB-NCM/RNDIS) para UFX Synopsys
$UsbFnKey = "HKLM:\SYSTEM\CurrentControlSet\Control\USBFN"
New-Item -Path "$UsbFnKey\Configurations\S22Composite" -Force | Out-Null
Set-ItemProperty -Path $UsbFnKey -Name "CurrentConfiguration" -Value "S22Composite" -Type String
Set-ItemProperty -Path "$UsbFnKey\Configurations\S22Composite" -Name "InterfaceList" -Value @("MTP", "RNDIS") -Type MultiString
Set-ItemProperty -Path "$UsbFnKey\Configurations\S22Composite" -Name "idVendor"  -Value 0x04E8 -Type DWord  # Samsung
Set-ItemProperty -Path "$UsbFnKey\Configurations\S22Composite" -Name "idProduct" -Value 0x6860 -Type DWord  # Composite MTP + Network
Set-ItemProperty -Path "$UsbFnKey\Configurations\S22Composite" -Name "ManufacturerString" -Value "Samsung / S22 Windows" -Type String
Set-ItemProperty -Path "$UsbFnKey\Configurations\S22Composite" -Name "ProductString"      -Value "Galaxy S22 (SM-S901E) MTP + USB-SSH" -Type String

# 2. Habilitar OpenSSH Server + Regra de Firewall TCP 22 (SSH via USB no boot)
$sshCap = Get-WindowsCapability -Online -Name "OpenSSH.Server~~~~0.0.1.0" -ErrorAction SilentlyContinue
if ($sshCap -and $sshCap.State -ne "Installed") {
    Add-WindowsCapability -Online -Name "OpenSSH.Server~~~~0.0.1.0" -ErrorAction SilentlyContinue | Out-Null
}
if (Get-Service sshd -ErrorAction SilentlyContinue) {
    Set-Service -Name sshd -StartupType Automatic
    Start-Service sshd -ErrorAction SilentlyContinue
}
if (-not (Get-NetFirewallRule -Name "S22-OpenSSH-USB" -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -Name "S22-OpenSSH-USB" -DisplayName "S22 OpenSSH Server (USB/Wi-Fi)" `
        -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22 | Out-Null
}

# 3. Aplicar IP estatico 172.16.42.1/24 em qualquer adaptador USB NCM/RNDIS detectado
$usbNics = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object {
    $_.InterfaceDescription -match "RNDIS|NCM|USB Ethernet|USB Network"
}
foreach ($nic in $usbNics) {
    Remove-NetIPAddress -InterfaceIndex $nic.ifIndex -Confirm:$false -ErrorAction SilentlyContinue
    New-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $UsbStaticIp -PrefixLength 24 -ErrorAction SilentlyContinue | Out-Null
    Write-Host "  [OK] USB SSH IP configurado: $UsbStaticIp/24 em $($nic.Name)" -ForegroundColor Green
}

# 4. Configurar PCIe ASPM powersave (para WCN6855 / QCA6490 Wi-Fi 2.4GHz + 5GHz)
powercfg /setacvalueindex SCHEME_CURRENT SUB_PCIEXPRESS ASPM 2 2>$null
powercfg /setdcvalueindex SCHEME_CURRENT SUB_PCIEXPRESS ASPM 2 2>$null
powercfg /setactive SCHEME_CURRENT 2>$null

# 5. Configurar limite de carga MAX77705 (50..100%)
if ($ChargeLimitPercent -ge 50 -and $ChargeLimitPercent -le 100) {
    $BattParams = "HKLM:\SYSTEM\CurrentControlSet\Services\max77705batt\Parameters"
    New-Item -Path $BattParams -Force | Out-Null
    Set-ItemProperty -Path $BattParams -Name "ChargeControlEndThreshold" -Value $ChargeLimitPercent -Type DWord
    Write-Host "  [OK] Limite de carga MAX77705 definido para $ChargeLimitPercent% (Buck-only power-path ao atingir limite)." -ForegroundColor Green
}

Write-Host "=== Setup concluido! Conecte via: ssh usuario@$UsbStaticIp ===" -ForegroundColor Green
