#!/usr/bin/env bash
set -euo pipefail

REPO="/mnt/c/Users/NetoBR/Desktop/S22 Windows/Windows S22/Windows 11 S22 Drivers & Patchs"
ADK="/mnt/c/ADK"
VER="10.0.26100.0"
KMDF_VER="1.33"

INC_KM="${ADK}/Include/${VER}/km"
INC_SHARED="${ADK}/Include/${VER}/shared"
INC_CRT="${ADK}/Include/${VER}/km/crt"
INC_WDF="${ADK}/Include/wdf/kmdf/${KMDF_VER}"

LIB_KM="${ADK}/Lib/${VER}/km/arm64"
LIB_WDF="${ADK}/Lib/wdf/kmdf/arm64/${KMDF_VER}"

CFLAGS=(
  --target=aarch64-pc-windows-msvc
  -std=c11
  -O2
  -fms-extensions
  -fms-compatibility
  -fms-compatibility-version=19.40
  -fno-stack-protector
  -mno-stack-arg-probe
  -ffunction-sections
  -fdata-sections
  -D_ARM64_
  -DARM64
  -D_WIN64
  -D_KERNEL_MODE
  -DNTDDI_VERSION=0x0A00000C
  -D_WIN32_WINNT=0x0A00
  -DWINVER=0x0A00
  -DWINNT=1
  -DKMDF_VERSION_MAJOR=1
  -DKMDF_VERSION_MINOR=33
  -DPOOL_NX_OPTIN=1
  -DPOOL_ZERO_DOWN_LEVEL_SUPPORT=1
  -Wno-pragma-pack
  -Wno-ignored-attributes
  -Wno-ignored-pragmas
  -Wno-missing-declarations
  -Wno-unknown-pragmas
  -Wno-microsoft-anon-tag
  -Wno-microsoft-enum-forward-reference
  -Wno-nonportable-include-path
  -Wno-incompatible-pointer-types-discards-qualifiers
  -isystem "${INC_KM}"
  -isystem "${INC_SHARED}"
  -isystem "${INC_CRT}"
  -isystem "${INC_WDF}"
)

LDFLAGS_COMMON=(
  /MACHINE:ARM64
  /SUBSYSTEM:NATIVE,10.00
  /DRIVER
  /ENTRY:FxDriverEntry
  /INTEGRITYCHECK
  /NODEFAULTLIB
  /MERGE:_TEXT=.text
  /MERGE:_PAGE=PAGE
  /OPT:REF
  /OPT:ICF
  /MANIFEST:NO
  /LIBPATH:"${LIB_KM}"
  /LIBPATH:"${LIB_WDF}"
  WdfDriverEntry.lib
  WdfLdr.lib
  ntoskrnl.lib
  hal.lib
  wmilib.lib
  BufferOverflowFastFailK.lib
)

fix_coff_data_rw() {
  python3 - "$1" << 'PY'
import struct, sys
p = sys.argv[1]
b = bytearray(open(p, "rb").read())
num_sec = struct.unpack_from("<H", b, 2)[0]
opt_sz  = struct.unpack_from("<H", b, 16)[0]
off = 20 + opt_sz
for i in range(num_sec):
    s = off + i * 40
    if b[s:s+8] == b".data\x00\x00\x00":
        ch = struct.unpack_from("<I", b, s + 36)[0]
        if (ch & 0x80000000) == 0:
            struct.pack_into("<I", b, s + 36, ch | 0x80000000)
open(p, "wb").write(b)
PY
}

OBJS_ONLY="${1:-}"

# 1. Build fts2ba61y.sys (STM FTS2BA61Y Touchscreen HID Minidriver)
echo "=== [1/2] Compiling fts2ba61y (ARM64) ==="
TOUCH_DIR="${REPO}/drivers/fts2ba61y-touch"
TOUCH_OUT="${TOUCH_DIR}/ARM64/Release/fts2ba61y"
mkdir -p "${TOUCH_OUT}"

for src in driver.c spi_tlmm.c fts2ba61y_proto.c; do
  echo "  CC ${src}"
  clang "${CFLAGS[@]}" -I "${TOUCH_DIR}" -c "${TOUCH_DIR}/${src}" -o "${TOUCH_OUT}/${src%.c}.obj"
  fix_coff_data_rw "${TOUCH_OUT}/${src%.c}.obj"
done

if [ "${OBJS_ONLY}" != "--objs-only" ]; then
  echo "  LD fts2ba61y.sys"
  lld-link "${LDFLAGS_COMMON[@]}" \
    /OUT:"${TOUCH_OUT}/fts2ba61y.sys" \
    "${TOUCH_OUT}/driver.obj" \
    "${TOUCH_OUT}/spi_tlmm.obj" \
    "${TOUCH_OUT}/fts2ba61y_proto.obj"
  cp -f "${TOUCH_DIR}/fts2ba61y.inf" "${TOUCH_OUT}/fts2ba61y.inf"
  rm -f "${TOUCH_OUT}"/*.obj
fi

# 2. Build max77705batt.sys (Maxim MAX77705 Battery Miniclass Driver)
echo "=== [2/4] Compiling max77705batt (ARM64) ==="
BATT_DIR="${REPO}/drivers/max77705-battery"
BATT_OUT="${BATT_DIR}/ARM64/Release/max77705batt"
mkdir -p "${BATT_OUT}"

for src in driver.c i2c_tlmm.c; do
  echo "  CC ${src}"
  clang "${CFLAGS[@]}" -I "${BATT_DIR}" -c "${BATT_DIR}/${src}" -o "${BATT_OUT}/${src%.c}.obj"
  fix_coff_data_rw "${BATT_OUT}/${src%.c}.obj"
done

if [ "${OBJS_ONLY}" != "--objs-only" ]; then
  echo "  LD max77705batt.sys"
  lld-link "${LDFLAGS_COMMON[@]}" \
    battc.lib \
    /OUT:"${BATT_OUT}/max77705batt.sys" \
    "${BATT_OUT}/driver.obj" \
    "${BATT_OUT}/i2c_tlmm.obj"
  cp -f "${BATT_DIR}/max77705batt.inf" "${BATT_OUT}/max77705batt.inf"
  rm -f "${BATT_OUT}"/*.obj
fi

# 3. Build max77705usbc.sys (Maxim MAX77705 USB-C / USB 3.0 / Dual-Role / PD / DP Alt-Mode)
echo "=== [3/4] Compiling max77705usbc (ARM64) ==="
USBC_DIR="${REPO}/drivers/max77705-usbc"
USBC_OUT="${USBC_DIR}/ARM64/Release/max77705usbc"
mkdir -p "${USBC_OUT}"

for src in driver.c usbc_hw.c; do
  echo "  CC ${src}"
  clang "${CFLAGS[@]}" -I "${USBC_DIR}" -c "${USBC_DIR}/${src}" -o "${USBC_OUT}/${src%.c}.obj"
  fix_coff_data_rw "${USBC_OUT}/${src%.c}.obj"
done

if [ "${OBJS_ONLY}" != "--objs-only" ]; then
  echo "  LD max77705usbc.sys"
  lld-link "${LDFLAGS_COMMON[@]}" \
    /OUT:"${USBC_OUT}/max77705usbc.sys" \
    "${USBC_OUT}/driver.obj" \
    "${USBC_OUT}/usbc_hw.obj"
  cp -f "${USBC_DIR}/max77705usbc.inf" "${USBC_OUT}/max77705usbc.inf"
  rm -f "${USBC_OUT}"/*.obj
fi

# 4. Build s22wlanpdc.sys (Galaxy S22 WCN6855 2.4GHz PA AOSS QMP s3e.e Enabler)
echo "=== [4/4] Compiling s22wlanpdc (ARM64) ==="
WLAN_DIR="${REPO}/drivers/s22-wlan-pdc"
WLAN_OUT="${WLAN_DIR}/ARM64/Release/s22wlanpdc"
mkdir -p "${WLAN_OUT}"

for src in driver.c; do
  echo "  CC ${src}"
  clang "${CFLAGS[@]}" -I "${WLAN_DIR}" -c "${WLAN_DIR}/${src}" -o "${WLAN_OUT}/${src%.c}.obj"
  fix_coff_data_rw "${WLAN_OUT}/${src%.c}.obj"
done

if [ "${OBJS_ONLY}" != "--objs-only" ]; then
  echo "  LD s22wlanpdc.sys"
  lld-link "${LDFLAGS_COMMON[@]}" \
    /OUT:"${WLAN_OUT}/s22wlanpdc.sys" \
    "${WLAN_OUT}/driver.obj"
  cp -f "${WLAN_DIR}/s22wlanpdc.inf" "${WLAN_OUT}/s22wlanpdc.inf"
  rm -f "${WLAN_OUT}"/*.obj
  echo "=== BUILD COMPLETE ==="
  ls -lh "${TOUCH_OUT}/fts2ba61y.sys" "${BATT_OUT}/max77705batt.sys" "${USBC_OUT}/max77705usbc.sys" "${WLAN_OUT}/s22wlanpdc.sys"
fi
