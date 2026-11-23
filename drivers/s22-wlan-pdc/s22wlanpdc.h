#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

#define S22_WLAN_POOL_TAG               'NLWS'

// {4A1D8E22-9B0C-43F1-8E6A-227C5B901103}
DEFINE_GUID(GUID_DEVINTERFACE_S22_WLAN_PDC,
    0x4a1d8e22, 0x9b0c, 0x43f1, 0x8e, 0x6a, 0x22, 0x7c, 0x5b, 0x90, 0x11, 0x03);

#define IOCTL_S22_WLAN_GET_STATE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_READ_ACCESS)

//
// SM8450 PCIe0 DesignWare Integrated MSI Receiver (iMSI-RX) @ DBI 0x60000000
// Intercepts WCN6855 MSI TLPs targeting 0x17100040 (APIC 0x0D BaseAddress + 0x40)
// and raises GIC_SPI 141..148 (INTID 0xAD..0xB4 = 173..180)
//
#define S22_PCIE0_DBI_PHYS              0x60000000ULL
#define S22_PCIE0_DBI_SIZE              0x00001000U
#define PCIE_MSI_ADDR_LO                0x0820U
#define PCIE_MSI_ADDR_HI                0x0824U
#define PCIE_MSI_INTR0_ENABLE           0x0828U
#define PCIE_MSI_INTR0_MASK             0x082CU
#define PCIE_MSI_INTR0_STATUS           0x0830U
#define PCIE_MSI_CTRL_STRIDE            0x000CU
#define PCIE_MSI_NUM_BANKS              8U
#define PCIE_MSI_FRAME_BANK             2U           // APIC 0x0D SPI Base=64 (0x40), Count=32 -> INTID 64..95
#define PCIE_MSI_BASE_GSIV              0x00ADU      // GIC_SPI 141 + 32 = 173 (msi0)

//
// SM8450 GICv3 Distributor (GICD) @ 0x17100000 (64 KB)
//
#define S22_GICD_PHYS                   0x17100000ULL
#define S22_GICD_SIZE                   0x00010000U
#define GICD_ISENABLER_OFFSET           0x0100U
#define GICD_ISPENDR_OFFSET             0x0200U
#define S22_MSI_DOORBELL_ADDR_LO        0x17100040U
#define S22_MSI_DOORBELL_ADDR_HI        0x00000000U

typedef struct _S22_WLAN_PDC_STATE {
    ULONG   Version;            // 15 = production lean iMSI-RX -> GICD MSI bridge
    BOOLEAN VirtualMode;
    BOOLEAN BridgeActive;
    BOOLEAN Reserved1;
    BOOLEAN Reserved2;
    ULONG   IrqCountConnected;
    ULONG   DbiRpId;
    ULONG   DbiMsiAddrLo;
    ULONG   GicdIsenabler2;
    LONG    MsiIrqCount;
    ULONG   LastMsiBank;
    ULONG   LastMsiMask;
    ULONG   LastStatus;
} S22_WLAN_PDC_STATE, *PS22_WLAN_PDC_STATE;

typedef struct _S22_MSI_IRQ_CTX {
    ULONG Bank;
} S22_MSI_IRQ_CTX, *PS22_MSI_IRQ_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S22_MSI_IRQ_CTX, S22MsiIrqGetContext)

typedef struct _S22_WLAN_CONTEXT {
    WDFDEVICE           Device;
    volatile UCHAR      *DbiBase;       // 0x60000000 (0x1000: PCIe0 DBI iMSI-RX)
    volatile UCHAR      *GicdBase;      // 0x17100000 (0x10000: GICD)
    WDFINTERRUPT        Interrupts[PCIE_MSI_NUM_BANKS];
    ULONG               InterruptCount;
    ULONG               ConnectedBankMask;
    LONG                BankOrMask[PCIE_MSI_NUM_BANKS];
    BOOLEAN             VirtualMode;
    S22_WLAN_PDC_STATE  State;
} S22_WLAN_CONTEXT, *PS22_WLAN_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S22_WLAN_CONTEXT, S22WlanGetContext)
