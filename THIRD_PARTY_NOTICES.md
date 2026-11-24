# Third-Party Notices, Licensing & Credits

This repository contains **only open-source code, ACPI tables, UEFI/Mesa patches, clean open-source binaries, build/install scripts, and documentation**. No proprietary Microsoft, Qualcomm, or Samsung binaries or firmware blobs are distributed in this repository.

## 1. Original Drivers, Backends, Scripts, and Documentation (MIT License)
The following components are 100% original works licensed under the [MIT License](LICENSE) (`Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors`):
- `drivers/fts2ba61y-touch/` — STMicroelectronics FTS2BA61Y HID Multitouch KMDF driver (`fts2ba61y.sys`)
- `drivers/s22-wlan-pdc/` — SM8450 PCIe0 DesignWare iMSI-RX → GICD MSI bridge KMDF driver (`s22wlanpdc.sys`)
- `drivers/adreno730-wddm/adreno730_hw.c`, `adreno730_hw.h`, `adreno730_display.c`, `adreno730_display.h`, `adreno_wddm_escape.h`, `scm_smc_arm64.asm`, `build_adreno730_gpu.ps1` — SM8450 / Galaxy S22 Adreno 730 5-stage hardware bring-up, Full WDDM 2.0 Display (VidPN, EDID, 60 Hz VSync, Scanout), WDDM 2.0 GpuMmu/Escape ABI, BR+BV concurrent binning sync, UBWC 4.0 initialization, ringbuffer wrap-around, and ARM64 SMC helper
- `patches/mesa-turnip-woa/tu_knl_wddm.cc`, `adreno_wddm_escape.h`, `cross-win-arm64.txt.in`, `build_turnip_arm64.sh`, `freedreno_icd.aarch64.json` — Native Windows WDDM 2.0 kernel backend for Mesa Turnip and Windows ARM64 cross-build tooling
- `drivers/max77705-battery/` — Experimental Maxim MAX77705 Fuel Gauge / Charger KMDF driver (`max77705batt.sys`)
- `drivers/max77705-usbc/` — Experimental Maxim MAX77705 CCIC / USB-C KMDF driver (`max77705usbc.sys`)
- `scripts/` — PowerShell and Shell build, signing, installation, DMA patching, recovery, and fast-charge scripts
- `docs/`, `README.md`, `CHECKLIST.md`

## 2. `adreno730-woa` & `freedreno` Register Database (MIT License)
- **Paths**: `drivers/adreno730-wddm/` (`adreno730_wddm.c`, `adreno730_d3d12.c`, `common/*.c`, `include/adreno/*.h`), `prebuilt/Adreno730-GPU/{adreno730_wddm.sys, adreno730_d3d12.dll, adreno730_wddm.inf}`
- **License**: MIT License (see [`drivers/adreno730-wddm/LICENSE`](drivers/adreno730-wddm/LICENSE))
- **Credits**:
  - `Copyright (c) 2026 Adreno 730 WOA contributors` — base WDDM miniport, D3D12 UMD skeleton, and common GPU/PM4/HFI/SMMU modules.
  - `freedreno` contributors (MIT) — numeric register offsets and PM4 definitions in `include/adreno/a730_regs.h` generated from the MIT-licensed `freedreno` XML register database.

## 3. Mesa 3D / Freedreno / Turnip & Zink (`mesa-turnip-woa`, MIT License)
- **Paths**: `patches/mesa-turnip-woa/mesa-turnip-win-arm64.patch`, `prebuilt/Adreno730-GPU/{vulkan_freedreno.dll, opengl32.dll, libgallium_wgl.dll}`
- **Base Sources**:
  - [Mesa 3D / Freedreno / Turnip / Gallium Zink](https://gitlab.freedesktop.org/mesa/mesa) (Mesa 26.1.0, MIT License)
  - [`arehnman/virtio-win-mesa`](https://github.com/arehnman/virtio-win-mesa) (commit `76c095b`, MIT License)
- **License**: MIT License (see [`patches/mesa-turnip-woa/LICENSE`](patches/mesa-turnip-woa/LICENSE)). All prebuilt DLLs are compiled strictly from these MIT open-source sources; no Microsoft VC++ CRT DLLs are bundled.

## 3b. DXVK ARM64 (DirectX 8/9/10/11 over Vulkan, zlib License — Downloaded on Demand)
- `scripts/install_adreno730_gpu.ps1 -InstallDXVK` optionally downloads the official MSYS2 `clangarm64` build of [doitsujin/dxvk](https://github.com/doitsujin/dxvk) (`mingw-w64-clang-aarch64-dxvk`, zlib/libpng license) directly from `https://repo.msys2.org/mingw/clangarm64/` at install time; DXVK binaries are not committed to this repository.

## 4. Project-Silicium (`Mu-Silicium` & `Silicium-ACPI`) / TianoCore EDK2 (BSD-2-Clause-Patent)
Patches and UEFI source modifications in `patches/mu-silicium/` and ACPI table overrides in `acpi/` extend:
- [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) (BSD-2-Clause-Patent / MIT)
- [Project-Silicium/Silicium-ACPI](https://github.com/Project-Silicium/Silicium-ACPI) (BSD-2-Clause-Patent)
- [TianoCore EDK II / Project Mu](https://github.com/microsoft/mu_basecore) (BSD-2-Clause-Patent)

## 5. Linux Kernel Hardware References (GPL-2.0-only — Reference Only)
Hardware register addresses, SM8450 device-tree constants, PCIe0 QMP Gen3x1 PHY tables, and Adreno 730 clock/SMMU/RSCC/PDC/UBWC 4.0/BR+BV hardware protocol facts were referenced from:
- Upstream Linux kernel (`drivers/gpu/drm/msm/adreno/*`, `drivers/phy/qualcomm/phy-qcom-qmp-pcie.c`, `drivers/pci/controller/dwc/pcie-qcom.c`, `arch/arm64/boot/dts/qcom/sm8450.dtsi`)
- [SM-S901E-Linux-Part-Working](https://github.com/Felipe00100/SM-S901E-Linux-Part-Working)

No GPL-licensed source files are compiled or linked into the Windows ARM64 drivers.

## 6. Proprietary Third-Party Binaries & Runtimes (NOT INCLUDED)
The following proprietary files are **excluded via `.gitignore`** and must be obtained legally by the end user:
- **Qualcomm Adreno 730 GPU Firmware (`a730_sqe.fw` v1.81 & `gmu_gen70000.bin`)**: Obtain from official upstream [`linux-firmware`](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/qcom) under `LICENSE.qcom` and place in `C:\Windows\System32\drivers\qcom\`.
- **Microsoft Visual C++ Redistributable for ARM64** (`vc_redist.arm64.exe`): Required runtime for `vulkan_freedreno.dll` — download directly from Microsoft.
- **Khronos Vulkan Runtime (`vulkan-1.dll` ARM64)**: Install from the official LunarG Vulkan SDK / Runtime for Windows ARM64.
- **Windows 11 ARM64 OS**: Obtain via official Microsoft / UUP dump channels with a valid Windows license.
- **Qualcomm Waipio (`SM8450`) Base SoC Drivers**: Obtained from Qualcomm/Microsoft Windows on ARM driver packages.
- **Qualcomm WCN6855 / QCA6490 Wi-Fi 6E Driver & Firmware** (`qcwlan8280.sys`, `qcwlan8280.inf`, `wlanfw20.mbn`, `m320.bin`, `bdwlan.*`, `regdb.bin`): Obtain from official Qualcomm / Microsoft / Lenovo ThinkPad X13s / Surface Pro 9 5G (`SC8280XP`) driver packages. Run `scripts/patch_qcwlan8280_wc.ps1` locally on your own copy of `qcwlan8280.sys` (v1.0.3451.5100) to generate `qcwlan8280_wc.sys`.
- **Samsung XBL UEFI DXE Binaries (`Mu-Silicium/Binaries/r0q`)**: Extracted locally from your own device's stock Samsung `AP` firmware (`xbl.elf` / `uefi.elf`) per `Mu-Silicium` build instructions.
