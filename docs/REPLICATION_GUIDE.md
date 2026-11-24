# Complete Replication Guide — Windows 11 ARM64 on Samsung Galaxy S22 5G (`SM-S901E` / `r0q`)

This guide reproduces our verified **Windows 11 Pro ARM64 (`26300.9457`)** setup on a **Samsung Galaxy S22 5G (`SM-S901E`, Snapdragon 8 Gen 1 / SM8450, codename `r0q`)** with working **UFS boot, Display, Touchscreen (`fts2ba61y.sys`), USB 3.0 OTG Host, UEFI Charger Mode, 1-click TWRP Reboot, Internal Wi-Fi 6E 5 GHz (`WCN6855` + `s22wlanpdc.sys` + `qcwlan8280_wc.sys`), and Adreno 730 3D Hardware GPU Acceleration (`adreno730_wddm.sys` v1.0.0.23 + Mesa 26.1.0 Turnip Vulkan 1.4.348 `vulkan_freedreno.dll`)**.

---

## 1. Mandatory Backups (DO NOT SKIP!)

Before touching UFS partitions or flashing `boot`, boot into **TWRP** and back up these irreplaceable Samsung partitions to your PC:

| Partition / LUN | Why It Is Critical |
| :--- | :--- |
| **`efs` (`sda1`/`sde*`), `sec_efs`, `modemst1`, `modemst2`, `fsg`, `fsc`** | Unique per-device IMEI, baseband calibration, cellular keys. **Losing these permanently breaks cellular/IMEI.** |
| **`persist`** | Factory sensor/Wi-Fi/BT calibration data. |
| **`recovery`** | Working TWRP recovery — your safety net to recover from any BSOD or boot issue. **Never overwrite `recovery` during Windows experiments.** |
| **`boot` (`sda25`)** | Stock Android boot image (allows returning to Android anytime). |
| **Raw GPT headers (`sda`..`sdf`, first 64 KB + last 64 KB)** | Allows instant GPT restoration if partitioning goes wrong. |

See [`scripts/UFS-PARTITIONS-SAFETY.md`](../scripts/UFS-PARTITIONS-SAFETY.md) for exact `adb` backup commands and partition rules:
- **NEVER** touch partitions `#1..#39` (Android/EFS/modem/TWRP).
- Only use **`sda25` (`boot`)** for UEFI (`Mu-r0q.img`), **`sda40` (FAT32 ESP)**, and **`sda41` (NTFS Windows)**.
- **NEVER** run `fsck.ntfs` or `ntfsfix` on `sda41` — always shut down Windows cleanly and always `sync && umount /tmp/win` in TWRP before `adb reboot`.

---

## 2. Prerequisites & Legal Third-Party Files

You will need:
1. **Unlocked bootloader + TWRP** installed on `SM-S901E` (`r0q`).
2. **WSL2 (Ubuntu)** with `iasl`, `python3`, `git`, `clang`, `lld`, and `chntpw` (`reged`) for building `Mu-Silicium`.
3. **Windows PC** with **Visual Studio Build Tools (MSVC ARM64)** + **Windows WDK / ADK (`10.0.26100.0`)** + **Python 3** for compiling/signing drivers.
4. **Legally obtained proprietary files (NOT included in this repo)**:
   - **Windows 11 ARM64 ISO/WIM** (tested with `26300.9457` pt-BR) via official Microsoft / UUP dump channels.
   - **Samsung `r0q` stock UEFI DXE blobs** (`Binaries/r0q/` extracted from your device's stock `AP` firmware per `Mu-Silicium` docs).
   - **Qualcomm Waipio (`SM8450`) Base SoC Drivers** (`qcabd`, `qcpmic8450`, `qcspmi8450`, `qcgpi8450`, `qcglink8450`, `qcipcrouter8450`, `qcscm`, `qcursext`, etc.) from Qualcomm/WOA driver packs.
   - **Qualcomm Adreno 730 GPU Firmware (`a730_sqe.fw` v1.81 & `gmu_gen70000.bin`)**: Obtain from official upstream [`linux-firmware`](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/qcom) (`LICENSE.qcom`).
   - **Microsoft Visual C++ Redistributable ARM64 (`vc_redist.arm64.exe`)** & **Khronos Vulkan Runtime (`vulkan-1.dll` ARM64)** for Mesa Turnip Vulkan 1.4.348.
   - **Qualcomm WCN6855 / QCA6490 Wi-Fi 6E Driver & Firmware** (`qcwlan8280.sys` `v1.0.3451.5100`, `qcwlan8280.inf`, `wlanfw20.mbn`, `m320.bin`, `bdwlan.*`, `regdb.bin`) from official Lenovo ThinkPad X13s / Surface Pro 9 5G (`SC8280XP`) driver packages.

---

## 3. Build Custom `Mu-Silicium` UEFI (`Mu-r0q.img`)

```bash
git clone --recursive https://github.com/Project-Silicium/Mu-Silicium.git
cd Mu-Silicium
git checkout cc64daf0
git submodule update --init --recursive

# 1. Apply our working Mu-Silicium UEFI patch
git apply /path/to/repo/patches/mu-silicium/mu-silicium-r0q.patch

# 2. Copy new files added by our port
cp /path/to/repo/patches/mu-silicium/S22Pcie0PhyTbl.h Silicon/Qualcomm/QcomPkg/Library/PlatformBootManagerLib/
cp /path/to/repo/patches/mu-silicium/EFII2C.h Silicon/Qualcomm/QcomPkg/Include/Protocol/
mkdir -p Silicon/Qualcomm/QcomPkg/Applications/ChargerMode Silicon/Qualcomm/QcomPkg/Applications/RebootRecovery
cp /path/to/repo/patches/mu-silicium/ChargerMode.{c,inf} Silicon/Qualcomm/QcomPkg/Applications/ChargerMode/
cp /path/to/repo/patches/mu-silicium/RebootRecovery.{c,inf} Silicon/Qualcomm/QcomPkg/Applications/RebootRecovery/

# 3. Apply working Silicium-ACPI overrides (NO IORT.aml — uses APIC 0x0D GIC MSI Frame!)
cp /path/to/repo/acpi/r0q/{AcpiTables.inf,SSDT.asl,SSDT.aml,APIC.aml,MCFG.aml} Silicium-ACPI/Platforms/Samsung/r0q/
cp /path/to/repo/acpi/waipio/{DSDT.asl,DSDT.aml} Silicium-ACPI/Silicon/Qualcomm/Waipio/

# 4. Place your extracted Samsung r0q XBL DXE blobs in Binaries/r0q/ and build
./build_uefi.sh -d r0q -r RELEASE
```

Flash the resulting `Mu-r0q.img` to `sda25` (`boot`) in TWRP and **always verify SHA-256**:
```bash
adb push Mu-r0q.img /tmp/mu.img
adb shell 'dd if=/tmp/mu.img of=/dev/block/sda25 bs=1048576 conv=fsync; sync; SZ=$(stat -c%s /tmp/mu.img); sha256sum /tmp/mu.img; head -c "$SZ" /dev/block/sda25 | sha256sum; rm -f /tmp/mu.img'
```

---

## 4. Partitioning, Windows 11 ARM64 Install & BCD

1. Create `sda40` (FAT32 ESP, e.g. `256–512 MB`) and `sda41` (NTFS 4K, Windows) at the end of `sda` (shrinking `userdata` only — **never** move `#1..#39`).
2. Ensure every partition on `sda` has a **unique `UniquePartitionGUID`** (Samsung's stock GPT may contain duplicate GUIDs that cause Windows `0x7B INACCESSIBLE_BOOT_DEVICE` `0xC0000034`).
3. Apply your Windows 11 ARM64 WIM to `sda41`, create EFI boot files on `sda40` (`bcdboot`), and enable test-signing on the BCD:
   ```cmd
   bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} testsigning on
   bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} nointegritychecks on
   ```
4. Place your `S22TestCert.cer` into `LocalMachine\Root` and `LocalMachine\TrustedPublisher` in the Windows `SOFTWARE` hive (or import it on first boot).

---

## 5. Build, Sign & Install Drivers (Touch + Wi-Fi 6E)

### 5.1 Build & Sign Our Open-Source Drivers (`fts2ba61y.sys` & `s22wlanpdc.sys`)
You can either compile from source (`drivers/`) or start from `prebuilt/`:
```powershell
# Rebuilds from source, generates .cat catalogs, creates your local test cert, and signs all drivers:
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_link_arm64.ps1
```
Install into your Windows image (offline via `DISM /Add-Driver` or live in Windows via `pnputil /add-driver ... /install`):
- `Touchscreen-FTS2BA61Y\fts2ba61y.inf` (`ACPI\STMT0001` — Touchscreen)
- `WiFi-WCN6855-PDC\s22wlanpdc.inf` (`ACPI\S22W0001` — DesignWare `iMSI-RX` → `GICD_ISPENDR[2]` MSI bridge, v1.0.15.0)
- Your locally obtained `Waipio-Base-Drivers` and `WiFi-WCN6855-Qualcomm` (`qcwlan8280.inf`)

### 5.2 Patch `qcwlan8280.sys` for SM8450 Non-Coherent PCIe0 (CRITICAL FOR WI-FI 6E!)
Qualcomm's `qcwlan8280.sys` (`v1.0.3451.5100`, originally built for IO-coherent `SC8280XP`) hardcodes `dmaV3AllocApi = 1` and allocates MHI/BHI/CE/DP DMA buffers as `MmCached`. Because **SM8450 PCIe0 is physically non-coherent** (does not snoop CPU caches), `MHIProcessEventRing` reads stale CPU cache lines and times out before entering Mission Mode.

Place your own copy of `qcwlan8280.sys` (`v1.0.3451.5100`) in `S901E Drivers\WiFi-WCN6855-Qualcomm\qcwlan8280.sys` and run:
```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\patch_qcwlan8280_wc.ps1
```
This applies a 3-word patch switching DMA allocations to `MmAllocateContiguousMemorySpecifyCache(..., MmWriteCombined)` (`Normal Non-Cacheable` on ARM64 — safe for unaligned accesses and 100% cache-coherent with SM8450 PCIe0!) and test-signs `qcwlan8280_wc.sys` with your local cert.

On the S22 (Administrator PowerShell):
```powershell
Copy-Item .\qcwlan8280_wc.sys C:\Windows\System32\drivers\qcwlan8280_wc.sys -Force
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\qcwlan" -Name "ImagePath" -Value "\SystemRoot\System32\drivers\qcwlan8280_wc.sys"
```
Restart the `Qualcomm WCN685x` device (or reboot) — **Wi-Fi 6E (5 GHz)** will enumerate `Wi-Fi` & `Wi-Fi 3` (`CM_PROB_NONE`), scan networks, and connect!

---

## 6. Adreno 730 3D GPU Acceleration (WDDM 2.0 + Mesa 26.1.0 Turnip Vulkan 1.4.348)

### 6.1 Obtain Required Official Runtimes & Firmware (NOT Included in Git)
1. Download `qcom/a730_sqe.fw` (v1.81, SHA-256 `4F8B288F340FF5B4881BED8DFEA90BB4A1B967556CA2DF6D07F4ED4E4A28D071`) and `qcom/gmu_gen70000.bin` from official upstream [`linux-firmware`](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/qcom) and copy them to `C:\Windows\System32\drivers\qcom\` on the S22.
2. Install **Microsoft Visual C++ Redistributable for ARM64** (`vc_redist.arm64.exe`) and the **Khronos Vulkan Runtime (`vulkan-1.dll` ARM64)** on the S22.

### 6.2 Build from Source (Optional — or use `prebuilt/Adreno730-GPU/`)
- **WDDM 2.0 KMD + D3D12 UMD (Windows PC with MSVC ARM64 + WDK 10.0.26100.0)**:
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\drivers\adreno730-wddm\build_adreno730_gpu.ps1
  ```
- **Mesa 26.1.0 Turnip Vulkan 1.4.348 `vulkan_freedreno.dll` (WSL2 Ubuntu + `clang-cl`/`lld`/`meson`/`ninja`)**:
  ```bash
  ./patches/mesa-turnip-woa/build_turnip_arm64.sh
  ```
- Generate `adreno730_wddm.cat` with `inf2cat /driver:<dir> /os:10_VB_ARM64` and sign `adreno730_wddm.sys`, `adreno730_d3d12.dll`, `adreno730_wddm.cat`, and `vulkan_freedreno.dll` with your own local test certificate.

### 6.3 Install & Activate on the S22 (Auto-Start with Anti-Bootloop Watchdog!)
On the S22 (Administrator PowerShell):
```powershell
# 1. Create the root-enumerated WDDM adapter device once (using WDK devcon ARM64)
devcon install .\prebuilt\Adreno730-GPU\adreno730_wddm.inf Root\Adreno730Wddm

# 2. Install WDDM 2.0 (v1.0.0.24, StartGpu=1 + BootInProgress watchdog),
#    Mesa Turnip Vulkan 1.4.348, Mesa Zink OpenGL 4.6, MSYS2 ARM64 DXVK (DX8/9/10/11),
#    and configure Microsoft Edge ANGLE-Vulkan acceleration:
powershell -ExecutionPolicy Bypass -File .\scripts\install_adreno730_gpu.ps1 -DriverDir .\prebuilt\Adreno730-GPU -InstallDXVK -ConfigureEdge
```
- Standard Vulkan apps using `vulkan-1.dll` automatically use **`Turnip Adreno (TM) 730 (Vulkan 1.4.348)`**.
- Drop `C:\Program Files\S22-Adreno730-GPU\OpenGL\{opengl32.dll, libgallium_wgl.dll}` next to any ARM64 OpenGL app for **OpenGL 4.6** (`Mesa Zink`).
- Drop `C:\Program Files\S22-Adreno730-GPU\DXVK\{d3d8,d3d9,d3d10core,d3d11,dxgi}.dll` next to any ARM64 DirectX 8/9/10/11 app for **DXVK hardware acceleration**.
- Microsoft Edge shortcuts launch with **ANGLE-Vulkan WebGL2** on Adreno 730.

---

## 7. Convenience Tools
- **1-Click TWRP Reboot**: Copy [`scripts/Reiniciar_TWRP.cmd`](../scripts/Reiniciar_TWRP.cmd) to the Windows Desktop and run as Administrator.
- **TWRP Fast-Charge**: Push [`scripts/fc.sh`](../scripts/fc.sh) to `/sdcard/fc.sh` in TWRP and run `sh /sdcard/fc.sh` (clears `VOTER_USB_100MA` via `usb_conf_test=2`, sets `siop_level=100`, turns off cores 1–7 + backlight, sets `powersave` on core 0).
- **UEFI 2.5A Charger Mode**: Select `Charger Mode` in the `Mu-Silicium` Boot Manager (`Vol+`/`Vol−`, both together = Enter).
