$ErrorActionPreference = "Stop"

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$Inf2Cat   = "C:\ADK\bin\10.0.26100.0\x86\inf2cat.exe"
$SignTool  = "C:\ADK\bin\10.0.26100.0\x64\signtool.exe"
$TouchOut  = Join-Path $RepoRoot "drivers\fts2ba61y-touch\ARM64\Release\fts2ba61y"
$BattOut   = Join-Path $RepoRoot "drivers\max77705-battery\ARM64\Release\max77705batt"
$UsbCOut   = Join-Path $RepoRoot "drivers\max77705-usbc\ARM64\Release\max77705usbc"
$WlanOut   = Join-Path $RepoRoot "drivers\s22-wlan-pdc\ARM64\Release\s22wlanpdc"
$CertFile  = Join-Path $RepoRoot "drivers\fts2ba61y-touch\S22TestCert.cer"

Write-Host "[1/4] Gerando catalogos .cat (inf2cat /os:10_GE_ARM64)..." -ForegroundColor Cyan
& $Inf2Cat "/driver:$TouchOut" /os:10_GE_ARM64 /verbose
& $Inf2Cat "/driver:$BattOut"  /os:10_GE_ARM64 /verbose
& $Inf2Cat "/driver:$UsbCOut"  /os:10_GE_ARM64 /verbose
& $Inf2Cat "/driver:$WlanOut"  /os:10_GE_ARM64 /verbose

Write-Host "[2/4] Verificando / criando certificado de teste S22 Windows Test Cert..." -ForegroundColor Cyan
$Cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=S22 Windows Test Cert" } | Select-Object -First 1
if (-not $Cert) {
    $Cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject "CN=S22 Windows Test Cert" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -NotAfter (Get-Date).AddYears(10)
    Write-Host "  Criado novo certificado: $($Cert.Thumbprint)" -ForegroundColor Green
} else {
    Write-Host "  Usando certificado existente: $($Cert.Thumbprint)" -ForegroundColor Green
}
Export-Certificate -Cert $Cert -FilePath $CertFile -Force | Out-Null

Write-Host "[3/4] Assinando fts2ba61y, max77705batt, max77705usbc e s22wlanpdc (.sys e .cat) com SHA256..." -ForegroundColor Cyan
& $SignTool sign /v /s My /sha1 $Cert.Thumbprint /fd SHA256 `
    "$TouchOut\fts2ba61y.sys" "$TouchOut\fts2ba61y.cat" `
    "$BattOut\max77705batt.sys" "$BattOut\max77705batt.cat" `
    "$UsbCOut\max77705usbc.sys" "$UsbCOut\max77705usbc.cat" `
    "$WlanOut\s22wlanpdc.sys" "$WlanOut\s22wlanpdc.cat"

Write-Host "[4/4] Consolidando todos os drivers em 'S901E Drivers'..." -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "stage_s901e_drivers.ps1")

Write-Host "=== CONCLUIDO: Drivers ARM64 compilados, catalogados (.cat), assinados e em S901E Drivers! ===" -ForegroundColor Green
