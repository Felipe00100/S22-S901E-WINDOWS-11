# Windows 11 ARM64 on Samsung Galaxy S22 5G (`SM-S901E` / `r0q` / SM8450)

Open-source KMDF drivers, ACPI tables, `Mu-Silicium` UEFI patches, replication scripts, and safety documentation for running **Windows 11 Pro ARM64 (`26300.9457`)** on the **Samsung Galaxy S22 5G (Snapdragon 8 Gen 1 / SM8450 / Waipio, codename `r0q`)**.

> [!CAUTION]
> **MANDATORY BACKUP BEFORE DOING ANYTHING!**
> Back up Samsung's irreplaceable modem/IMEI/calibration partitions (`efs`, `sec_efs`, `modemst1`, `modemst2`, `fsg`, `fsc`, `persist`, `recovery`, stock `boot`, and raw GPT headers of `sda`..`sdf`) to a PC **before** modifying UFS partitions or flashing UEFI. Read [`scripts/UFS-PARTITIONS-SAFETY.md`](scripts/UFS-PARTITIONS-SAFETY.md) first!

> [!NOTE]
> **Legal & Licensing Notice**
> Only original open-source drivers (MIT), UEFI/ACPI patches (BSD-2-Clause-Patent / MIT), and automation scripts are distributed here. **Proprietary Qualcomm, Microsoft, and Samsung binaries** (`Waipio-Base-Drivers`, `qcwlan8280.sys`, `wlanfw20.mbn`, `m320.bin`, `bdwlan.*`, Windows ISOs/WIMs, Samsung XBL DXEs, and personal test certificates) are **excluded via `.gitignore`** and must be obtained legally from official sources. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and [`docs/REPLICATION_GUIDE.md`](docs/REPLICATION_GUIDE.md).

---

## Hardware Status (Verified on Real `SM-S901E`)

| Subsystem | Status | Implementation |
| :--- | :---: | :--- |
| **UEFI (`Mu-Silicium` `r0q`) & Boot** | ✅ Working | Custom `Mu-Silicium` on `sda25` (`boot`), 15s auto-boot timer, power-button-free `Vol+ & Vol−` Enter, `UFS0._STA = 0x0F` |
| **Internal UFS Storage (`sda41` NTFS)** | ✅ Working | Clean Windows 11 Pro ARM64 boot & clean shutdown (`rw` mount in TWRP, zero `fsck.ntfs`) |
| **Display (SimpleFB)** | ✅ Working | Native FHD+ panel via UEFI framebuffer |
| **Touchscreen (`fts2ba61y.sys` v1.0.2.0)** | ✅ Working | Custom MIT KMDF HID Multitouch driver (`ACPI\STMT0001`), TLMM SPI bit-bang + `PM8350C LDO8/LDO11` in UEFI |
| **Internal Wi-Fi 6E — 5 GHz (`WCN6855`)** | ✅ Working | UEFI PCIe0 Gen2 x1 + QMP PHY + SMMU `CB[74]` emulated bypass + `APIC 0x0D` GIC MSI Frame + `s22wlanpdc.sys` v1.0.16.0 (`iMSI-RX → GICD_ISPENDR[2]`) + `patch_qcwlan8280_wc.ps1` (`MmWriteCombined` DMA patch) |
| **USB 3.0 OTG Host (Mouse/Keyboard/Hub/Tethering)** | ✅ Working | `URS0` (`urssynopsys.sys`) + MAX77705 MUIC `0x06=0x09` + 5V/1500mA boost in UEFI |
| **UEFI Charger Mode (`ChargerMode.efi`)** | ✅ Working | Live MAX77705 I2C5 telemetry (%, mV, mA, W, °C), 2.5A charging, OLED saver, button-free exit |
| **1-Click TWRP Reboot (`Reiniciar_TWRP.cmd`)** | ✅ Working | ESP flag `\EFI\S22\reboot_twrp.flag` → PMK8350 `SDAM_2 0x7148` via `SPMIDxe` → warm reboot into TWRP (0 UFS writes) |
| **Wi-Fi 2.4 GHz TX (`s3e.e` PA)** | 🔧 Planned | Needs PMR735A `s3e.e` 2.2V PA enable via AOSS QMP (`0x0C300000`) in Windows |
| **Battery % / Charge Limit (`max77705batt.sys`)** | 🧪 Experimental | Source in `drivers/max77705-battery/` — pending live bring-up (currently `_STA = 0` in `SSDT.asl`) |
| **Dynamic USB-C / PD / Device Mode (`max77705usbc.sys`)** | 🧪 Experimental | Source in `drivers/max77705-usbc/` — pending live bring-up (currently `_STA = 0` in `SSDT.asl`) |
| **GPU (Adreno 730), Audio, BT, Sensors, Camera, Modem** | ❌ Not Yet | Future phases (see [`CHECKLIST.md`](CHECKLIST.md)) |

---

## Repository Layout

```text
├── LICENSE                        # MIT License (with BSD-2-Clause-Patent note for UEFI/ACPI)
├── THIRD_PARTY_NOTICES.md         # Full licensing & legal sourcing notice
├── CHECKLIST.md                   # Verified hardware checklist (pt-BR)
├── HANDOFF_NEXT_AI.md             # Technical architecture & next-steps handoff
├── docs/
│   └── REPLICATION_GUIDE.md       # Complete step-by-step replication & backup guide
├── prebuilt/                      # Compiled 100% open-source verified ARM64 drivers (.sys + .inf)
│   ├── Touchscreen-FTS2BA61Y/     #   fts2ba61y.sys (v1.0.2.0)
│   └── WiFi-WCN6855-PDC/          #   s22wlanpdc.sys (v1.0.16.0 hardened MSI bridge)
├── drivers/                       # Full C source code for all custom ARM64 KMDF drivers
│   ├── fts2ba61y-touch/           #   Verified: FTS2BA61Y HID Multitouch
│   ├── s22-wlan-pdc/              #   Verified: DesignWare iMSI-RX -> GICD MSI bridge
│   ├── max77705-battery/          #   Experimental: MAX77705 Fuel Gauge & Charger
│   └── max77705-usbc/             #   Experimental: MAX77705 CCIC / USB-C / PD
├── patches/mu-silicium/           # Working Mu-Silicium & Silicium-ACPI patches + source files
│   ├── mu-silicium-r0q.patch      #   Unified git diff against Project-Silicium/Mu-Silicium @ cc64daf0
│   ├── silicium-acpi-r0q.patch    #   Unified git diff against Project-Silicium/Silicium-ACPI @ f798933
│   ├── PlatformBootManager.c      #   PCIe0 Gen2 x1, WCN6855, SMMU bypass, iMSI-RX, Touch, USB OTG, TWRP flag
│   ├── S22Pcie0PhyTbl.h           #   SM8450 QMP Gen3x1 PHY register table
│   ├── MemoryMapLib.c             #   NoHob MMIO mappings for GCC / PCIe0 AHB & AXI
│   ├── ChargerMode.c / .inf       #   UEFI MAX77705 Charger Mode app
│   ├── RebootRecovery.c / .inf    #   UEFI Reboot to Recovery app
│   └── Boot-Manager.patch         #   15s auto-boot + Vol+/Vol- Enter
├── acpi/
│   ├── r0q/                       #   Working r0q ACPI tables: SSDT.asl/.aml, APIC.dsl/.aml, MCFG.dsl/.aml, AcpiTables.inf
│   └── waipio/                    #   Working Waipio DSDT.asl/.aml (UFS0._STA = 0x0F fix)
└── scripts/
    ├── build_and_link_arm64.ps1   #   Full ARM64 build + link + .cat + test-signing
    ├── sign_and_stage_drivers.ps1 #   Catalog & sign drivers locally with your own test cert
    ├── patch_qcwlan8280_wc.ps1    #   Local 3-word MmWriteCombined DMA patcher for qcwlan8280.sys
    ├── Reiniciar_TWRP.cmd         #   1-click Desktop -> UEFI -> TWRP reboot
    ├── fc.sh                      #   TWRP fast-charge helper (siop_level=100 + powersave)
    └── UFS-PARTITIONS-SAFETY.md   #   Critical UFS partition protection & backup guide
```

---

## Quick Start
1. **Back up everything first** → [`docs/REPLICATION_GUIDE.md#1-mandatory-backups`](docs/REPLICATION_GUIDE.md) & [`scripts/UFS-PARTITIONS-SAFETY.md`](scripts/UFS-PARTITIONS-SAFETY.md).
2. **Build `Mu-Silicium` for `r0q`** using `patches/mu-silicium/mu-silicium-r0q.patch` and `silicium-acpi-r0q.patch` (base commits `cc64daf0` / `f798933`).
3. **Install Windows 11 ARM64**, stage your locally obtained Qualcomm Waipio + WCN6855 drivers, run `scripts/patch_qcwlan8280_wc.ps1` on your `qcwlan8280.sys`, and install our signed `fts2ba61y.sys` + `s22wlanpdc.sys` (`prebuilt/` or rebuilt from `drivers/`).

Full instructions are in **[`docs/REPLICATION_GUIDE.md`](docs/REPLICATION_GUIDE.md)**.

## Credits
- [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) & [Silicium-ACPI](https://github.com/Project-Silicium/Silicium-ACPI)
- [SM-S901E-Linux-Part-Working](https://github.com/Felipe00100/SM-S901E-Linux-Part-Working) / Linux 7.2 mainline SM8450 & `r0q` hardware references
- [WOA-Project](https://github.com/WOA-Project) community
