#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>

//
// Pool tag
//
#define FTS_POOL_TAG                        'STFS'

//
// Virtual HID device IDs (STMicroelectronics VID = 0x0483)
//
#define FTS_HID_VID                         0x0483
#define FTS_HID_PID                         0x2B61
#define FTS_HID_VERSION                     0x0100

//
// HID Report IDs (Windows Multitouch Digitizer standard)
//
#define REPORTID_TOUCH                      0x01
#define REPORTID_MAX_COUNT                  0x02
#define REPORTID_FEATURE_THQA               0x03

#define FTS_MAX_CONTACTS                    10
#define FTS_LOGICAL_MAX_X                   4095
#define FTS_LOGICAL_MAX_Y                   4095

//
// SM8450 TLMM GPIO register offsets (per 0x1000-byte pin page)
//   GPIO 28: MISO  (offset 0x0000 in block 0)
//   GPIO 29: MOSI  (offset 0x1000 in block 0)
//   GPIO 30: SCK   (offset 0x2000 in block 0)
//   GPIO 31: CS    (offset 0x3000 in block 0)
//   GPIO 46: IRQ   (offset 0x0000 in block 1)
//
#define TLMM_GPIO_STRIDE                    0x1000
#define TLMM_GPIO_CFG_OFFSET                0x0000
#define TLMM_GPIO_IN_OUT_OFFSET             0x0004
#define TLMM_SPI_PHYS_BASE                  0x0F11C000ULL
#define TLMM_IRQ_PHYS_BASE                  0x0F12E000ULL

#define FTS_STATUS_TIP_SWITCH               0x01
#define FTS_STATUS_IN_RANGE                 0x02
#define FTS_STATUS_CONFIDENCE               0x04

#define TLMM_PIN_IDX_MISO                   0
#define TLMM_PIN_IDX_MOSI                   1
#define TLMM_PIN_IDX_SCK                    2
#define TLMM_PIN_IDX_CS                     3

#define TLMM_CFG_PULL_NONE                  (0U << 0)
#define TLMM_CFG_FUNC_GPIO                  (0U << 2)
#define TLMM_CFG_DRV_2MA                    (0U << 6)
#define TLMM_CFG_DRV_6MA                    (2U << 6)
#define TLMM_CFG_OE                         (1U << 9)

#define TLMM_IO_IN_BIT                      (1U << 0)
#define TLMM_IO_OUT_BIT                     (1U << 1)

//
// STMicroelectronics FTS2BA61Y commands & constants
// (Ported from Linux mainline driver fts2ba61y.c for Samsung Galaxy S22 r0q)
//
#define FTS2BA61Y_CMD_SENSE_ON              0x10
#define FTS2BA61Y_CMD_SENSE_OFF             0x11
#define FTS2BA61Y_CMD_READ_PANEL_INFO       0x23
#define FTS2BA61Y_CMD_READ_FW_VER           0x24
#define FTS2BA61Y_CMD_TOUCHTYPE             0x30
#define FTS2BA61Y_CMD_CLEAR_EVENTS          0x62
#define FTS2BA61Y_CMD_READ_EVENT            0x87
#define FTS2BA61Y_CMD_CUSTOM_W              0xC0
#define FTS2BA61Y_CMD_CUSTOM_R              0xD1
#define FTS2BA61Y_CMD_REG_W                 0xFA
#define FTS2BA61Y_CMD_REG_R                 0xFB

#define FTS2BA61Y_MASK_TOUCH                (1U << 0)
#define FTS2BA61Y_MASK_PALM                 (1U << 5)
#define FTS2BA61Y_MASK_WET                  (1U << 6)
#define FTS2BA61Y_TOUCHTYPE_DEFAULT         (FTS2BA61Y_MASK_TOUCH | FTS2BA61Y_MASK_PALM | FTS2BA61Y_MASK_WET)

#define FTS2BA61Y_EVENT_STATUSTYPE_INFO     0x02
#define FTS2BA61Y_INFO_READY_STATUS         0x00
#define FTS2BA61Y_COORDINATE_EVENT          0x00

#define FTS2BA61Y_TOUCHTYPE_NORMAL          0x00
#define FTS2BA61Y_TOUCHTYPE_GLOVE           0x03
#define FTS2BA61Y_TOUCHTYPE_PALM            0x05
#define FTS2BA61Y_TOUCHTYPE_WET             0x06

#define FTS2BA61Y_ACTION_NONE               0x00
#define FTS2BA61Y_ACTION_PRESS              0x01
#define FTS2BA61Y_ACTION_MOVE               0x02
#define FTS2BA61Y_ACTION_RELEASE            0x03

#define FTS2BA61Y_EVENT_BUFF_SIZE           16
#define FTS2BA61Y_PANEL_INFO_SIZE           11
#define FTS2BA61Y_RESET_CMD_SIZE            5
#define FTS2BA61Y_EVENT_COUNT               31
#define FTS2BA61Y_MAX_XFER_SIZE             520

//
// Packed HID Reports
//
#include <pshpack1.h>

typedef struct _FTS_HID_CONTACT {
    UCHAR   Status;      // Bit 0: TipSwitch, Bit 1: InRange, Bit 2: Confidence, Bits 3..7: Padding
    UCHAR   ContactId;
    UCHAR   Pressure;    // 0 .. 63 (Tip Pressure)
    USHORT  X;           // 0 .. 4095
    USHORT  Y;           // 0 .. 4095
} FTS_HID_CONTACT, *PFTS_HID_CONTACT;

typedef struct _FTS_HID_TOUCH_REPORT {
    UCHAR           ReportId;                       // REPORTID_TOUCH (0x01)
    FTS_HID_CONTACT Contacts[FTS_MAX_CONTACTS];
    UCHAR           ContactCount;
    USHORT          ScanTime;                       // 100us units
} FTS_HID_TOUCH_REPORT, *PFTS_HID_TOUCH_REPORT;

typedef struct _FTS_HID_MAX_COUNT_REPORT {
    UCHAR   ReportId;                               // REPORTID_MAX_COUNT (0x02)
    UCHAR   MaxCount;                               // FTS_MAX_CONTACTS (10)
} FTS_HID_MAX_COUNT_REPORT, *PFTS_HID_MAX_COUNT_REPORT;

typedef struct _FTS_HID_THQA_REPORT {
    UCHAR   ReportId;                               // REPORTID_FEATURE_THQA (0x03)
    UCHAR   Blob[256];
} FTS_HID_THQA_REPORT, *PFTS_HID_THQA_REPORT;

#include <poppack.h>

//
// Internal per-finger tracking state
//
typedef struct _FTS_SLOT_STATE {
    BOOLEAN Active;
    BOOLEAN ReleasePending;
    BOOLEAN Confident;
    USHORT  X;
    USHORT  Y;
    UCHAR   Pressure;
} FTS_SLOT_STATE;

//
// WDF Device Context
//
typedef struct _FTS_DEVICE_CONTEXT {
    WDFDEVICE       Device;
    WDFQUEUE        IoQueue;
    WDFQUEUE        ReadReportQueue;
    WDFTIMER        PollTimer;
    WDFSPINLOCK     StateLock;
    WDFWAITLOCK     SpiLock;

    // Mapped SM8450 TLMM MMIO regions (NULL in VirtualMode)
    volatile UCHAR  *SpiGpioBase;     // 4 pages: GPIO 28, 29, 30, 31
    SIZE_T          SpiGpioLen;
    volatile UCHAR  *IrqGpioBase;     // 1 page:  GPIO 46
    SIZE_T          IrqGpioLen;

    BOOLEAN         VirtualMode;      // TRUE only on ROOT\STMT0001 (QEMU VM testing)
    BOOLEAN         HardwareReady;
    volatile LONG   SensingEnabled;   // 0/1, modified atomically to prevent D0Exit timer race

    USHORT          MaxX;
    USHORT          MaxY;
    UCHAR           TxCount;
    USHORT          ScanTime;

    FTS_SLOT_STATE  Slots[FTS_MAX_CONTACTS];
    BOOLEAN         ReportDirty;

    // Pre-allocated transfer/event buffers (protected by SpiLock / passive PollTimer)
    UCHAR           SpiTxBuf[FTS2BA61Y_MAX_XFER_SIZE];
    UCHAR           SpiRxBuf[FTS2BA61Y_MAX_XFER_SIZE];
    UCHAR           EventBuf[FTS2BA61Y_EVENT_COUNT * FTS2BA61Y_EVENT_BUFF_SIZE];
} FTS_DEVICE_CONTEXT, *PFTS_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FTS_DEVICE_CONTEXT, FtsGetDeviceContext)

//
// spi_tlmm.c
//
VOID
TlmmSpiInitPins(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    );

BOOLEAN
TlmmIsIrqAsserted(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    );

NTSTATUS
TlmmSpiTransferLocked(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_reads_bytes_(Length) const UCHAR *TxBuf,
    _Out_writes_bytes_opt_(Length) UCHAR *RxBuf,
    _In_ ULONG Length
    );

NTSTATUS
TlmmSpiTransfer(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_reads_bytes_(Length) const UCHAR *TxBuf,
    _Out_writes_bytes_opt_(Length) UCHAR *RxBuf,
    _In_ ULONG Length
    );

//
// fts2ba61y_proto.c
//
NTSTATUS
FtsHardwareInit(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    );

NTSTATUS
FtsSetSensing(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Enable
    );

NTSTATUS
FtsPollEvents(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _Out_ PBOOLEAN HasUpdate
    );

VOID
FtsBuildHidReport(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _Out_ PFTS_HID_TOUCH_REPORT Report
    );
