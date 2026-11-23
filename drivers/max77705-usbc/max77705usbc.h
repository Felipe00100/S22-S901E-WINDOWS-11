#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

#define S22_USBC_POOL_TAG                   'CBSU'

// {9B4E2A10-7C3F-4D81-A5E2-1190F8C4D201}
DEFINE_GUID(GUID_DEVINTERFACE_S22_USBC,
    0x9b4e2a10, 0x7c3f, 0x4d81, 0xa5, 0xe2, 0x11, 0x90, 0xf8, 0xc4, 0xd2, 0x01);

#define IOCTL_S22_USBC_GET_STATE    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_S22_USBC_SET_ROLE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef enum _S22_USB_ROLE {
    S22UsbRoleNone = 0,
    S22UsbRoleDevice = 1,   // Peripheral / UFP (MTP + USB NCM/SSH)
    S22UsbRoleHost = 2      // OTG Host / DFP (USB drives, hubs, keyboards)
} S22_USB_ROLE;

typedef enum _S22_CC_ORIENTATION {
    S22CcNone = 0,
    S22Cc1Normal = 1,
    S22Cc2Flipped = 2
} S22_CC_ORIENTATION;

typedef struct _S22_USBC_STATE {
    ULONG   Version;            // 1
    BOOLEAN VirtualMode;
    BOOLEAN CableAttached;
    BOOLEAN VbusPresent;
    BOOLEAN OtgBoostActive;
    BOOLEAN SuperSpeedActive;   // TRUE = USB 3.0 5Gbps, FALSE = USB 2.0 fallback
    BOOLEAN PdContractActive;
    BOOLEAN DpAltModeActive;
    BOOLEAN DpHpdHigh;
    ULONG   UsbRole;            // S22_USB_ROLE
    ULONG   Orientation;        // S22_CC_ORIENTATION
    ULONG   PdVoltageMv;        // e.g. 5000 or 9000
    ULONG   PdCurrentMa;        // e.g. 500, 1500, 2000, 3000
    ULONG   ChgIlimMa;          // Applied CHGIN_ILIM
    UCHAR   DpPinAssignment;    // 0, 'C', 'D', 'E'
    UCHAR   CcStatus0;
    UCHAR   PdStatus0;
    UCHAR   PdStatus1;
} S22_USBC_STATE, *PS22_USBC_STATE;

//
// SM8450 TLMM I2C5 GPIO pages (0x0F1CE000, len 0x2000)
//
#define TLMM_GPIO_STRIDE                    0x1000
#define TLMM_GPIO_CFG_OFFSET                0x0000
#define TLMM_GPIO_IN_OUT_OFFSET             0x0004
#define TLMM_PIN_IDX_SDA                    0
#define TLMM_PIN_IDX_SCL                    1
#define TLMM_CFG_PULL_NONE                  (0U << 0)
#define TLMM_CFG_FUNC_GPIO                  (0U << 2)
#define TLMM_CFG_DRV_2MA                    (0U << 6)
#define TLMM_CFG_OE                         (1U << 9)
#define TLMM_IO_IN_BIT                      (1U << 0)

//
// SM8450 DWC3 QSCRATCH registers (0x0A6F8800, len 0x400)
//
#define QSCRATCH_GENERAL_CFG                0x0008
#define QSCRATCH_HS_PHY_CTRL                0x0010
#define QSCRATCH_UTMI_OTG_VBUS_VALID        (1U << 20)
#define QSCRATCH_SW_SESSVLD_SEL             (1U << 28)
#define QSCRATCH_SS_PHY_CTRL                0x0030
#define QSCRATCH_LANE0_PWR_PRESENT          (1U << 24)

//
// SM8450 QMP USB3/DP Combo PHY v3 registers (0x088E8000, len 0x3000)
//
#define QMP_COM_BASE                        0x0000
#define QMP_DP_COM_SW_RESET                 0x0004
#define QMP_DP_COM_TYPEC_CTRL               0x0010
#define QMP_SW_PORTSELECT_VAL               (1U << 0)  // 0 = CC1, 1 = CC2
#define QMP_SW_PORTSELECT_MUX               (1U << 1)
#define QMP_RXA_BASE                        0x1400
#define QMP_RXB_BASE                        0x1800
#define QMP_USB3_PCS_BASE                   0x1C00

// SM8450 / S22 4nm QMP USB3 tuning offsets (from s22-linux local-sm8450-qmp-combo-usb3-tuning.patch)
#define QSERDES_V5_RX_RX_MODE_01_LOW        0x0184
#define QSERDES_V5_RX_RX_MODE_01_HIGH       0x0188
#define QPHY_V4_PCS_LOCK_DETECT_CONFIG1     0x00C4
#define QPHY_V4_PCS_LOCK_DETECT_CONFIG2     0x00C8
#define QPHY_V4_PCS_LOCK_DETECT_CONFIG3     0x00CC
#define QPHY_V4_PCS_LOCK_DETECT_CONFIG6     0x00D8

//
// MAX77705 I2C 7-bit addresses & registers (from s22-linux max77705-typec & dp-altmode patches)
//
#define MAX77705_USBC_ADDR                  0x25
#define MAX77705_CHG_ADDR                   0x69

// MAX77705 USBC / CCIC registers (0x25)
#define MAX77705_UIC_HW_REV                 0x00
#define MAX77705_UIC_FW_REV                 0x01
#define MAX77705_UIC_INT                    0x02
#define MAX77705_CC_INT                     0x03
#define MAX77705_PD_INT                     0x04
#define MAX77705_VDM_INT                    0x05
#define MAX77705_USBC_STATUS2               0x07
#define MAX77705_CC_STATUS0                 0x0A
#define MAX77705_PD_STATUS0                 0x0C
#define MAX77705_PD_STATUS1                 0x0D
#define MAX77705_UIC_INT_M                  0x0E
#define MAX77705_CC_INT_M                   0x0F
#define MAX77705_PD_INT_M                   0x10
#define MAX77705_VDM_INT_M                  0x11
#define MAX77705_OPCODE_WRITE               0x21
#define MAX77705_OPCODE_WRITE_END           0x41
#define MAX77705_OPCODE_READ                0x51

#define MAX77705_UIC_APCMDRES               (1U << 7)
#define MAX77705_PD_DATAROLE_DFP            (1U << 7)
#define MAX77705_PD_PSRDY_STAT              (1U << 4)

// CC_STATUS0 bitfields
#define MAX77705_CCPINSTAT_SHIFT            6
#define MAX77705_CCPINSTAT_MASK             0xC0   // 1 = CC1, 2 = CC2
#define MAX77705_CCISTAT_SHIFT              4
#define MAX77705_CCISTAT_MASK               0x30   // 1 = 500mA, 2 = 1.5A, 3 = 3.0A
#define MAX77705_CCSTAT_MASK                0x07   // 0 = None, 1 = Sink (UFP/charging), 2 = Source (DFP/OTG)

#define MAX77705_CC_NONE                    0
#define MAX77705_CC_SINK                    1
#define MAX77705_CC_SOURCE                  2

// MAX77705 OPCODE mailbox commands
#define MAX77705_OP_CTRL1_W                 0x06
#define MAX77705_OP_SET_SNKCAP              0x2E
#define MAX77705_OP_CURRENT_SRCCAP          0x30
#define MAX77705_OP_SRCCAP_REQUEST          0x32
#define MAX77705_OP_SET_VDM_REQ             0x48
#define MAX77705_OP_GET_VDM_RESP            0x4B
#define MAX77705_OP_SET_ALTMODE             0x55
#define MAX77705_OP_FW_AUTOIBUS             0x57

#define MAX77705_MUIC_COM_USB               0x09
#define MAX77705_AUTOIBUS_AT_OFF            0x01
#define MAX77705_SVDM_DP_ENTER_MODE         0xFF018104U
#define MAX77705_SVDM_DP_CONFIGURE          0xFF018111U
#define MAX77705_DP_CONF_UFP_D_V13          0x06

// MAX77705 Charger registers (0x69)
#define MAX77705_CHG_REG_INT_OK             0xB2
#define MAX77705_CHG_REG_CNFG_00            0xB7
#define MAX77705_CHG_REG_CNFG_02            0xB9
#define MAX77705_CHG_REG_CNFG_09            0xC0
#define MAX77705_CHGIN_OK_BIT               (1U << 6)
#define MAX77705_MODE_CHARGER_BUCK          0x05
#define MAX77705_MODE_OTG_BOOST             0x0A
#define MAX77705_OTG_ILIM_1500MA            0xC0

typedef struct _S22_USBC_CONTEXT {
    WDFDEVICE       Device;
    WDFWAITLOCK     HwLock;
    WDFTIMER        PollTimer;

    volatile UCHAR  *I2cGpioBase;       // Region 0: TLMM I2C5 (0x0F1CE000, 0x2000)
    SIZE_T          I2cGpioLen;
    volatile UCHAR  *QscratchBase;      // Region 1: DWC3 QSCRATCH (0x0A6F8800, 0x400)
    SIZE_T          QscratchLen;
    volatile UCHAR  *QmpPhyBase;        // Region 2: QMP Combo PHY (0x088E8000, 0x3000)
    SIZE_T          QmpPhyLen;

    BOOLEAN         VirtualMode;
    S22_USBC_STATE  State;
    UCHAR           LastCcStat;
} S22_USBC_CONTEXT, *PS22_USBC_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S22_USBC_CONTEXT, S22UsbCGetContext)

// usbc_hw.c
VOID S22UsbCHwInit(_In_ PS22_USBC_CONTEXT Ctx);
VOID S22UsbCHwPoll(_In_ PS22_USBC_CONTEXT Ctx);
VOID S22UsbCApplyRole(_In_ PS22_USBC_CONTEXT Ctx, _In_ S22_USB_ROLE Role);
VOID S22UsbCApplyOrientation(_In_ PS22_USBC_CONTEXT Ctx, _In_ S22_CC_ORIENTATION Orient);
