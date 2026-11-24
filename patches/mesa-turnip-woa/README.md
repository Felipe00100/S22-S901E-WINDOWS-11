# Mesa 26.1.0 Turnip Vulkan 1.4.348 for Windows 11 ARM64 (`Adreno 730`)

This folder contains our **MIT-licensed** native Windows WDDM 2.0 kernel backend (`tu_knl_wddm.cc`), shared escape ABI (`adreno_wddm_escape.h`), and minimal 27-file Windows ARM64 cross-compilation patch (`mesa-turnip-win-arm64.patch`) for **Mesa 26.1.0 Turnip (`vulkan_freedreno.dll`)**.

## What Works on Physical Galaxy S22 (`SM-S901E` / Adreno 730)
- **Vulkan 1.4.348** via the official Windows Khronos loader (`C:\Windows\System32\vulkan-1.dll`)
- **Native WDDM 2.0 backend (`tu_knl_wddm.cc`)**: dynamic `D3DKMT*` + `DXGI`, `tu_bo` GPU VA allocations (`0x20000000..0x50000000`), binary + timeline `vk_sync`, chunked `ADRENO_WDDM_ESC_SUBMIT_IBS` submission
- **Win32 WSI (`VK_KHR_win32_surface` + `VkSwapchainKHR`)**: verified live on S22 desktop at **355–402 FPS** (`624×441`, 240 continuous frames with ringbuffer wrap-around)
- **Full 3D Hardware Rasterization**: SPIR-V → A730 IR3 graphics pipelines, Gouraud RGB triangle rasterized in **0.15 ms** (`0` pixel mismatches), bit-exact `vkCmdCopyBuffer` / `vkCmdUpdateBuffer` across all sizes

## Key Windows ARM64 Fixes Included in `mesa-turnip-win-arm64.patch`
- **`src/util/cache_ops_aarch64.c`**: Avoids EL0 `mrs ctr_el0` (`0xC000001D` illegal instruction trap on Windows ARM64) by returning SM8450 64-byte cache line geometry under `_WIN32`.
- **`src/freedreno/vulkan/tu_device.cc`**: Guards `close(device->local_fd)` when `local_fd == -1` to prevent Windows UCRT `__fastfail (0xC0000409)` on `vkDestroyInstance`.
- **`src/freedreno/vulkan/tu_clear_blit.cc`**: Caps A2D `CopyBuffer` `block_size` to `4` (`PIPE_FORMAT_R32_UINT`) under `TU_HAS_WDDM` for 100% bit-exact copies across all sizes.
- **`src/vulkan/util/vk_entrypoints_gen.py`**: Emits `extern "C"` non-template declarations for MSVC `/alternatename` compatibility and exact `TU_GENX` dispatch tables.
- **`src/util/u_atomic.h`**: Uses GCC/Clang atomic builtins with `/clang:-mno-outline-atomics /clang:-march=armv8.2-a` (native ARMv8.2 LSE atomics).

## How to Build from Source (WSL2 Ubuntu + Windows MSVC/WDK)
Prerequisites in WSL2: `clang`, `lld`, `llvm`, `meson`, `ninja-build`, `python3-mako`, `python3-yaml`, `bison`, `flex`, `git`.
Prerequisites on Windows host: Visual Studio 2022 MSVC ARM64 + Windows SDK/WDK `10.0.26100.0`.

```bash
chmod +x build_turnip_arm64.sh
MSVC_DIR=/mnt/c/VS2022/VC/Tools/MSVC/14.44.35207 \
WDK_DIR=/mnt/c/ADK \
./build_turnip_arm64.sh
```

Output: `~/s22-turnip-woa/build-woa-arm64/src/freedreno/vulkan/vulkan_freedreno.dll` (~13.3 MB PE32+ ARM64).

## Runtime Prerequisites on the S22 (NOT Bundled)
1. **Microsoft Visual C++ Redistributable for ARM64** (`vc_redist.arm64.exe` from Microsoft) — provides `vcruntime140.dll`, `msvcp140.dll`, etc.
2. **Khronos Vulkan Runtime (`vulkan-1.dll` ARM64)** — installed with the LunarG Vulkan SDK for ARM64 or standard Windows Vulkan runtime.
3. **Adreno 730 WDDM 2.0 KMD (`adreno730_wddm.sys` v1.0.0.23)** + official `linux-firmware` `a730_sqe.fw` & `gmu_gen70000.bin`.

Use [`scripts/install_adreno730_gpu.ps1`](../../scripts/install_adreno730_gpu.ps1) on the S22 to install both the WDDM driver and the Turnip Vulkan ICD.

## Licensing & Credits
Licensed under the **MIT License** (see [`LICENSE`](LICENSE)). Built on upstream [Mesa 3D / Freedreno / Turnip](https://gitlab.freedesktop.org/mesa/mesa) and [`arehnman/virtio-win-mesa`](https://github.com/arehnman/virtio-win-mesa) (commit `76c095b`).
