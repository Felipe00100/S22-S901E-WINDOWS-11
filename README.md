# Windows 11 ARM64 on Samsung Galaxy S22 5G (`SM-S901E` / `r0q` / SM8450)

Open-source ARM64 drivers (including **Adreno 730 WDDM 2.0 + Mesa 26.1.0 Turnip Vulkan 1.4.348 3D Hardware Acceleration**), ACPI tables, `Mu-Silicium` UEFI patches, replication scripts, and safety documentation for running **Windows 11 Pro ARM64 (`26300.9457`)** on the **Samsung Galaxy S22 5G (Snapdragon 8 Gen 1 / SM8450 / Waipio, codename `r0q`)**.

> [!CAUTION]
> **MANDATORY BACKUP BEFORE DOING ANYTHING!**
> Back up Samsung's irreplaceable modem/IMEI/calibration partitions (`efs`, `sec_efs`, `modemst1`, `modemst2`, `fsg`, `fsc`, `persist`, `recovery`, stock `boot`, and raw GPT headers of `sda`..`sdf`) to a PC **before** modifying UFS partitions or flashing UEFI. Read [`scripts/UFS-PARTITIONS-SAFETY.md`](scripts/UFS-PARTITIONS-SAFETY.md) first!

> [!NOTE]
> **Legal & Licensing Notice**
> Only open-source drivers (MIT), Mesa Turnip patches (MIT), UEFI/ACPI patches (BSD-2-Clause-Patent / MIT), and replication scripts (MIT) are distributed here. **Proprietary Qualcomm, Microsoft, and Samsung binaries** (`a730_sqe.fw`, `gmu_gen70000.bin`, `Waipio-Base-Drivers`, `qcwlan8280.sys`, `wlanfw20.mbn`, `m320.bin`, `bdwlan.*`, VC++ CRT DLLs, Windows ISOs/WIMs, Samsung XBL DXEs, and personal test certificates) are **excluded via `.gitignore`** and must be obtained legally from official sources. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and [`docs/REPLICATION_GUIDE.md`](docs/REPLICATION_GUIDE.md).

---

## Hardware Status (Verified on Real `SM-S901E`)

| Subsystem | Status | Implementation |
| :--- | :---: | :--- |
| **GPU 3D Acceleration, Full WDDM 2.0 Display & Native DirectX 12 (Adreno 730)** | ✅ Working | **`adreno730_wddm.sys` + `adreno730_d3d12.dll` v1.0.0.39** (Full WDDM 2.0 Render + Display: `S22 OLED` `1080×2340 @ 60Hz` VidPN/EDID/VSync, `StartGpu=1` + `EnableDisplay=1` default cold-reboot verified + `BootInProgress` anti-bootloop watchdog) + **100% Original MIT Native DirectX 12 Hardware Stack** (`adreno_ir3_asm` + `adreno_a730_3d` + `adreno_dxbc_ir3` compiling standard HLSL `vs_5_0`/`ps_5_0`/`cs_5_0` directly to A730 64-bit IR3: `1.22 ms` native D3D12 3D triangle + 2D HW Clear + `cs_5_0` Compute) + **Mesa 26.1.0 Turnip Vulkan 1.4.348** (`355–402 FPS`) + **Mesa 26.1.0 Zink OpenGL 4.6** (`61.9 FPS`) + **DXVK 3.1.1 ARM64 DirectX 8/9/10/11** (`3.64 ms`) + **Edge WebGL2** (`60 draws / 0.50 ms`) |
| **UEFI (`Mu-Silicium` `r0q`) & Boot** | ✅ Working | Custom `Mu-Silicium` on `sda25` (`boot`), 15s auto-boot timer, power-button-free `Vol+ & Vol−` Enter, `UFS0._STA = 0x0F` |
| **Internal UFS Storage (`sda41` NTFS)** | ✅ Working | Clean Windows 11 Pro ARM64 boot & clean shutdown (`rw` mount in TWRP, zero `fsck.ntfs`) |
| **Display (SimpleFB)** | ✅ Working | Native FHD+ panel via UEFI framebuffer |
| **Touchscreen (`fts2ba61y.sys` v1.0.2.0)** | ✅ Working | Custom MIT KMDF HID Multitouch driver (`ACPI\STMT0001`), TLMM SPI bit-bang + `PM8350C LDO8/LDO11` in UEFI |
| **Internal Wi-Fi 6E — 5 GHz (`WCN6855`)** | ✅ Working | UEFI PCIe0 Gen2 x1 + QMP PHY + SMMU `CB[74]` emulated bypass + `APIC 0x0D` GIC MSI Frame + `s22wlanpdc.sys` v1.0.19.0 (`iMSI-RX → GICD_ISPENDR[2]`) + `patch_qcwlan8280_wc.ps1` (`MmWriteCombined` DMA patch) |
| **USB 3.0 OTG Host (Mouse/Keyboard/Hub/Tethering)** | ✅ Working | `URS0` (`urssynopsys.sys`) + MAX77705 MUIC `0x06=0x09` + 5V/1500mA boost in UEFI |
| **UEFI Charger Mode (`ChargerMode.efi`)** | ✅ Working | Live MAX77705 I2C5 telemetry (%, mV, mA, W, °C), 2.5A charging, OLED saver, button-free exit |
| **1-Click TWRP Reboot (`Reiniciar_TWRP.cmd`)** | ✅ Working | ESP flag `\EFI\S22\reboot_twrp.flag` → PMK8350 `SDAM_2 0x7148` via `SPMIDxe` → warm reboot into TWRP (0 UFS writes) |
| **TWRP Fast-Charge (`scripts/fc.sh`)** | ✅ Working | Clears `VOTER_USB_100MA` (`usb_conf_test=2`), `siop_level=100`, cores 1–7 offline, `powersave` |
| **D3D12 Textures/Samplers/DXGI SwapChain + DPU Scanout / DWM Offload** | 🔧 Next Phase | Native D3D12 2D Clear + 3D VS/PS + CS HLSL→IR3 & Full WDDM Display (`EnableDisplay=1` default) are LIVE; next: D3D12 SRV/Samplers + DXGI `Present` + SM8450 DPU scanout blit for native `dwm.exe` desktop composition |
| **Wi-Fi 2.4 GHz TX (`s3e.e` PA)** | 🔧 Planned | Needs PMR735A `s3e.e` 2.2V PA enable via AOSS QMP (`0x0C300000`) in Windows |
| **Battery % / USB-C PD (`max77705batt.sys` / `max77705usbc.sys`)** | 🧪 Experimental | Source in `drivers/max77705-*` — pending live bring-up (currently `_STA = 0` in `SSDT.asl`) |
| **Audio, BT, Sensors, Camera, Modem** | ❌ Not Yet | Future phases (see [`CHECKLIST.md`](CHECKLIST.md)) |

---

## Repository Layout

```text
├── LICENSE                        # MIT License (+ component license summary)
├── THIRD_PARTY_NOTICES.md         # Full licensing, credits & legal sourcing notice
├── CHECKLIST.md                   # Verified hardware checklist (pt-BR)
├── docs/
│   └── REPLICATION_GUIDE.md       # Complete step-by-step replication, GPU & backup guide
├── prebuilt/                      # Compiled 100% open-source verified ARM64 binaries (MIT, unsigned)
│   ├── Adreno730-GPU/             #   adreno730_wddm.sys + adreno730_d3d12.dll (v1.0.0.39), vulkan_freedreno.dll, opengl32.dll (Mesa 26.1.0)
│   ├── Touchscreen-FTS2BA61Y/     #   fts2ba61y.sys (v1.0.2.0)
│   └── WiFi-WCN6855-PDC/          #   s22wlanpdc.sys (v1.0.19.0 hardened MSI bridge)
├── drivers/                       # Full open-source driver code (MIT)
│   ├── adreno730-wddm/            #   Verified: Adreno 730 WDDM 2.0 KMD + D3D12 UMD + build script
│   ├── fts2ba61y-touch/           #   Verified: FTS2BA61Y HID Multitouch
│   ├── s22-wlan-pdc/              #   Verified: DesignWare iMSI-RX -> GICD MSI bridge
│   ├── max77705-battery/          #   Experimental: MAX77705 Fuel Gauge & Charger
│   └── max77705-usbc/             #   Experimental: MAX77705 CCIC / USB-C / PD
├── patches/
│   ├── mesa-turnip-woa/           #   Mesa 26.1.0 Turnip Windows ARM64 WDDM backend (tu_knl_wddm.cc) + patch + build script
│   └── mu-silicium/               #   Working Mu-Silicium & Silicium-ACPI UEFI patches (cc64daf0 / f798933)
├── acpi/
│   ├── r0q/                       #   Working r0q ACPI tables: SSDT, APIC (0x0D MSI), MCFG, AcpiTables.inf
│   └── waipio/                    #   Working Waipio DSDT (UFS0._STA = 0x0F fix)
└── scripts/
    ├── install_adreno730_gpu.ps1  #   Install Adreno 730 WDDM 2.0 + Turnip Vulkan 1.4.348 ICD (boot-safe)
    ├── build_and_link_arm64.ps1   #   ARM64 build + link + .cat + test-signing for KMDF drivers
    ├── build_drivers_clang_arm64.sh # Clang ARM64 helper called by build_and_link_arm64.ps1
    ├── sign_and_stage_drivers.ps1 #   Catalog & sign drivers locally with your own test cert
    ├── stage_s901e_drivers.ps1    #   Stage and inject S901E drivers into a mounted Windows image
    ├── patch_qcwlan8280_wc.ps1    #   Local 3-word MmWriteCombined DMA patcher for qcwlan8280.sys
    ├── S22-USB-SSH-MTP-Setup.ps1  #   Configure OpenSSH + USB networking on S22
    ├── Set-S22ChargeLimit.ps1     #   Configure MAX77705 charge limit (50–100%)
    ├── Reiniciar_TWRP.cmd         #   1-click Desktop -> UEFI -> TWRP reboot
    ├── fc.sh                      #   TWRP fast-charge helper (usb_conf_test=2 + siop_level=100)
    └── UFS-PARTITIONS-SAFETY.md   #   Critical UFS partition protection & backup guide
```

---

## Quick Start
1. **Back up everything first** → [`docs/REPLICATION_GUIDE.md#1-mandatory-backups`](docs/REPLICATION_GUIDE.md) & [`scripts/UFS-PARTITIONS-SAFETY.md`](scripts/UFS-PARTITIONS-SAFETY.md).
2. **Build `Mu-Silicium` for `r0q`** using `patches/mu-silicium/mu-silicium-r0q.patch` and `silicium-acpi-r0q.patch` (base commits `cc64daf0` / `f798933`).
3. **Install Windows 11 ARM64**, stage your locally obtained Qualcomm Waipio + WCN6855 drivers, run `scripts/patch_qcwlan8280_wc.ps1` on your `qcwlan8280.sys`, and install our signed `fts2ba61y.sys` + `s22wlanpdc.sys`.
4. **Enable Adreno 730 3D Vulkan 1.4.348 Acceleration**: place official `linux-firmware` `a730_sqe.fw` (v1.81) + `gmu_gen70000.bin` in `C:\Windows\System32\drivers\qcom\`, install Microsoft VC++ ARM64 Redistributable & Khronos Vulkan Runtime (`vulkan-1.dll`), sign `prebuilt/Adreno730-GPU/` (or rebuild from `drivers/adreno730-wddm/` + `patches/mesa-turnip-woa/`), and run `scripts/install_adreno730_gpu.ps1 -EnableGpuNow`.

Full instructions are in **[`docs/REPLICATION_GUIDE.md`](docs/REPLICATION_GUIDE.md)**.

## Credits
- **[Mesa 3D / Freedreno / Turnip](https://gitlab.freedesktop.org/mesa/mesa)** & **[`arehnman/virtio-win-mesa`](https://github.com/arehnman/virtio-win-mesa)** — Mesa 26.1.0 Turnip Vulkan 1.4 driver (MIT)
- **`adreno730-woa` contributors** — Base Adreno 730 WDDM/D3D12/common framework (MIT)
- **[Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)** & **[Silicium-ACPI](https://github.com/Project-Silicium/Silicium-ACPI)** — UEFI/ACPI platform base (BSD-2-Clause-Patent / MIT)
- **[SM-S901E-Linux-Part-Working](https://github.com/Felipe00100/SM-S901E-Linux-Part-Working)** & **Linux Kernel `drm/msm` / SM8450 mainline** — Hardware register & protocol references
- **[WOA-Project](https://github.com/WOA-Project)** community
