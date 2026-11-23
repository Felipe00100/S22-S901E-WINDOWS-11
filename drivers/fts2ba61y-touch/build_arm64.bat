@echo off
REM Build & Test-Sign fts2ba61y.sys (ARM64) using Microsoft EWDK / WDK Developer Command Prompt
echo [1/3] Building fts2ba61y.sys for ARM64 Release...
msbuild fts2ba61y.vcxproj /p:Configuration=Release /p:Platform=ARM64 /t:Rebuild
if errorlevel 1 exit /b 1

echo [2/3] Generating fts2ba61y.cat...
inf2cat /driver:ARM64\Release\fts2ba61y /os:10_VB_ARM64
if errorlevel 1 echo Warning: inf2cat returned non-zero (check OS flag for your WDK version).

echo [3/3] Creating test certificate & signing (requires testsigning on in Windows BCD)...
if not exist S22TestCert.cer (
    makecert -r -pe -ss PrivateCertStore -n "CN=S22 Windows Test Cert" S22TestCert.cer
)
signtool sign /a /v /s PrivateCertStore /n "S22 Windows Test Cert" /fd SHA256 ARM64\Release\fts2ba61y\fts2ba61y.sys ARM64\Release\fts2ba61y\fts2ba61y.cat

echo Done! Output package in ARM64\Release\fts2ba61y\
