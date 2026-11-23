@echo off
REM Build & Test-Sign max77705batt.sys (ARM64) using Visual Studio + WDK / EWDK
echo [1/3] Building max77705batt.sys for ARM64 Release...
msbuild max77705batt.vcxproj /p:Configuration=Release /p:Platform=ARM64 /t:Rebuild
if errorlevel 1 exit /b 1

echo [2/3] Generating max77705batt.cat...
inf2cat /driver:ARM64\Release\max77705batt /os:10_VB_ARM64
if errorlevel 1 echo Warning: inf2cat returned non-zero (check OS flag for your WDK version).

echo [3/3] Signing with S22 Windows Test Cert (requires testsigning on in Windows BCD)...
if not exist ..\fts2ba61y-touch\S22TestCert.cer (
    makecert -r -pe -ss PrivateCertStore -n "CN=S22 Windows Test Cert" ..\fts2ba61y-touch\S22TestCert.cer
)
signtool sign /a /v /s PrivateCertStore /n "S22 Windows Test Cert" /fd SHA256 ARM64\Release\max77705batt\max77705batt.sys ARM64\Release\max77705batt\max77705batt.cat

echo Done! Output package in ARM64\Release\max77705batt\
