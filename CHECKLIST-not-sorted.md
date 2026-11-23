# S22 Windows 11 ARM64 Checklist (Unordered / Engineering Log)
Updated 2026-11-22 · SM-S901E (`r0q`, SM8450 / Waipio) · Windows 11 Pro ARM64 `26300.9457`

## Core & UEFI (`Mu-Silicium`)
- [x] Mandatory EFS / modem / persist / recovery / GPT backups documented (`scripts/UFS-PARTITIONS-SAFETY.md`)
- [x] Custom `Mu-Silicium` UEFI (`boot.img` v4 on `sda25`) with 15s auto-boot & button-free `Vol+ & Vol-` Enter
- [x] `ChargerMode.efi`: MAX77705 I2C5 telemetry (2 mΩ, %, V/mV, mA, W, °C), 2.5A charging, OLED saver
- [x] `RebootRecovery.efi` + `scripts/Reiniciar_TWRP.cmd`: 1-click Desktop → UEFI → TWRP via PMK8350 `SDAM_2 0x7148` (0 UFS writes)
- [x] `scripts/fc.sh`: TWRP fast-charge helper (`siop_level=100`, cores 5–7 offline, `powersave`)
- [x] `Waipio/DSDT.asl` (`UFS0._STA = 0x0F`) + clean Windows 11 ARM64 boot & clean `rw` shutdown on `sda41`

## Verified Working Drivers & Hardware
- [x] **Touchscreen (`fts2ba61y.sys` v1.0.1.0, `ACPI\STMT0001`)**: STMicroelectronics FTS2BA61Y HID Multitouch over TLMM SPI bit-bang + `PM8350C LDO8/LDO11`
- [x] **USB 3.0 OTG Host (`URS0` + `urssynopsys.sys` + MAX77705 MUIC/Boost)**: USB hubs, mouse, keyboard, mass storage, USB tethering
- [x] **Internal Wi-Fi 6E — 5 GHz (`WCN6855 / QCA6490`, `PCI\VEN_17CB&DEV_1103`)**:
  - [x] UEFI PCIe0 Gen2 x1 link-up (`ep=0x110317CB`), QMP Gen3x1 PHY (`S22Pcie0PhyTbl.h`), `GPIO 204` EGPIO fix, `apps_smmu` emulated bypass (`CB[74] SCTLR=0`, 0 faults)
  - [x] `APIC.aml` (`0x0D` GIC MSI Frame `0x17100000` INTID 64..95) + `MCFG.aml` + `SSDT.asl` (`PCI0` `_CCA=0`, `WPDC`)
  - [x] `s22wlanpdc.sys` v1.0.15.0 lean production MSI bridge (`DWC iMSI-RX → GICD_ISPENDR[2]`)
  - [x] `scripts/patch_qcwlan8280_wc.ps1` (3-word `MmWriteCombined` DMA patch for SM8450 non-coherent PCIe0) — connects & browses cleanly on 5 GHz!

## Next Bring-Up (Experimental / Pending)
- [ ] Wi-Fi 2.4 GHz PA (`PMR735A s3e.e` via AOSS QMP in Windows)
- [ ] Battery driver (`drivers/max77705-battery/` — fix `0x2B` and test live over Wi-Fi)
- [ ] USB-C dynamic controller (`drivers/max77705-usbc/` — hot-plug, PD, Device mode, hub multi-device stability)
- [ ] GPU Adreno 730 (WDDM), Audio (CS35L41/ADSP), Bluetooth, SLPI Sensors, Cameras, Modem/GPS, Haptics, NFC, S0ix
