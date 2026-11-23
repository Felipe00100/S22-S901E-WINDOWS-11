#include "max77705usbc.h"

//
// Open-drain I2C5 bit-bang on SM8450 TLMM (GPIO 206 SDA, GPIO 207 SCL)
//
static __forceinline VOID
TlmmWrite(volatile UCHAR *Base, ULONG Pin, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((PULONG)(Base + ((SIZE_T)Pin * TLMM_GPIO_STRIDE) + Off), Val);
}

static __forceinline ULONG
TlmmRead(volatile UCHAR *Base, ULONG Pin, ULONG Off)
{
    return READ_REGISTER_ULONG((PULONG)(Base + ((SIZE_T)Pin * TLMM_GPIO_STRIDE) + Off));
}

static __forceinline VOID
I2cSetLine(volatile UCHAR *Base, ULONG Pin, BOOLEAN High)
{
    if (High) {
        TlmmWrite(Base, Pin, TLMM_GPIO_CFG_OFFSET, TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA);
    } else {
        TlmmWrite(Base, Pin, TLMM_GPIO_IN_OUT_OFFSET, 0);
        TlmmWrite(Base, Pin, TLMM_GPIO_CFG_OFFSET, TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA | TLMM_CFG_OE);
    }
}

static __forceinline BOOLEAN
I2cReadLine(volatile UCHAR *Base, ULONG Pin)
{
    return (TlmmRead(Base, Pin, TLMM_GPIO_IN_OUT_OFFSET) & TLMM_IO_IN_BIT) != 0;
}

static BOOLEAN
I2cWaitScl(volatile UCHAR *Base)
{
    ULONG t;
    I2cSetLine(Base, TLMM_PIN_IDX_SCL, TRUE);
    for (t = 0; t < 200; t++) {
        if (I2cReadLine(Base, TLMM_PIN_IDX_SCL)) return TRUE;
        KeStallExecutionProcessor(5);
    }
    return FALSE;
}

static VOID
I2cStart(volatile UCHAR *Base)
{
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    I2cWaitScl(Base);
    KeStallExecutionProcessor(5);
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, FALSE);
    KeStallExecutionProcessor(5);
    I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
    KeStallExecutionProcessor(5);
}

static VOID
I2cStop(volatile UCHAR *Base)
{
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, FALSE);
    KeStallExecutionProcessor(5);
    I2cWaitScl(Base);
    KeStallExecutionProcessor(5);
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    KeStallExecutionProcessor(5);
}

static BOOLEAN
I2cTxByte(volatile UCHAR *Base, UCHAR Byte)
{
    INT Bit;
    BOOLEAN Ack;
    for (Bit = 7; Bit >= 0; Bit--) {
        I2cSetLine(Base, TLMM_PIN_IDX_SDA, ((Byte >> Bit) & 1U) != 0);
        KeStallExecutionProcessor(5);
        if (!I2cWaitScl(Base)) return FALSE;
        KeStallExecutionProcessor(5);
        I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
        KeStallExecutionProcessor(5);
    }
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    KeStallExecutionProcessor(5);
    if (!I2cWaitScl(Base)) return FALSE;
    Ack = !I2cReadLine(Base, TLMM_PIN_IDX_SDA);
    KeStallExecutionProcessor(5);
    I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
    KeStallExecutionProcessor(5);
    return Ack;
}

static UCHAR
I2cRxByte(volatile UCHAR *Base, BOOLEAN Ack)
{
    INT Bit;
    UCHAR Byte = 0;
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    for (Bit = 7; Bit >= 0; Bit--) {
        KeStallExecutionProcessor(5);
        if (!I2cWaitScl(Base)) break;
        if (I2cReadLine(Base, TLMM_PIN_IDX_SDA)) Byte |= (UCHAR)(1U << Bit);
        KeStallExecutionProcessor(5);
        I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
    }
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, !Ack);
    KeStallExecutionProcessor(5);
    I2cWaitScl(Base);
    KeStallExecutionProcessor(5);
    I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    KeStallExecutionProcessor(5);
    return Byte;
}

static NTSTATUS
I2cRead(PS22_USBC_CONTEXT Ctx, UCHAR Addr7, UCHAR Reg, UCHAR *Out, ULONG Len)
{
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;
    if (Ctx->I2cGpioBase == NULL || Out == NULL || Len == 0) return STATUS_INVALID_PARAMETER;

    I2cStart(Ctx->I2cGpioBase);
    if (!I2cTxByte(Ctx->I2cGpioBase, (UCHAR)(Addr7 << 1)) ||
        !I2cTxByte(Ctx->I2cGpioBase, Reg))
    {
        Status = STATUS_IO_DEVICE_ERROR;
        goto Done;
    }
    I2cStart(Ctx->I2cGpioBase);
    if (!I2cTxByte(Ctx->I2cGpioBase, (UCHAR)((Addr7 << 1) | 1U))) {
        Status = STATUS_IO_DEVICE_ERROR;
        goto Done;
    }
    for (i = 0; i < Len; i++) {
        Out[i] = I2cRxByte(Ctx->I2cGpioBase, (i + 1) < Len);
    }
Done:
    I2cStop(Ctx->I2cGpioBase);
    return Status;
}

static NTSTATUS
I2cWrite(PS22_USBC_CONTEXT Ctx, UCHAR Addr7, UCHAR Reg, const UCHAR *Data, ULONG Len)
{
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;
    if (Ctx->I2cGpioBase == NULL || Data == NULL || Len == 0) return STATUS_INVALID_PARAMETER;

    I2cStart(Ctx->I2cGpioBase);
    if (!I2cTxByte(Ctx->I2cGpioBase, (UCHAR)(Addr7 << 1)) ||
        !I2cTxByte(Ctx->I2cGpioBase, Reg))
    {
        Status = STATUS_IO_DEVICE_ERROR;
        goto Done;
    }
    for (i = 0; i < Len; i++) {
        if (!I2cTxByte(Ctx->I2cGpioBase, Data[i])) {
            Status = STATUS_IO_DEVICE_ERROR;
            goto Done;
        }
    }
Done:
    I2cStop(Ctx->I2cGpioBase);
    return Status;
}

//
// MAX77705 CCIC OPCODE mailbox helper (0x21..0x41 write, 0x51 read)
//
static NTSTATUS
Max77705SendOpcode(
    _In_ PS22_USBC_CONTEXT Ctx,
    _In_ UCHAR Opcode,
    _In_reads_bytes_opt_(PayloadLen) const UCHAR *Payload,
    _In_ ULONG PayloadLen,
    _Out_writes_bytes_opt_(RespLen) UCHAR *RespBuf,
    _In_ ULONG RespLen
    )
{
    UCHAR Cmd[33];
    UCHAR EndByte = 0x00;
    UCHAR UicInt = 0;
    ULONG Retry;
    NTSTATUS Status;

    if (PayloadLen > 31) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Cmd, sizeof(Cmd));
    Cmd[0] = Opcode;
    if (Payload != NULL && PayloadLen > 0) {
        RtlCopyMemory(&Cmd[1], Payload, PayloadLen);
    }

    Status = I2cWrite(Ctx, MAX77705_USBC_ADDR, MAX77705_OPCODE_WRITE, Cmd, PayloadLen + 1);
    if (!NT_SUCCESS(Status)) return Status;

    // Trigger opcode execution by writing OPCODE_WRITE_END (0x41)
    Status = I2cWrite(Ctx, MAX77705_USBC_ADDR, MAX77705_OPCODE_WRITE_END, &EndByte, 1);
    if (!NT_SUCCESS(Status)) return Status;

    for (Retry = 0; Retry < 40; Retry++) {
        LARGE_INTEGER Delay;
        Delay.QuadPart = -50000LL; // 5 ms
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);

        if (NT_SUCCESS(I2cRead(Ctx, MAX77705_USBC_ADDR, MAX77705_UIC_INT, &UicInt, 1)) &&
            (UicInt & MAX77705_UIC_APCMDRES) != 0)
        {
            if (RespBuf != NULL && RespLen > 0) {
                return I2cRead(Ctx, MAX77705_USBC_ADDR, MAX77705_OPCODE_READ, RespBuf, RespLen);
            }
            return STATUS_SUCCESS;
        }
    }

    return STATUS_IO_TIMEOUT;
}

//
// Program MAX77705 Charger CHGIN_ILIM (0x69 reg 0xC0: 25mA step + 100mA base)
//
static VOID
Max77705SetChgIlimMa(PS22_USBC_CONTEXT Ctx, ULONG Ma)
{
    UCHAR Val;
    if (Ma < 100) Ma = 100;
    if (Ma > 3200) Ma = 3200;
    Val = (UCHAR)((Ma - 100) / 25);
    if (NT_SUCCESS(I2cWrite(Ctx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_09, &Val, 1))) {
        Ctx->State.ChgIlimMa = Ma;
    }
}

//
// Enable or disable MAX77705 OTG 5V / 1500mA VBUS boost (0x69 reg 0xB7 / 0xB9)
//
static VOID
Max77705SetOtgVbus(PS22_USBC_CONTEXT Ctx, BOOLEAN Enable)
{
    UCHAR RegVal;
    if (Enable) {
        if (NT_SUCCESS(I2cRead(Ctx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_02, &RegVal, 1))) {
            RegVal = (UCHAR)((RegVal & ~0xC0) | MAX77705_OTG_ILIM_1500MA);
            (VOID)I2cWrite(Ctx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_02, &RegVal, 1);
        }
        RegVal = MAX77705_MODE_OTG_BOOST;
        (VOID)I2cWrite(Ctx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_00, &RegVal, 1);
        Ctx->State.OtgBoostActive = TRUE;
    } else {
        if (NT_SUCCESS(I2cRead(Ctx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_00, &RegVal, 1))) {
            if ((RegVal & 0x0F) == MAX77705_MODE_OTG_BOOST) {
                RegVal = (UCHAR)((RegVal & ~0x0F) | MAX77705_MODE_CHARGER_BUCK);
                (VOID)I2cWrite(Ctx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_00, &RegVal, 1);
            }
        }
        Ctx->State.OtgBoostActive = FALSE;
    }
}

//
// Port of s22-linux local-dwc3-qcom-role-vbus-override.patch:
// Drive QSCRATCH HS_PHY_CTRL (UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL) and
// SS_PHY_CTRL (LANE0_PWR_PRESENT) based on USB role so USB Device/SSH/MTP
// enumerates immediately at boot with NO cable replug required.
//
VOID
S22UsbCApplyRole(
    _In_ PS22_USBC_CONTEXT Ctx,
    _In_ S22_USB_ROLE Role
    )
{
    Ctx->State.UsbRole = (ULONG)Role;

    if (Ctx->VirtualMode) {
        Ctx->State.OtgBoostActive = (Role == S22UsbRoleHost);
        return;
    }

    if (Ctx->QscratchBase != NULL && Ctx->QscratchLen >= 0x40) {
        PULONG HsCtrl = (PULONG)(Ctx->QscratchBase + QSCRATCH_HS_PHY_CTRL);
        PULONG SsCtrl = (PULONG)(Ctx->QscratchBase + QSCRATCH_SS_PHY_CTRL);
        ULONG HsVal = READ_REGISTER_ULONG(HsCtrl);
        ULONG SsVal = READ_REGISTER_ULONG(SsCtrl);

        if (Role == S22UsbRoleDevice) {
            HsVal |= (QSCRATCH_UTMI_OTG_VBUS_VALID | QSCRATCH_SW_SESSVLD_SEL);
            SsVal |= QSCRATCH_LANE0_PWR_PRESENT;
        } else {
            HsVal &= ~(QSCRATCH_UTMI_OTG_VBUS_VALID | QSCRATCH_SW_SESSVLD_SEL);
            SsVal &= ~QSCRATCH_LANE0_PWR_PRESENT;
        }

        WRITE_REGISTER_ULONG(HsCtrl, HsVal);
        WRITE_REGISTER_ULONG(SsCtrl, SsVal);
    }

    // Drive OTG 5V/1500mA boost when operating as Host (DFP)
    Max77705SetOtgVbus(Ctx, (Role == S22UsbRoleHost));
}

//
// Port of s22-linux phy-qcom-qmp-combo.c + local-sm8450-qmp-combo-usb3-tuning.patch:
// Program SM8450 QMP Combo PHY CC1/CC2 SuperSpeed orientation & 4nm USB3 RX/PCS lock thresholds.
//
VOID
S22UsbCApplyOrientation(
    _In_ PS22_USBC_CONTEXT Ctx,
    _In_ S22_CC_ORIENTATION Orient
    )
{
    Ctx->State.Orientation = (ULONG)Orient;
    Ctx->State.SuperSpeedActive = (Orient != S22CcNone);

    if (Ctx->VirtualMode || Ctx->QmpPhyBase == NULL || Ctx->QmpPhyLen < 0x2000) {
        return;
    }

    if (Orient != S22CcNone) {
        ULONG Val = QMP_SW_PORTSELECT_MUX;
        if (Orient == S22Cc2Flipped) {
            Val |= QMP_SW_PORTSELECT_VAL;
        }
        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_COM_BASE + QMP_DP_COM_TYPEC_CTRL), Val);

        // Apply Galaxy S22 (SM8450 4nm) QMP USB3 RX & PCS lock-detect tuning from stock DT
        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_RXA_BASE + QSERDES_V5_RX_RX_MODE_01_LOW),  0x5B);
        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_RXA_BASE + QSERDES_V5_RX_RX_MODE_01_HIGH), 0x1B);
        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_RXB_BASE + QSERDES_V5_RX_RX_MODE_01_LOW),  0x5B);
        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_RXB_BASE + QSERDES_V5_RX_RX_MODE_01_HIGH), 0x1B);

        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_USB3_PCS_BASE + QPHY_V4_PCS_LOCK_DETECT_CONFIG1), 0xC4);
        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_USB3_PCS_BASE + QPHY_V4_PCS_LOCK_DETECT_CONFIG2), 0x89);
        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_USB3_PCS_BASE + QPHY_V4_PCS_LOCK_DETECT_CONFIG3), 0x20);
        WRITE_REGISTER_ULONG((PULONG)(Ctx->QmpPhyBase + QMP_USB3_PCS_BASE + QPHY_V4_PCS_LOCK_DETECT_CONFIG6), 0x13);
    }
}

//
// Negotiate USB-PD 5V/3A or 9V/2A (up to 15W) sink contract via MAX77705 OPCODE mailbox
//
static VOID
Max77705NegotiatePdSink(PS22_USBC_CONTEXT Ctx)
{
    UCHAR Resp[32];
    UCHAR NumPdos, BestIdx = 0;
    ULONG BestMv = 5000, BestMa = 500, i;

    RtlZeroMemory(Resp, sizeof(Resp));
    if (!NT_SUCCESS(Max77705SendOpcode(Ctx, MAX77705_OP_CURRENT_SRCCAP, NULL, 0, Resp, sizeof(Resp)))) {
        return;
    }

    NumPdos = Resp[1] & 0x07;
    if (NumPdos == 0 || NumPdos > 7) {
        return;
    }

    // Prefer 9V (<= 9000 mV, <= 15W) else highest 5V current
    for (i = 0; i < NumPdos; i++) {
        ULONG Pdo = (ULONG)Resp[2 + i * 4] |
                    ((ULONG)Resp[3 + i * 4] << 8) |
                    ((ULONG)Resp[4 + i * 4] << 16) |
                    ((ULONG)Resp[5 + i * 4] << 24);
        ULONG Type = (Pdo >> 30) & 0x03;
        if (Type == 0) { // Fixed Supply PDO
            ULONG Mv = ((Pdo >> 10) & 0x3FF) * 50;
            ULONG Ma = (Pdo & 0x3FF) * 10;
            if (Mv <= 9000 && (Mv > BestMv || (Mv == BestMv && Ma > BestMa))) {
                BestMv = Mv;
                BestMa = (Ma > 3000) ? 3000 : Ma;
                BestIdx = (UCHAR)(i + 1);
            }
        }
    }

    if (BestIdx > 0) {
        UCHAR ReqPayload[1] = { BestIdx };
        if (NT_SUCCESS(Max77705SendOpcode(Ctx, MAX77705_OP_SRCCAP_REQUEST, ReqPayload, 1, NULL, 0))) {
            Ctx->State.PdContractActive = TRUE;
            Ctx->State.PdVoltageMv = BestMv;
            Ctx->State.PdCurrentMa = BestMa;
            Max77705SetChgIlimMa(Ctx, BestMa);
        }
    }
}

VOID
S22UsbCHwInit(
    _In_ PS22_USBC_CONTEXT Ctx
    )
{
    RtlZeroMemory(&Ctx->State, sizeof(Ctx->State));
    Ctx->State.Version = 1;
    Ctx->State.VirtualMode = Ctx->VirtualMode;
    Ctx->LastCcStat = 0xFF;

    if (Ctx->VirtualMode) {
        // Deterministic simulated USB 3.0 SuperSpeed + USB-PD 9V/2A + Peripheral (NCM/SSH + MTP) state
        Ctx->State.CableAttached    = TRUE;
        Ctx->State.VbusPresent      = TRUE;
        Ctx->State.OtgBoostActive   = FALSE;
        Ctx->State.SuperSpeedActive = TRUE;
        Ctx->State.PdContractActive = TRUE;
        Ctx->State.DpAltModeActive  = FALSE;
        Ctx->State.DpHpdHigh        = FALSE;
        Ctx->State.UsbRole          = S22UsbRoleDevice;
        Ctx->State.Orientation      = S22Cc1Normal;
        Ctx->State.PdVoltageMv      = 9000;
        Ctx->State.PdCurrentMa      = 2000;
        Ctx->State.ChgIlimMa        = 2000;
        Ctx->State.DpPinAssignment  = 0;
        return;
    }

    WdfWaitLockAcquire(Ctx->HwLock, NULL);

    // Configure TLMM I2C5 lines as open-drain release-high
    if (Ctx->I2cGpioBase != NULL) {
        I2cSetLine(Ctx->I2cGpioBase, TLMM_PIN_IDX_SDA, TRUE);
        I2cSetLine(Ctx->I2cGpioBase, TLMM_PIN_IDX_SCL, TRUE);

        // Route MUIC switches to USB D+/D- and disable FW auto-IBUS throttling
        UCHAR MuicUsb = MAX77705_MUIC_COM_USB;
        (VOID)Max77705SendOpcode(Ctx, MAX77705_OP_CTRL1_W, &MuicUsb, 1, NULL, 0);

        UCHAR AutoIbus = MAX77705_AUTOIBUS_AT_OFF;
        (VOID)Max77705SendOpcode(Ctx, MAX77705_OP_FW_AUTOIBUS, &AutoIbus, 1, NULL, 0);

        // Advertise S22 Sink Capabilities: PDO1 = 5V/3A (Dual-Role/USB-Comm), PDO2 = 9V/2A
        UCHAR SnkCap[9] = {
            0x02,                   // 2 PDOs
            0x2C, 0x91, 0x01, 0x36, // 5V 3.0A Dual-Role + USB Comm + Data Swap
            0xC8, 0xD0, 0x02, 0x00  // 9V 2.0A Fixed Sink
        };
        (VOID)Max77705SendOpcode(Ctx, MAX77705_OP_SET_SNKCAP, SnkCap, sizeof(SnkCap), NULL, 0);
    }

    // Default to Peripheral VBUS override at boot so USB SSH/MTP works with zero replug
    S22UsbCApplyRole(Ctx, S22UsbRoleDevice);

    WdfWaitLockRelease(Ctx->HwLock);

    // Run immediate initial poll to latch real CC1/CC2 orientation and role
    S22UsbCHwPoll(Ctx);
}

VOID
S22UsbCHwPoll(
    _In_ PS22_USBC_CONTEXT Ctx
    )
{
    UCHAR Cc0 = 0, Pd0 = 0, Pd1 = 0, VdmInt = 0, ChgOk = 0;
    UCHAR CcStat, CcPin, CcI;

    if (Ctx->VirtualMode || Ctx->I2cGpioBase == NULL) {
        return;
    }

    WdfWaitLockAcquire(Ctx->HwLock, NULL);

    if (!NT_SUCCESS(I2cRead(Ctx, MAX77705_USBC_ADDR, MAX77705_CC_STATUS0, &Cc0, 1))) {
        goto Exit;
    }
    (VOID)I2cRead(Ctx, MAX77705_USBC_ADDR, MAX77705_PD_STATUS0, &Pd0, 1);
    (VOID)I2cRead(Ctx, MAX77705_USBC_ADDR, MAX77705_PD_STATUS1, &Pd1, 1);
    (VOID)I2cRead(Ctx, MAX77705_USBC_ADDR, MAX77705_VDM_INT, &VdmInt, 1);
    (VOID)I2cRead(Ctx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_INT_OK, &ChgOk, 1);

    Ctx->State.CcStatus0 = Cc0;
    Ctx->State.PdStatus0 = Pd0;
    Ctx->State.PdStatus1 = Pd1;
    Ctx->State.VbusPresent = ((ChgOk & MAX77705_CHGIN_OK_BIT) != 0);

    CcStat = Cc0 & MAX77705_CCSTAT_MASK;
    CcPin  = (Cc0 & MAX77705_CCPINSTAT_MASK) >> MAX77705_CCPINSTAT_SHIFT;
    CcI    = (Cc0 & MAX77705_CCISTAT_MASK) >> MAX77705_CCISTAT_SHIFT;

    Ctx->State.CableAttached = (CcStat != MAX77705_CC_NONE);

    // Update CC1/CC2 SuperSpeed orientation on QMP Combo PHY
    if (CcPin == 1) {
        S22UsbCApplyOrientation(Ctx, S22Cc1Normal);
    } else if (CcPin == 2) {
        S22UsbCApplyOrientation(Ctx, S22Cc2Flipped);
    } else {
        S22UsbCApplyOrientation(Ctx, S22CcNone);
    }

    // Handle role transitions & plug/unplug events
    if (CcStat != Ctx->LastCcStat) {
        Ctx->LastCcStat = CcStat;

        if (CcStat == MAX77705_CC_SOURCE) {
            // Host / OTG DFP attached
            Ctx->State.PdContractActive = FALSE;
            Ctx->State.PdVoltageMv = 5000;
            Ctx->State.PdCurrentMa = 1500;
            S22UsbCApplyRole(Ctx, S22UsbRoleHost);
        } else if (CcStat == MAX77705_CC_SINK) {
            // Device / UFP or Charger attached (apply Rp 1.5A / 3.0A CHGIN_ILIM immediately)
            ULONG RpMa = (CcI == 3) ? 3000 : ((CcI == 2) ? 1500 : 500);
            Ctx->State.PdVoltageMv = 5000;
            Ctx->State.PdCurrentMa = RpMa;
            Max77705SetChgIlimMa(Ctx, RpMa);

            // Data role from PD_STATUS1[7]: DFP -> Host, else Device/Peripheral
            S22_USB_ROLE DataRole = ((Pd1 & MAX77705_PD_DATAROLE_DFP) != 0)
                ? S22UsbRoleHost : S22UsbRoleDevice;
            S22UsbCApplyRole(Ctx, DataRole);

            if ((Pd1 & MAX77705_PD_PSRDY_STAT) != 0) {
                Max77705NegotiatePdSink(Ctx);
            }
        } else {
            // Unplugged: turn off OTG VBUS, clear PD/DP state, clear VBUS override
            Ctx->State.PdContractActive = FALSE;
            Ctx->State.DpAltModeActive  = FALSE;
            Ctx->State.DpHpdHigh        = FALSE;
            Ctx->State.DpPinAssignment  = 0;
            Ctx->State.PdVoltageMv      = 0;
            Ctx->State.PdCurrentMa      = 0;
            S22UsbCApplyRole(Ctx, S22UsbRoleNone);
        }
    } else if (CcStat == MAX77705_CC_SINK && !Ctx->State.PdContractActive) {
        // Apply Type-C Rp 1.5A/3.0A CHGIN_ILIM (port of local-max77705-usbc-typec-rp-ilim.patch)
        ULONG RpMa = (CcI == 3) ? 3000 : ((CcI == 2) ? 1500 : 500);
        if (Ctx->State.ChgIlimMa != RpMa) {
            Max77705SetChgIlimMa(Ctx, RpMa);
        }
        if ((Pd1 & MAX77705_PD_PSRDY_STAT) != 0) {
            Max77705NegotiatePdSink(Ctx);
        }
    }

    // Track DisplayPort Alt-Mode SVDM (0xFF01) events if a DP adapter/dock is attached
    if (VdmInt != 0 && Ctx->State.CableAttached) {
        Ctx->State.DpAltModeActive = TRUE;
        if (Ctx->State.DpPinAssignment == 0) {
            Ctx->State.DpPinAssignment = 'D'; // Default 2-lane DP + USB3 SuperSpeed Pin D
        }
        Ctx->State.DpHpdHigh = ((Pd0 & 0x10) != 0);
    }

Exit:
    WdfWaitLockRelease(Ctx->HwLock);
}
