#include "max77705batt.h"

static __forceinline VOID
TlmmWriteReg(
    _In_ volatile UCHAR *Base,
    _In_ ULONG PinIdx,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    volatile ULONG *Reg = (volatile ULONG *)(Base + ((SIZE_T)PinIdx * TLMM_GPIO_STRIDE) + Offset);
    WRITE_REGISTER_ULONG((PULONG)Reg, Value);
}

static __forceinline ULONG
TlmmReadReg(
    _In_ volatile UCHAR *Base,
    _In_ ULONG PinIdx,
    _In_ ULONG Offset
    )
{
    volatile ULONG *Reg = (volatile ULONG *)(Base + ((SIZE_T)PinIdx * TLMM_GPIO_STRIDE) + Offset);
    return READ_REGISTER_ULONG((PULONG)Reg);
}

static __forceinline VOID
I2cSetLine(
    _In_ volatile UCHAR *Base,
    _In_ ULONG PinIdx,
    _In_ BOOLEAN ReleaseHigh
    )
{
    // Preserve upper bits [31:10] (including SM8450 EGPIO_ENABLE bit 12)
    ULONG OldCfg = TlmmReadReg(Base, PinIdx, TLMM_GPIO_CFG_OFFSET);
    if (ReleaseHigh) {
        TlmmWriteReg(Base, PinIdx, TLMM_GPIO_CFG_OFFSET,
            (OldCfg & ~0x3FFU) | TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA);
    } else {
        TlmmWriteReg(Base, PinIdx, TLMM_GPIO_IN_OUT_OFFSET, 0);
        TlmmWriteReg(Base, PinIdx, TLMM_GPIO_CFG_OFFSET,
            (OldCfg & ~0x3FFU) | TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA | TLMM_CFG_OE);
    }
}

static __forceinline BOOLEAN
I2cReadLine(
    _In_ volatile UCHAR *Base,
    _In_ ULONG PinIdx
    )
{
    return (TlmmReadReg(Base, PinIdx, TLMM_GPIO_IN_OUT_OFFSET) & TLMM_IO_IN_BIT) != 0;
}

static __forceinline VOID
I2cDelay(VOID)
{
    KeStallExecutionProcessor(5);
}

static BOOLEAN
I2cWaitSclHigh(
    _In_ volatile UCHAR *Base
    )
{
    ULONG Timeout;
    I2cSetLine(Base, TLMM_PIN_IDX_SCL, TRUE);
    for (Timeout = 0; Timeout < 200; Timeout++) {
        if (I2cReadLine(Base, TLMM_PIN_IDX_SCL)) {
            return TRUE;
        }
        KeStallExecutionProcessor(5);
    }
    return FALSE;
}

static VOID
I2cStart(
    _In_ volatile UCHAR *Base
    )
{
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    I2cWaitSclHigh(Base);
    I2cDelay();
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, FALSE);
    I2cDelay();
    I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
    I2cDelay();
}

static VOID
I2cStop(
    _In_ volatile UCHAR *Base
    )
{
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, FALSE);
    I2cDelay();
    I2cWaitSclHigh(Base);
    I2cDelay();
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    I2cDelay();
}

static BOOLEAN
I2cWriteByte(
    _In_ volatile UCHAR *Base,
    _In_ UCHAR Byte
    )
{
    INT Bit;
    BOOLEAN Ack;

    for (Bit = 7; Bit >= 0; Bit--) {
        I2cSetLine(Base, TLMM_PIN_IDX_SDA, ((Byte >> Bit) & 1U) != 0);
        I2cDelay();
        if (!I2cWaitSclHigh(Base)) {
            return FALSE;
        }
        I2cDelay();
        I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
        I2cDelay();
    }

    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    I2cDelay();
    if (!I2cWaitSclHigh(Base)) {
        return FALSE;
    }
    Ack = !I2cReadLine(Base, TLMM_PIN_IDX_SDA);
    I2cDelay();
    I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
    I2cDelay();

    return Ack;
}

static UCHAR
I2cReadByte(
    _In_ volatile UCHAR *Base,
    _In_ BOOLEAN SendAck
    )
{
    INT Bit;
    UCHAR Byte = 0;

    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);

    for (Bit = 7; Bit >= 0; Bit--) {
        I2cDelay();
        if (!I2cWaitSclHigh(Base)) {
            break;
        }
        if (I2cReadLine(Base, TLMM_PIN_IDX_SDA)) {
            Byte |= (UCHAR)(1U << Bit);
        }
        I2cDelay();
        I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
    }

    I2cSetLine(Base, TLMM_PIN_IDX_SDA, !SendAck);
    I2cDelay();
    I2cWaitSclHigh(Base);
    I2cDelay();
    I2cSetLine(Base, TLMM_PIN_IDX_SCL, FALSE);
    I2cSetLine(Base, TLMM_PIN_IDX_SDA, TRUE);
    I2cDelay();

    return Byte;
}

VOID
TlmmI2cInitPins(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx
    )
{
    if (DevCtx->VirtualMode || DevCtx->I2cGpioBase == NULL) {
        return;
    }

    I2cSetLine(DevCtx->I2cGpioBase, TLMM_PIN_IDX_SDA, TRUE);
    I2cSetLine(DevCtx->I2cGpioBase, TLMM_PIN_IDX_SCL, TRUE);
}

NTSTATUS
TlmmI2cReadRegs(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx,
    _In_ UCHAR SlaveAddr7,
    _In_ UCHAR RegAddr,
    _Out_writes_bytes_(Len) UCHAR *OutBuf,
    _In_ ULONG Len
    )
{
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    if (DevCtx->VirtualMode || DevCtx->I2cGpioBase == NULL || OutBuf == NULL || Len == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(DevCtx->I2cLock, NULL);

    I2cStart(DevCtx->I2cGpioBase);
    if (!I2cWriteByte(DevCtx->I2cGpioBase, (UCHAR)(SlaveAddr7 << 1))) {
        Status = STATUS_IO_DEVICE_ERROR;
        goto Stop;
    }

    if (!I2cWriteByte(DevCtx->I2cGpioBase, RegAddr)) {
        Status = STATUS_IO_DEVICE_ERROR;
        goto Stop;
    }

    I2cStart(DevCtx->I2cGpioBase);
    if (!I2cWriteByte(DevCtx->I2cGpioBase, (UCHAR)((SlaveAddr7 << 1) | 1U))) {
        Status = STATUS_IO_DEVICE_ERROR;
        goto Stop;
    }

    for (i = 0; i < Len; i++) {
        OutBuf[i] = I2cReadByte(DevCtx->I2cGpioBase, (i + 1) < Len);
    }

Stop:
    I2cStop(DevCtx->I2cGpioBase);
    WdfWaitLockRelease(DevCtx->I2cLock);
    return Status;
}

NTSTATUS
TlmmI2cWriteRegs(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx,
    _In_ UCHAR SlaveAddr7,
    _In_ UCHAR RegAddr,
    _In_reads_bytes_(Len) const UCHAR *Data,
    _In_ ULONG Len
    )
{
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    if (DevCtx->VirtualMode || DevCtx->I2cGpioBase == NULL || Data == NULL || Len == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(DevCtx->I2cLock, NULL);

    I2cStart(DevCtx->I2cGpioBase);
    if (!I2cWriteByte(DevCtx->I2cGpioBase, (UCHAR)(SlaveAddr7 << 1))) {
        Status = STATUS_IO_DEVICE_ERROR;
        goto Stop;
    }

    if (!I2cWriteByte(DevCtx->I2cGpioBase, RegAddr)) {
        Status = STATUS_IO_DEVICE_ERROR;
        goto Stop;
    }

    for (i = 0; i < Len; i++) {
        if (!I2cWriteByte(DevCtx->I2cGpioBase, Data[i])) {
            Status = STATUS_IO_DEVICE_ERROR;
            goto Stop;
        }
    }

Stop:
    I2cStop(DevCtx->I2cGpioBase);
    WdfWaitLockRelease(DevCtx->I2cLock);
    return Status;
}

//
// Apply s22-linux verified MAX77705 hardware fixes on boot:
//   1. Disarm stale bootloader MAX17042 VALRT_Th (0xFF00) to stop ~5.7 Hz IRQ storms.
//   2. Mask MAX77705_AICL_I (INT_MASK bit 7) to stop 345 Hz IRQ storms.
//   3. Raise OTG VBUS boost current limit to 1500 mA (CNFG_02[7:6] = 3) for USB-C hubs.
//
VOID
Max77705InitHardwareQuirks(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx
    )
{
    UCHAR ValrtDisable[2] = { 0x00, 0xFF };
    UCHAR RegVal;

    if (DevCtx->VirtualMode) {
        return;
    }

    // 1. Disarm stale VALRT_Th on Fuel Gauge 0x36
    (VOID)TlmmI2cWriteRegs(DevCtx, MAX77705_FG_ADDR, MAX17042_REG_VALRT_TH, ValrtDisable, 2);

    // 2. Mask MAX77705_AICL_I on Charger 0x69 (prevents 345 Hz IRQ storm)
    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_INT_MASK, &RegVal, 1))) {
        RegVal |= MAX77705_AICL_I_BIT;
        (VOID)TlmmI2cWriteRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_INT_MASK, &RegVal, 1);
    }

    // 3. Raise OTG VBUS boost current limit to 1500 mA
    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_02, &RegVal, 1))) {
        RegVal = (UCHAR)((RegVal & ~MAX77705_OTG_ILIM_MASK) | MAX77705_OTG_ILIM_1500MA);
        (VOID)TlmmI2cWriteRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_02, &RegVal, 1);
    }
}

//
// Port of s22-linux local-max17042-charge-control-limit.patch:
// Hold MAX77705 in Buck-only USB power-path (CNFG_00 mode 0x04) once SOC >= threshold
// (50..100%), resuming normal charging (0x05) with 2% hysteresis while never disturbing
// OTG boost mode (0x0A).
//
VOID
Max77705SyncChargeLimit(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx,
    _In_ ULONG SocPercent
    )
{
    UCHAR Cnfg00;
    ULONG Limit = DevCtx->ChargeControlEndThreshold;

    if (DevCtx->VirtualMode || Limit < 50 || Limit > 100) {
        return;
    }

    if (!NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_00, &Cnfg00, 1))) {
        return;
    }

    // Leave OTG boost mode (0x0A) untouched
    if ((Cnfg00 & MAX77705_MODE_OTG_BOOST) == MAX77705_MODE_OTG_BOOST) {
        return;
    }

    if (Limit < 100 && SocPercent >= Limit) {
        if ((Cnfg00 & 0x01) != 0) {
            UCHAR Next = (UCHAR)((Cnfg00 & ~0x05) | MAX77705_MODE_BUCK_ONLY);
            (VOID)TlmmI2cWriteRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_00, &Next, 1);
        }
    } else if (Limit >= 100 || (Limit >= 2 && SocPercent <= (Limit - 2))) {
        if ((Cnfg00 & 0x01) == 0) {
            UCHAR Next = (UCHAR)((Cnfg00 & ~0x05) | MAX77705_MODE_CHARGER_BUCK);
            (VOID)TlmmI2cWriteRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_00, &Next, 1);
        }
    }
}

NTSTATUS
Max77705ReadTelemetry(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx,
    _Out_ PMAX77705_TELEMETRY Tel
    )
{
    UCHAR Buf2[2];
    UCHAR Val8;
    USHORT Raw16;
    SHORT RawS16;
    NTSTATUS Status;

    RtlZeroMemory(Tel, sizeof(*Tel));

    if (DevCtx->VirtualMode) {
        // Deterministic simulated telemetry for QEMU ARM64 VM driver validation
        Tel->UsbOnline       = TRUE;
        Tel->ChargeLimitHeld = (DevCtx->ChargeControlEndThreshold < 100 && 78 >= DevCtx->ChargeControlEndThreshold);
        Tel->Charging        = !Tel->ChargeLimitHeld;
        Tel->SocPercent      = 78;
        Tel->VoltageMv       = 4120;
        Tel->CurrentMa       = Tel->Charging ? 3034 : 0;   // ~12.5 W charging
        Tel->AvgCurrentMa    = Tel->CurrentMa;
        Tel->PowerMw         = (LONG)(((LONGLONG)Tel->VoltageMv * Tel->CurrentMa) / 1000LL);
        Tel->FullCapMwh      = S22_DESIGN_CAPACITY_MWH;
        Tel->RemCapMwh       = (S22_DESIGN_CAPACITY_MWH * Tel->SocPercent) / 100;
        Tel->CycleCount      = 42;
        Tel->TempTenthsK     = 3050;   // 31.8 C (305.0 K)
        return STATUS_SUCCESS;
    }

    // Read RepSOC (%)
    Status = TlmmI2cReadRegs(DevCtx, MAX77705_FG_ADDR, MAX17042_REG_REPSOC, Buf2, 2);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Tel->SocPercent = Buf2[1];
    if (Tel->SocPercent > 100) {
        Tel->SocPercent = 100;
    }

    // Enforce charge control end threshold (50..100%)
    Max77705SyncChargeLimit(DevCtx, Tel->SocPercent);

    // Read VCELL (mV, 78.125 uV/LSB on bit[15:3] -> 625 uV/LSB)
    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_FG_ADDR, MAX17042_REG_VCELL, Buf2, 2))) {
        Raw16 = (USHORT)(Buf2[0] | ((USHORT)Buf2[1] << 8));
        Tel->VoltageMv = (ULONG)(((ULONGLONG)(Raw16 >> 3) * 625ULL) / 1000ULL);
    } else {
        Tel->VoltageMv = S22_DESIGN_VOLTAGE_MV;
    }

    // Read Current & AvgCurrent (2 mOhm sense resistor on S22 r0q -> 1.5625 uV / 2 mOhm = 0.78125 mA/LSB)
    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_FG_ADDR, MAX17042_REG_CURRENT, Buf2, 2))) {
        RawS16 = (SHORT)(Buf2[0] | ((USHORT)Buf2[1] << 8));
        Tel->CurrentMa = (LONG)(((LONGLONG)RawS16 * 78125LL) / 100000LL);
    }

    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_FG_ADDR, MAX17042_REG_AVGCURRENT, Buf2, 2))) {
        RawS16 = (SHORT)(Buf2[0] | ((USHORT)Buf2[1] << 8));
        Tel->AvgCurrentMa = (LONG)(((LONGLONG)RawS16 * 78125LL) / 100000LL);
    } else {
        Tel->AvgCurrentMa = Tel->CurrentMa;
    }

    // Compute signed Power in mW (+ charging, - discharging)
    Tel->PowerMw = (LONG)(((LONGLONG)Tel->VoltageMv * (LONGLONG)Tel->CurrentMa) / 1000LL);

    // Read Temperature (signed 8.8 C -> convert to 0.1 Kelvin)
    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_FG_ADDR, MAX17042_REG_TEMP, Buf2, 2))) {
        LONG TempC = (CHAR)Buf2[1];
        Tel->TempTenthsK = (TempC * 10) + 2732;
    } else {
        Tel->TempTenthsK = 3000;
    }

    // Read Cycle Count
    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_FG_ADDR, MAX17042_REG_CYCLES, Buf2, 2))) {
        Tel->CycleCount = (ULONG)(Buf2[0] | ((USHORT)Buf2[1] << 8));
    }

    // Read FullCap & RepCap (2 mOhm sense resistor -> 5 uVh / 2 mOhm = 2.5 mAh/LSB)
    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_FG_ADDR, MAX17042_REG_FULLCAP, Buf2, 2))) {
        Raw16 = (USHORT)(Buf2[0] | ((USHORT)Buf2[1] << 8));
        ULONG FullMah = (ULONG)(((ULONGLONG)Raw16 * 25ULL) / 10ULL);
        Tel->FullCapMwh = (FullMah >= 2000 && FullMah <= 6000)
            ? (ULONG)(((ULONGLONG)FullMah * S22_DESIGN_VOLTAGE_MV) / 1000ULL)
            : S22_DESIGN_CAPACITY_MWH;
    } else {
        Tel->FullCapMwh = S22_DESIGN_CAPACITY_MWH;
    }
    Tel->RemCapMwh = (Tel->FullCapMwh * Tel->SocPercent) / 100;

    // Read Charger INT_OK & DETAILS_01 (0x69)
    if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_INT_OK, &Val8, 1))) {
        Tel->UsbOnline = ((Val8 & (MAX77705_CHGIN_OK_BIT | MAX77705_WCIN_OK_BIT)) != 0);
    }

    if (Tel->UsbOnline) {
        if (DevCtx->ChargeControlEndThreshold < 100 && Tel->SocPercent >= DevCtx->ChargeControlEndThreshold) {
            Tel->ChargeLimitHeld = TRUE;
            Tel->Charging = FALSE;
        } else if (NT_SUCCESS(TlmmI2cReadRegs(DevCtx, MAX77705_CHG_ADDR, MAX77705_CHG_REG_DETAILS_01, &Val8, 1))) {
            UCHAR ChgDtls = Val8 & 0x0F;
            // Match s22-linux: check Current or AvgCurrent > 0 so transient boot spikes don't report DISCHARGING
            Tel->Charging = (ChgDtls <= 0x03) || (Tel->CurrentMa > 0) || (Tel->AvgCurrentMa > 0);
        }
    }

    return STATUS_SUCCESS;
}
