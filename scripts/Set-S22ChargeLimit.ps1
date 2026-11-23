<#
.SYNOPSIS
    Define o limite de carregamento (50% a 100%) do MAX77705 no Galaxy S22 (SM-S901E).
    Ao atingir o limite, o driver max77705batt.sys mantem o S22 em modo Buck-only (energia direto do USB sem carregar a bateria).
.EXAMPLE
    .\Set-S22ChargeLimit.ps1 -Limit 80
#>
param(
    [ValidateRange(50, 100)]
    [int]$Limit = 80
)

$BattParams = "HKLM:\SYSTEM\CurrentControlSet\Services\max77705batt\Parameters"
New-Item -Path $BattParams -Force | Out-Null
Set-ItemProperty -Path $BattParams -Name "ChargeControlEndThreshold" -Value $Limit -Type DWord
Write-Host "Limite de carga do Galaxy S22 (MAX77705) atualizado para $Limit%." -ForegroundColor Green
Write-Host "Reinicie o dispositivo ou recarregue o driver de bateria para aplicar imediatamente." -ForegroundColor Yellow
