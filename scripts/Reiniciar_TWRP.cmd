@echo off
:: Samsung Galaxy S22 (SM-S901E / r0q) — 1-Click Reboot to TWRP Recovery
:: Writes \EFI\S22\reboot_twrp.flag on ESP; Mu-Silicium PlatformBootManager.c
:: detects it at boot, sets PMK8350 SDAM_2 0x7148 bit1, and warm-reboots into TWRP.
:: Requires Administrator privileges.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERRO] Execute este script como Administrador!
    pause
    exit /b 1
)

echo [1/3] Montando particao EFI (ESP) em S:...
mountvol S: /S
if %errorlevel% neq 0 (
    echo [ERRO] Falha ao montar a particao ESP em S:.
    pause
    exit /b 1
)

if not exist "S:\EFI\S22" mkdir "S:\EFI\S22"
echo TWRP > "S:\EFI\S22\reboot_twrp.flag"

echo [2/3] Desmontando S:...
mountvol S: /D

echo [3/3] Reiniciando para o TWRP via Mu-Silicium...
shutdown /r /t 0
