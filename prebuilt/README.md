# Prebuilt Open-Source ARM64 Drivers (Verified on Hardware)

This folder contains **only 100% open-source, MIT-licensed drivers compiled directly from this repository's `drivers/` and `patches/mesa-turnip-woa/` trees** and verified on physical **Samsung Galaxy S22 5G (`SM-S901E` / `r0q`)** hardware running Windows 11 Pro ARM64 (`26300.9457`):

| Folder | Binaries | Version | License | Status | Hardware |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `Adreno730-GPU/` | `adreno730_wddm.sys`, `adreno730_d3d12.dll`, `adreno730_wddm.inf`, `vulkan_freedreno.dll`, `freedreno_icd.aarch64.json`, `opengl32.dll`, `libgallium_wgl.dll` | KMD+UMD `1.0.0.50` (Full WDDM 2.0 Display + Scanout Blit + 100% Original MIT D3D12 HW 2D/3D/CS/TPL1 Textures/Win32 SwapChain) / Mesa `26.1.0` Turnip (Vulkan `1.4.348`) + Zink (OpenGL `4.6`) | **MIT** | **Verified (Native D3D12 3D 1.66 ms, TPL1 Textures 0.87 ms, CS, Win32 SwapChain 10/10 Present, Vulkan 402 FPS, OpenGL 4.6 62 FPS, DXVK DX11, Edge WebGL2)** | Qualcomm Adreno 730 Full WDDM 2.0 + Native D3D12 + Turnip Vulkan 1.4.348 + Zink OpenGL 4.6 |
| `Touchscreen-FTS2BA61Y/` | `fts2ba61y.sys` + `fts2ba61y.inf` | `1.0.2.0` | **MIT** | **Verified** | STMicroelectronics FTS2BA61Y 10-finger HID Multitouch (TLMM SPI bit-bang) |
| `WiFi-WCN6855-PDC/` | `s22wlanpdc.sys` + `s22wlanpdc.inf` | `1.0.19.0` | **MIT** | **Verified** | SM8450 PCIe0 DesignWare `iMSI-RX` → `GICD_ISPENDR[2]` MSI Bridge for WCN6855 Wi-Fi 6E |

## Test-Signing Notice
Windows 11 ARM64 requires kernel drivers (`.sys`) to be cataloged (`.cat`) and signed. Private keys and personal `.cer`/`.cat` files are intentionally omitted from Git.

To generate your own test certificate, `.cat` catalogs, and signed binaries locally:
1. Run `scripts/sign_and_stage_drivers.ps1` (or rebuild from source via `drivers/adreno730-wddm/build_adreno730_gpu.ps1` and `scripts/build_and_link_arm64.ps1`).
2. Import your generated `S22TestCert.cer` into `LocalMachine\Root` and `LocalMachine\TrustedPublisher` on the S22.
3. Enable test-signing on the S22 BCD (`bcdedit /set testsigning on`).

## Proprietary Files NOT Included Here
- **Adreno 730 Firmware (`a730_sqe.fw` v1.81 & `gmu_gen70000.bin`)**: Obtain from official upstream `linux-firmware` (`qcom/`) and place in `C:\Windows\System32\drivers\qcom\`.
- **Microsoft VC++ Redistributable ARM64** (`vc_redist.arm64.exe`) & **Khronos Vulkan Loader (`vulkan-1.dll` ARM64)**: Install from official Microsoft / LunarG distributions before using `vulkan_freedreno.dll`.
- **Qualcomm Base SoC & Wi-Fi Drivers** (`Waipio-Base-Drivers`, `qcwlan8280.sys`, `wlanfw20.mbn`, `m320.bin`, `bdwlan.*`): Obtain from official sources; see [`docs/REPLICATION_GUIDE.md`](../docs/REPLICATION_GUIDE.md).
