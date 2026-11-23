# Handoff Guide for Next AI — Windows 11 ARM64 on Samsung Galaxy S22 5G (`SM-S901E` / `r0q` / SM8450)

> **Purpose**: Complete technical handoff so any future AI assistant can continue immediately with zero lost context.
> **Last Updated**: 2026-11-22 (Wi-Fi 6E 5 GHz VERIFIED WORKING & CLEANED!)

---

## 1. Hardware & Environment Summary

| Item | Value |
| :--- | :--- |
| **Device** | Samsung Galaxy S22 5G (`SM-S901E`, Snapdragon 8 Gen 1 / SM8450 / Waipio, codename `r0q`) |
| **ADB Serial (TWRP)** | `RXCT506000Z` |
| **OS Installed** | Windows 11 Pro ARM64 (`26300.9457` pt-BR) on `/dev/block/sda41` (NTFS) |
| **ESP Partition** | `/dev/block/sda40` (FAT32, UEFI boot files + `\EFI\S22\` flags) |
| **Boot / UEFI Partition** | `/dev/block/sda25` (`boot`, `Mu-r0q.img`) |
| **UEFI Source Tree (WSL)** | `/home/netobr/Mu-Silicium` (`cc64daf0` + `Silicium-ACPI` `f798933`) |
| **Linux Reference (READ-ONLY!)** | `/home/netobr/s22-kernel/linux-7.2.11` and `/home/netobr/s22-linux` (**DO NOT MODIFY!**) |
| **GitHub Repository** | `https://github.com/Felipe00100/S22-S901E-WINDOWS-11` |

### Hard Safety Rules (NEVER VIOLATE)
1. **NEVER touch Samsung EFS/modem/TWRP partitions (`#1..#39`).** Only flash `/dev/block/sda25` (`boot`), mount `/dev/block/sda40` (ESP), and mount `/dev/block/sda41` (Windows NTFS).
2. **NEVER run `fsck.ntfs` / `ntfsfix` on `/dev/block/sda41`.**
3. **ALWAYS `sync && umount /tmp/win` and `umount /tmp/esp`** before `adb reboot`.
4. **ALWAYS verify SHA-256** of flashed/deployed files.
5. **NEVER commit proprietary Qualcomm/Samsung/Microsoft binaries** (`S901E Drivers/`, `qcwlan*.sys`, `*.mbn`, `*.elf`, `*.img`, `*.cer`, `*.pfx`) to public GitHub.

---

## 2. Verified Working Milestones on Physical `SM-S901E`

1. **Windows 11 Pro ARM64 Boot & Clean Shutdown**: Custom `Mu-Silicium` (`UFS0._STA = 0x0F`), SimpleFB display, clean `rw` NTFS shutdown.
2. **Touchscreen (`fts2ba61y.sys` v1.0.1.0, `ACPI\STMT0001`)**: TLMM SPI bit-bang (`GPIO 28..31`, `IRQ 46`) + RPMh `ldoc8` (1.8V) / `ldoc11` (3.0V) in UEFI.
3. **USB 3.0 OTG Host (`URS0` + `urssynopsys.sys`)**: MAX77705 MUIC `0x06=0x09` + 5V/1500mA boost in UEFI; USB hubs, mouse, keyboard, mass storage, USB tethering working.
4. **1-Click TWRP Reboot (`scripts/Reiniciar_TWRP.cmd`) & UEFI Charger Mode (`ChargerMode.efi`) & TWRP Fast-Charge (`scripts/fc.sh`)**: All verified.
5. **Internal Wi-Fi 6E — 5 GHz (`WCN6855 / QCA6490`, `PCI\VEN_17CB&DEV_1103&SUBSYS_010817CB`) — VERIFIED CONNECTED & BROWSING!**:
   - **UEFI (`PlatformBootManager.c` + `S22Pcie0PhyTbl.h` + `MemoryMapLib.c`)**: Gen2 x1 link-up (`ep=0x110317CB`), `GPIO 204` (`XO_CLK`) EGPIO bit 12 fix, iATU CFG0 4 KB limit, `apps_smmu` (`0x15000000`) clean emulated bypass (`SMR[112]=0x80011C00`, `S2CR[112]=0x4A`, `CB[74].SCTLR=0`, `CB[74].FSR=0`), `PARF_INT_ALL_MASK |= 0x7F802000`, DesignWare `iMSI-RX` armed at `0x17100040`.
   - **ACPI (`acpi/r0q/`)**: `MCFG.aml` + **`APIC.aml` with `0x0D` GIC MSI Frame (`0x17100000`, INTID 64..95)** (NOT `IORT.aml`, which hangs SM8450 Windows HAL!) + `SSDT.asl` (`PCI0` `_CCA=0`, `WPDC` `ACPI\S22W0001` claiming `msi0..msi7` `0xAD..0xB4`).
   - **Lean Production MSI Bridge (`s22wlanpdc.sys` v1.0.15.0)**: Intercepts Bank 2 MSIs in DWC `iMSI-RX` and writes `Status & GICD_ISENABLER[2]` to `GICD_ISPENDR[2]`. Zero timers, zero file I/O, zero BSOD.
   - **SM8450 Non-Coherent DMA Fix (`scripts/patch_qcwlan8280_wc.ps1` → `qcwlan8280_wc.sys`)**:
     - Root cause discovered via uncached DRAM peek: `WCN6855` wrote valid `MHI_STATE_M0` + `MHI_EE_SBL` events to DRAM and fired MSIs, but `qcwlan8280.sys` (`v1.0.3451.5100`) hardcoded `dmaV3AllocApi = 1` (`0x14002d1f0`) and `AllocateCommonBufferWithBounds(..., MmCached)` (`0x140008448`), causing stale CPU cache reads on SM8450's non-coherent PCIe0.
     - 3-word patch (`0x14002d1f0`, `0x1402a8a20`, `0x1400084d4`) forces `MmAllocateContiguousMemorySpecifyCache(..., MmWriteCombined)` (`Normal Non-Cacheable` on ARM64 — safe for unaligned `ldur x3, [x26, #0x1c]` and 100% coherent with non-snooping PCIe0!).
     - Deployed as `C:\Windows\System32\drivers\qcwlan8280_wc.sys` (`Services\qcwlan\ImagePath`), leaving original `qcwlan8280.sys` untouched. All debug registry keys and dumps cleaned.

---

## 3. Next Bring-Up Tasks (Can Now Be Tested Live Over Internal Wi-Fi!)

With working internal 5 GHz Wi-Fi on the S22, new drivers and registry experiments can be tested live via a LAN PowerShell bridge or SSH in seconds without TWRP:

1. **Wi-Fi 2.4 GHz TX (`PMR735A s3e.e` PA)**:
   - 5 GHz works out of the box; 2.4 GHz needs the 2.2V `s3e.e` PA rail enabled via SM8450 AOSS QMP (`0x0C300000`) + IPCC (`0x0ED18000`) **in Windows** (NEVER in UEFI — `qcpep8450.sys` owns AOSS QMP during boot).
   - Add safe post-boot AOSS QMP `{class: vreg, res: s3e, val: 1}` (after `qcpep8450` init) to `s22wlanpdc.sys` (or test via live IOCTL first!).
2. **Battery Driver (`drivers/max77705-battery/`, `ACPI\MAXM7705`)**:
   - Currently `_STA = 0` in `SSDT.asl`. Debug `PANIC_STACK_SWITCH (0x2B)` (check stack usage / I2C5 TLMM bit-bang spinlocks / `BatteryClassInitializeDevice` callback IRQL!) and test live over Wi-Fi.
3. **USB-C Dynamic Driver (`drivers/max77705-usbc/`, `ACPI\MAXM7706`)**:
   - Currently `_STA = 0` in `SSDT.asl`. Bring up live over Wi-Fi for dynamic hot-plug, multi-device USB hub stability (mouse + keyboard + tethering), USB-PD charging, and USB Device (MTP/NCM) mode.
4. **Future Subsystems**: Adreno 730 GPU (WDDM), Audio (`CS35L41`/ADSP), Bluetooth (`WCN6855` UART), SLPI Sensors, Cameras, Modem/GPS, Haptics, NFC, S0ix.
