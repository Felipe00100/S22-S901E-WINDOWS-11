<#
.SYNOPSIS
    Execute ESTE script como Administrador DENTRO da VM QEMU Windows 11 ARM64
    para instalar o certificado de teste, criar os dispositivos virtuais
    ROOT\STMT0001 (Touchscreen) e ROOT\MAXM7705 (Bateria) e validar ambos os drivers!
#>
$ErrorActionPreference = "Stop"
$DriversRoot = Split-Path -Parent $PSScriptRoot
$S901eDir    = Join-Path $DriversRoot "S901E Drivers"
$CertPath    = Join-Path $S901eDir "S22TestCert.cer"

if (Test-Path $CertPath) {
    Write-Host "[1/4] Instalando certificado de teste em Root e TrustedPublisher..." -ForegroundColor Cyan
    Import-Certificate -FilePath $CertPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $CertPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}

Write-Host "[2/4] Habilitando testsigning no BCD (se ainda nao estiver ativo)..." -ForegroundColor Cyan
$bcd = bcdedit /enum "{current}" | Out-String
if ($bcd -notmatch "testsigning\s+Yes") {
    bcdedit /set testsigning on
    Write-Host "  [AVISO] testsigning ativado! Reinicie a VM e rode este script novamente." -ForegroundColor Yellow
    exit 0
}

Write-Host "[3/4] Instalando drivers via pnputil (System32\drivers)..." -ForegroundColor Cyan
pnputil /add-driver "$S901eDir\Touchscreen-FTS2BA61Y\fts2ba61y.inf" /install
pnputil /add-driver "$S901eDir\Battery-MAX77705\max77705batt.inf" /install

Write-Host "[4/4] Criando dispositivos virtuais ROOT\STMT0001 e ROOT\MAXM7705 (se devgen/pnputil disponivel)..." -ForegroundColor Cyan
if (Get-Command devgen -ErrorAction SilentlyContinue) {
    devgen /add /bus ROOT /hardwareid "ROOT\STMT0001"
    devgen /add /bus ROOT /hardwareid "ROOT\MAXM7705"
} else {
    Write-Host "  Dica: use 'hdwwiz' (Adicionar Hardware Herdado) ou devcon/devgen para criar ROOT\STMT0001 e ROOT\MAXM7705." -ForegroundColor Yellow
}

Write-Host "`nVerificando arquivos em C:\Windows\System32\DriverStore\FileRepository\:" -ForegroundColor Green
Get-ChildItem "C:\Windows\System32\DriverStore\FileRepository\fts2ba61y*", "C:\Windows\System32\DriverStore\FileRepository\max77705batt*" -ErrorAction SilentlyContinue
Get-PnpDevice -FriendlyName "*FTS2BA61Y*", "*MAX77705*" -ErrorAction SilentlyContinue | Format-Table Status, Class, FriendlyName, InstanceId
