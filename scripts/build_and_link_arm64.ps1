$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$LinkExe  = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.52.37025\bin\Hostx64\x64\link.exe"
$LibKm    = "C:\ADK\Lib\10.0.26100.0\km\arm64"
$LibWdf   = "C:\ADK\Lib\wdf\kmdf\arm64\1.33"

Write-Host "[1/3] Compiling ARM64 COFF objects (.obj) with Clang..." -ForegroundColor Cyan
wsl bash "/mnt/c/Users/NetoBR/Desktop/S22 Windows/Windows S22/Windows 11 S22 Drivers & Patchs/scripts/build_drivers_clang_arm64.sh" --objs-only
if ($LASTEXITCODE -ne 0) { throw "clang compilation failed" }

Write-Host "[2/3] Linking ARM64 .sys drivers with Microsoft VS 2026 link.exe..." -ForegroundColor Cyan
$CommonLink = @(
    "/NOLOGO",
    "/MACHINE:ARM64",
    "/SUBSYSTEM:NATIVE,10.00",
    "/DRIVER",
    "/KERNEL",
    "/ENTRY:FxDriverEntry",
    "/INTEGRITYCHECK",
    "/DYNAMICBASE",
    "/HIGHENTROPYVA",
    "/NXCOMPAT",
    "/TSAWARE:NO",
    "/RELEASE",
    "/NODEFAULTLIB",
    "/MERGE:_TEXT=.text",
    "/MERGE:_PAGE=PAGE",
    "/SECTION:.data,RW",
    "/SECTION:INIT,d",
    "/OPT:REF",
    "/OPT:ICF",
    "/MANIFEST:NO",
    "/LIBPATH:$LibKm",
    "/LIBPATH:$LibWdf",
    "WdfDriverEntry.lib",
    "WdfLdr.lib",
    "ntoskrnl.lib",
    "hal.lib",
    "wmilib.lib",
    "BufferOverflowFastFailK.lib"
)

$TouchOut = Join-Path $RepoRoot "drivers\fts2ba61y-touch\ARM64\Release\fts2ba61y"
& $LinkExe @CommonLink "/OUT:$TouchOut\fts2ba61y.sys" "$TouchOut\driver.obj" "$TouchOut\spi_tlmm.obj" "$TouchOut\fts2ba61y_proto.obj"
if ($LASTEXITCODE -ne 0) { throw "link.exe failed for fts2ba61y.sys" }
Remove-Item "$TouchOut\*.obj" -Force
Copy-Item (Join-Path $RepoRoot "drivers\fts2ba61y-touch\fts2ba61y.inf") "$TouchOut\fts2ba61y.inf" -Force

$BattOut = Join-Path $RepoRoot "drivers\max77705-battery\ARM64\Release\max77705batt"
& $LinkExe @CommonLink "battc.lib" "/OUT:$BattOut\max77705batt.sys" "$BattOut\driver.obj" "$BattOut\i2c_tlmm.obj"
if ($LASTEXITCODE -ne 0) { throw "link.exe failed for max77705batt.sys" }
Remove-Item "$BattOut\*.obj" -Force
Copy-Item (Join-Path $RepoRoot "drivers\max77705-battery\max77705batt.inf") "$BattOut\max77705batt.inf" -Force

$UsbCOut = Join-Path $RepoRoot "drivers\max77705-usbc\ARM64\Release\max77705usbc"
& $LinkExe @CommonLink "/OUT:$UsbCOut\max77705usbc.sys" "$UsbCOut\driver.obj" "$UsbCOut\usbc_hw.obj"
if ($LASTEXITCODE -ne 0) { throw "link.exe failed for max77705usbc.sys" }
Remove-Item "$UsbCOut\*.obj" -Force
Copy-Item (Join-Path $RepoRoot "drivers\max77705-usbc\max77705usbc.inf") "$UsbCOut\max77705usbc.inf" -Force

$WlanOut = Join-Path $RepoRoot "drivers\s22-wlan-pdc\ARM64\Release\s22wlanpdc"
& $LinkExe @CommonLink "/OUT:$WlanOut\s22wlanpdc.sys" "$WlanOut\driver.obj"
if ($LASTEXITCODE -ne 0) { throw "link.exe failed for s22wlanpdc.sys" }
Remove-Item "$WlanOut\*.obj" -Force
Copy-Item (Join-Path $RepoRoot "drivers\s22-wlan-pdc\s22wlanpdc.inf") "$WlanOut\s22wlanpdc.inf" -Force

Write-Host "[3/3] Generating .cat, signing, and staging into S901E Drivers..." -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "sign_and_stage_drivers.ps1")
