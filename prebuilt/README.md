# Prebuilt Open-Source ARM64 Drivers (Verified on Hardware)

This folder contains **only 100% open-source, MIT-licensed drivers compiled from this repository's `drivers/` source tree** and verified on physical **Samsung Galaxy S22 5G (`SM-S901E` / `r0q`)** hardware running Windows 11 Pro ARM64 (`26300.9457`):

| Folder | Driver | Version | Status | Hardware |
| :--- | :--- | :--- | :--- | :--- |
| `Touchscreen-FTS2BA61Y/` | `fts2ba61y.sys` + `fts2ba61y.inf` | `1.0.1.0` | **Verified** | STMicroelectronics FTS2BA61Y 10-finger HID Multitouch (TLMM SPI bit-bang) |
| `WiFi-WCN6855-PDC/` | `s22wlanpdc.sys` + `s22wlanpdc.inf` | `1.0.15.0` | **Verified** | SM8450 PCIe0 DesignWare `iMSI-RX` → `GICD_ISPENDR[2]` MSI Bridge for WCN6855 Wi-Fi 6E |

## Test-Signing Notice
Windows 11 ARM64 requires drivers to be cataloged (`.cat`) and signed. Private keys and personal `.cer`/`.cat` files are intentionally omitted from Git.

To generate your own test certificate, `.cat` catalogs, and signed binaries locally:
1. Run `scripts/sign_and_stage_drivers.ps1` (or rebuild from source with `scripts/build_and_link_arm64.ps1`).
2. Import your generated `S22TestCert.cer` into `LocalMachine\Root` and `LocalMachine\TrustedPublisher` on the S22.
3. Enable test-signing on the S22 BCD (`bcdedit /set testsigning on`).

## Where Are the Qualcomm Drivers?
Proprietary Qualcomm binaries (`Waipio-Base-Drivers`, `qcwlan8280.sys`, `wlanfw20.mbn`, `m320.bin`, `bdwlan.*`) are **not redistributed** for licensing reasons. See [`docs/REPLICATION_GUIDE.md`](../docs/REPLICATION_GUIDE.md) for official sources and how to run `scripts/patch_qcwlan8280_wc.ps1` locally on your own `qcwlan8280.sys`.
