# Third-Party Notices & Licensing

This repository contains **only open-source code, ACPI tables, UEFI patches, build/patch scripts, and documentation**. No proprietary Microsoft, Qualcomm, or Samsung binaries are distributed in this repository.

## 1. Original Drivers, Scripts, and Documentation (MIT License)
The following components are original works licensed under the [MIT License](LICENSE):
- `drivers/fts2ba61y-touch/` — STMicroelectronics FTS2BA61Y HID Multitouch KMDF driver (`fts2ba61y.sys`)
- `drivers/s22-wlan-pdc/` — SM8450 PCIe0 DesignWare iMSI-RX → GICD MSI bridge KMDF driver (`s22wlanpdc.sys`)
- `drivers/max77705-battery/` — Experimental Maxim MAX77705 Fuel Gauge / Charger KMDF driver (`max77705batt.sys`)
- `drivers/max77705-usbc/` — Experimental Maxim MAX77705 CCIC / USB-C KMDF driver (`max77705usbc.sys`)
- `prebuilt/` — Compiled ARM64 binaries built strictly from the MIT-licensed open-source drivers above (`fts2ba61y.sys`, `s22wlanpdc.sys`)
- `scripts/` — PowerShell, Python, and Shell build/signing/patching/recovery automation scripts (including `patch_qcwlan8280_wc.ps1` and `fc.sh`)
- `docs/`, `README.md`, `CHECKLIST.md`, `HANDOFF_NEXT_AI.md`

## 2. Project-Silicium (`Mu-Silicium` & `Silicium-ACPI`) / TianoCore EDK2 (BSD-2-Clause-Patent)
Patches and UEFI source modifications in `patches/mu-silicium/` and ACPI table overrides in `acpi/` extend:
- [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) (BSD-2-Clause-Patent / MIT)
- [Project-Silicium/Silicium-ACPI](https://github.com/Project-Silicium/Silicium-ACPI) (BSD-2-Clause-Patent)
- [TianoCore EDK II / Project Mu](https://github.com/microsoft/mu_basecore) (BSD-2-Clause-Patent)

## 3. Linux Kernel Hardware Reference (GPL-2.0-only)
Hardware initialization register sequences for SM8450 PCIe0 QMP Gen3x1 PHY (`patches/mu-silicium/S22Pcie0PhyTbl.h`) and device-tree address/IRQ constants were derived from upstream Linux kernel drivers (`drivers/phy/qualcomm/phy-qcom-qmp-pcie.c`, `drivers/pci/controller/dwc/pcie-qcom.c`, `arch/arm64/boot/dts/qcom/sm8450.dtsi`) and the Samsung Galaxy S22 mainline Linux port (`SM-S901E-Linux-Part-Working`).

## 4. Proprietary Third-Party Binaries (NOT INCLUDED)
The following proprietary files are **excluded via `.gitignore`** and must be obtained legally by the end user:
- **Windows 11 ARM64 OS**: Obtain via official Microsoft / UUP dump channels with a valid Windows license.
- **Qualcomm Waipio (`SM8450`) Base SoC Drivers**: Obtained from Qualcomm/Microsoft Windows on ARM driver packages (e.g., WOA / Surface / Qualcomm Reference Device packages).
- **Qualcomm WCN6855 / QCA6490 Wi-Fi 6E Driver & Firmware** (`qcwlan8280.sys`, `qcwlan8280.inf`, `wlanfw20.mbn`, `m320.bin`, `bdwlan.*`, `regdb.bin`): Obtain from official Qualcomm / Microsoft / Lenovo ThinkPad X13s / Surface Pro 9 5G (`SC8280XP`) driver packages. Run `scripts/patch_qcwlan8280_wc.ps1` locally on your own copy of `qcwlan8280.sys` (v1.0.3451.5100) to generate `qcwlan8280_wc.sys` for SM8450 non-coherent PCIe0.
- **Samsung XBL UEFI DXE Binaries (`Mu-Silicium/Binaries/r0q`)**: Extracted locally from your own device's stock Samsung `AP` firmware (`xbl.elf` / `uefi.elf`) per `Mu-Silicium` build instructions.
