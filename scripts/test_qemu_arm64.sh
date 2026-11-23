#!/usr/bin/env bash
set -euo pipefail

# VM QEMU Windows 11 ARM64 no WSL para testar carregamento de drivers (ROOT\STMT0001 e ROOT\MAXM7705)
VM_DIR="${HOME}/s22-win11-vm"
ISO_PATH="/mnt/c/Users/NetoBR/Desktop/S22 Windows/Windows S22/Windows11_Client_arm64_pt-br_26300_9457.iso"
DRIVERS_DIR="/mnt/c/Users/NetoBR/Desktop/S22 Windows/Windows S22/Windows 11 S22 Drivers & Patchs/S901E Drivers"
DISK_IMG="${VM_DIR}/win11-arm64.qcow2"
VARS_IMG="${VM_DIR}/AAVMF_VARS.fd"
CODE_IMG="/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd"

mkdir -p "${VM_DIR}"

if [ ! -f "${DISK_IMG}" ]; then
  echo "[*] Criando disco virtual qcow2 de 40G em ${DISK_IMG}..."
  qemu-img create -f qcow2 "${DISK_IMG}" 40G
fi

if [ ! -f "${VARS_IMG}" ]; then
  echo "[*] Copiando variaveis UEFI AAVMF..."
  cp /usr/share/AAVMF/AAVMF_VARS.fd "${VARS_IMG}"
fi

echo "[*] Iniciando QEMU ARM64 (VNC em localhost:5900)..."
echo "    Dentro do Windows ARM64 na VM:"
echo "    1) bcdedit /set testsigning on && shutdown /r /t 0"
echo "    2) devgen /add /bus ROOT /hardwareid ROOT\\STMT0001  (Touchscreen Virtual)"
echo "    3) devgen /add /bus ROOT /hardwareid ROOT\\MAXM7705  (Bateria Virtual 78% / 12.5W)"
echo "    4) pnputil /add-driver E:\\*.inf /subdirs /install"

exec qemu-system-aarch64 \
  -machine virt,gic-version=3 \
  -cpu cortex-a76 \
  -smp 4 \
  -m 4096 \
  -drive if=pflash,format=raw,readonly=on,file="${CODE_IMG}" \
  -drive if=pflash,format=raw,file="${VARS_IMG}" \
  -device virtio-gpu-pci \
  -device qemu-xhci \
  -device usb-kbd \
  -device usb-tablet \
  -drive if=none,id=hd0,file="${DISK_IMG}",format=qcow2 \
  -device virtio-blk-pci,drive=hd0 \
  -drive if=none,id=cd0,media=cdrom,readonly=on,file="${ISO_PATH}" \
  -device usb-storage,drive=cd0,removable=on \
  -drive if=none,id=drv,file=fat:ro:"${DRIVERS_DIR}" \
  -device usb-storage,drive=drv,removable=on \
  -vnc :0
