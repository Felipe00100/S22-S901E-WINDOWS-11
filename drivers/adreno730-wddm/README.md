# Adreno 730 WDDM 2.0 Render-Only KMD + D3D12 UMD (`SM-S901E` / SM8450)

Open-source **MIT-licensed** Windows 11 ARM64 WDDM 2.0 GpuMmu kernel miniport (`adreno730_wddm.sys`, v1.0.0.23) and D3D12 user-mode driver (`adreno730_d3d12.dll`) for the **Qualcomm Adreno 730 (`chip_id = 0x07030001`)** on Samsung Galaxy S22 5G (`SM-S901E` / `r0q`).

Pairs directly with **[Mesa 26.1.0 Turnip Vulkan 1.4.348 for Windows ARM64](../../patches/mesa-turnip-woa/README.md)** to deliver real 3D hardware-accelerated Vulkan graphics on physical S22 silicon (**355–402 FPS** live Win32 WSI Swapchain verified).

## Key Features
- **Full 5-stage SM8450 Hardware Bring-Up (`adreno730_hw.c`)**:
  1. `GCC` + `GPUCC` clocks, warm `GPU_GX_GDSC` collapse, `GPU_CX_GDSC`, and `gpu_smmu` vote.
  2. `SMMU-500` (`CB0` GPU + `CB5` GMU, `2 MiB` page-table pool = 1 GiB VA capacity, ARM64 `MmWriteCombined` non-coherent DMA, EL0/EL1 `AP` permission split).
  3. `RSCC` + `GMU_PDC` RPMh initialization + GMU Cortex-M3 boot (`gmu_gen70000.bin`).
  4. Live Qualcomm `cmd-db @ 0x80860000` parsing + HFI `PERF_TABLE`, `BW_TABLE`, `CORE_FW_START`, `START`, `GX_BW_PERF_VOTE`.
  5. `GMU_OOB_GPU_SET`, A730 **UBWC 4.0** init, `a730_sqe.fw` v1.81 boot, full **BR + BV concurrent binning** submission (`a7xx_submit()`), 19.2 MHz `CP_ALWAYS_ON_COUNTER` timestamps, and continuous ringbuffer wrap-around (`CP_RB_RPTR` sync).
- **Boot-Safe by Default (`StartGpu = 0`)**:
  - `adreno730_wddm.inf` installs with `StartGpu = 0` so Windows 11 always boots safely even after hard resets. Activate live via `scripts/install_adreno730_gpu.ps1 -EnableGpuNow` (which starts the GPU and immediately reverts `StartGpu = 0` for the next boot).

## Required Firmware Blobs (NOT Included)
Qualcomm GPU firmware blobs are proprietary and **not distributed** in this repository. Obtain them legally from official upstream [`linux-firmware`](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/qcom) (`LICENSE.qcom`) and copy them to `C:\Windows\System32\drivers\qcom\` on the S22:
- `qcom/a730_sqe.fw` (v1.81, `75,924` bytes, SHA-256: `4F8B288F340FF5B4881BED8DFEA90BB4A1B967556CA2DF6D07F4ED4E4A28D071`)
- `qcom/gmu_gen70000.bin` (`57,272` bytes)

## Build from Source
Requires Visual Studio 2022 MSVC ARM64 + Windows WDK `10.0.26100.0`:
```powershell
powershell -ExecutionPolicy Bypass -File .\drivers\adreno730-wddm\build_adreno730_gpu.ps1
```
Then sign `build\arm64\adreno730_wddm.sys`, `adreno730_d3d12.dll`, and its `.cat` catalog with your own local test certificate (see [`docs/REPLICATION_GUIDE.md`](../../docs/REPLICATION_GUIDE.md)).

## Licensing & Credits
Licensed under the **MIT License** (see [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md)):
- Original SM8450 S22 hardware bring-up, WDDM 2.0 GpuMmu/Escape upgrades, BR+BV/UBWC/ringbuffer fixes, and ARM64 SMC helper by Felipe00100 & S22 Windows Community Project Contributors (MIT).
- Derived from `adreno730-woa` (MIT) and the `freedreno` register XML database (MIT).
- Hardware protocol & register sequences informed by upstream Linux kernel `drivers/gpu/drm/msm/adreno` (GPL-2.0-only, reference-only; no GPL code is compiled or linked).
