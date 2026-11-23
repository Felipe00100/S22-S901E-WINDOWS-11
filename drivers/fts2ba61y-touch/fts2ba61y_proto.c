#include "fts2ba61y.h"

static VOID
FtsSleepMs(
    _In_ ULONG Milliseconds
    )
{
    LARGE_INTEGER Interval;
    Interval.QuadPart = -10000LL * (LONGLONG)Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static USHORT
FtsQueryScanTime100Us(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    ULONGLONG Freq = (ULONGLONG)DevCtx->QpcFreq.QuadPart;
    LARGE_INTEGER Now;
    ULONGLONG Secs;
    ULONGLONG Rem;
    ULONGLONG Units100Us;

    if (Freq == 0ULL) {
        return (USHORT)(DevCtx->ScanTime + 40U);
    }

    Now = KeQueryPerformanceCounter(NULL);
    Secs = (ULONGLONG)Now.QuadPart / Freq;
    Rem  = (ULONGLONG)Now.QuadPart % Freq;
    Units100Us = (Secs * 10000ULL) + ((Rem * 10000ULL) / Freq);
    return (USHORT)(Units100Us & 0xFFFFULL);
}

static VOID
FtsBuildAndEnqueueReportLocked(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    FTS_HID_TOUCH_REPORT Report;
    UCHAR i;
    UCHAR Count = 0;

    RtlZeroMemory(&Report, sizeof(Report));
    Report.ReportId = REPORTID_TOUCH;

    for (i = 0; i < FTS_MAX_CONTACTS; i++) {
        if (DevCtx->Slots[i].Active) {
            UCHAR StatusBits = FTS_STATUS_TIP_SWITCH | FTS_STATUS_IN_RANGE;
            if (DevCtx->Slots[i].Confident) {
                StatusBits |= FTS_STATUS_CONFIDENCE;
            }
            Report.Contacts[Count].Status    = StatusBits;
            Report.Contacts[Count].ContactId = i;
            Report.Contacts[Count].Pressure  = DevCtx->Slots[i].Pressure;
            Report.Contacts[Count].X         = DevCtx->Slots[i].X;
            Report.Contacts[Count].Y         = DevCtx->Slots[i].Y;
            Count++;
        } else if (DevCtx->Slots[i].ReleasePending) {
            // Emit explicit final TipSwitch=0 / InRange=0 report per Windows Touch HID spec
            UCHAR StatusBits = DevCtx->Slots[i].Confident ? FTS_STATUS_CONFIDENCE : 0;
            Report.Contacts[Count].Status    = StatusBits;
            Report.Contacts[Count].ContactId = i;
            Report.Contacts[Count].Pressure  = 0;
            Report.Contacts[Count].X         = DevCtx->Slots[i].X;
            Report.Contacts[Count].Y         = DevCtx->Slots[i].Y;
            DevCtx->Slots[i].ReleasePending  = FALSE;
            Count++;
        }
    }

    Report.ContactCount = Count;
    Report.ScanTime     = DevCtx->ScanTime;

    // Short FIFO ring queue (T-9): prevents quick press+release taps from being lost
    if (DevCtx->ReportCount == FTS_REPORT_QUEUE_SIZE) {
        DevCtx->ReportHead = (DevCtx->ReportHead + 1U) % FTS_REPORT_QUEUE_SIZE;
        DevCtx->ReportCount--;
    }
    DevCtx->ReportQueue[DevCtx->ReportTail] = Report;
    DevCtx->ReportTail = (DevCtx->ReportTail + 1U) % FTS_REPORT_QUEUE_SIZE;
    DevCtx->ReportCount++;
}

static VOID
FtsReleaseAllActiveSlots(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _Out_ PBOOLEAN HasUpdate
    )
{
    UCHAR i;
    BOOLEAN HadActive = FALSE;

    WdfSpinLockAcquire(DevCtx->StateLock);
    for (i = 0; i < FTS_MAX_CONTACTS; i++) {
        if (DevCtx->Slots[i].Active) {
            DevCtx->Slots[i].Active         = FALSE;
            DevCtx->Slots[i].ReleasePending = TRUE;
            DevCtx->Slots[i].Pressure       = 0;
            HadActive = TRUE;
        }
    }
    if (HadActive) {
        DevCtx->ScanTime = FtsQueryScanTime100Us(DevCtx);
        FtsBuildAndEnqueueReportLocked(DevCtx);
        *HasUpdate = TRUE;
    }
    WdfSpinLockRelease(DevCtx->StateLock);
}

static NTSTATUS
FtsWriteLocked(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_reads_bytes_(CmdLen) const UCHAR *Cmd,
    _In_ ULONG CmdLen,
    _In_reads_bytes_opt_(DataLen) const UCHAR *Data,
    _In_ ULONG DataLen
    )
{
    UCHAR TxBuf[64];
    ULONG Len = CmdLen + DataLen;

    if (CmdLen == 0 || Len < CmdLen || (Len + 1U) > sizeof(TxBuf)) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(TxBuf, Cmd, CmdLen);
    if (DataLen > 0 && Data != NULL) {
        RtlCopyMemory(TxBuf + CmdLen, Data, DataLen);
    }

    // Prepend CUSTOM_W (0xC0) unless it is a direct REG_W (0xFA) or REG_R (0xFB)
    if (Cmd[0] != FTS2BA61Y_CMD_REG_W && Cmd[0] != FTS2BA61Y_CMD_REG_R) {
        RtlMoveMemory(TxBuf + 1, TxBuf, Len);
        TxBuf[0] = FTS2BA61Y_CMD_CUSTOM_W;
        Len++;
    }

    return TlmmSpiTransferLocked(DevCtx, TxBuf, NULL, Len);
}

static NTSTATUS
FtsWrite(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_reads_bytes_(CmdLen) const UCHAR *Cmd,
    _In_ ULONG CmdLen,
    _In_reads_bytes_opt_(DataLen) const UCHAR *Data,
    _In_ ULONG DataLen
    )
{
    NTSTATUS Status;

    WdfWaitLockAcquire(DevCtx->SpiLock, NULL);
    Status = FtsWriteLocked(DevCtx, Cmd, CmdLen, Data, DataLen);
    WdfWaitLockRelease(DevCtx->SpiLock);
    return Status;
}

static NTSTATUS
FtsRead(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_reads_bytes_(TxLen) const UCHAR *Reg,
    _In_ ULONG TxLen,
    _Out_writes_bytes_(RxLen) UCHAR *OutBuf,
    _In_ ULONG RxLen
    )
{
    ULONG EffectiveTxLen;
    ULONG TotalLen;
    BOOLEAN IsDirectCmd;
    NTSTATUS Status;

    if (Reg == NULL || OutBuf == NULL || TxLen == 0 || RxLen == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    IsDirectCmd = (Reg[0] == FTS2BA61Y_CMD_READ_EVENT ||
                   Reg[0] == FTS2BA61Y_CMD_REG_W ||
                   Reg[0] == FTS2BA61Y_CMD_REG_R);

    // Strict TxLen & TotalLen bounds check (T-8), matching Linux fts2ba61y_read()
    if (IsDirectCmd) {
        EffectiveTxLen = TxLen;
        if (TxLen > FTS2BA61Y_MAX_XFER_SIZE ||
            RxLen > FTS2BA61Y_MAX_XFER_SIZE ||
            (RxLen + 1U + EffectiveTxLen) > FTS2BA61Y_MAX_XFER_SIZE)
        {
            return STATUS_INVALID_PARAMETER;
        }
    } else {
        if (TxLen > 3U ||
            RxLen > FTS2BA61Y_MAX_XFER_SIZE ||
            (RxLen + 4U) > FTS2BA61Y_MAX_XFER_SIZE)
        {
            return STATUS_INVALID_PARAMETER;
        }
        EffectiveTxLen = 3U;
    }

    WdfWaitLockAcquire(DevCtx->SpiLock, NULL);

    RtlZeroMemory(DevCtx->SpiTxBuf, sizeof(DevCtx->SpiTxBuf));
    RtlZeroMemory(DevCtx->SpiRxBuf, sizeof(DevCtx->SpiRxBuf));

    if (IsDirectCmd) {
        RtlCopyMemory(DevCtx->SpiTxBuf, Reg, TxLen);
    } else {
        // Matches Linux fts2ba61y_read(): 0xD1 + big-endian 16-bit reg_val (0x0000 for TxLen==1)
        USHORT RegVal = 0;
        if (TxLen == 2U) {
            RegVal = Reg[0];
        } else if (TxLen == 3U) {
            RegVal = (USHORT)(Reg[0] | ((USHORT)Reg[1] << 8));
        }

        DevCtx->SpiTxBuf[0] = FTS2BA61Y_CMD_CUSTOM_R;
        DevCtx->SpiTxBuf[1] = (UCHAR)(RegVal >> 8);
        DevCtx->SpiTxBuf[2] = (UCHAR)(RegVal & 0xFF);

        Status = FtsWriteLocked(DevCtx, Reg, 1, NULL, 0);
        if (!NT_SUCCESS(Status)) {
            goto Exit;
        }
    }

    TotalLen = RxLen + 1U + EffectiveTxLen;
    Status = TlmmSpiTransferLocked(DevCtx, DevCtx->SpiTxBuf, DevCtx->SpiRxBuf, TotalLen);
    if (NT_SUCCESS(Status)) {
        RtlCopyMemory(OutBuf, DevCtx->SpiRxBuf + 1U + EffectiveTxLen, RxLen);
    }

Exit:
    WdfWaitLockRelease(DevCtx->SpiLock);
    return Status;
}

static NTSTATUS
FtsWaitForReady(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    UCHAR Buffer[FTS2BA61Y_EVENT_BUFF_SIZE];
    UCHAR Cmd = FTS2BA61Y_CMD_READ_EVENT;
    INT Retries;

    // Matches Linux fts2ba61y_wait_for_ready() (5 retries * 20 ms = 100 ms max)
    // Layout verified against Linux fts2ba61y.c: FTS2BA61Y_MASK_STYPE = GENMASK(5,2)
    for (Retries = 0; Retries < 5; Retries++) {
        NTSTATUS Status = FtsRead(DevCtx, &Cmd, 1, Buffer, sizeof(Buffer));
        if (NT_SUCCESS(Status)) {
            UCHAR Stype = (Buffer[0] >> 2) & 0x0F;
            UCHAR StatusId = Buffer[1];
            if (Stype == FTS2BA61Y_EVENT_STATUSTYPE_INFO &&
                StatusId == FTS2BA61Y_INFO_READY_STATUS)
            {
                return STATUS_SUCCESS;
            }
        }
        FtsSleepMs(20);
    }

    return STATUS_DEVICE_NOT_READY;
}

static NTSTATUS
FtsReset(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    UCHAR Cmd = FTS2BA61Y_CMD_REG_W;
    const UCHAR ResetSeq[FTS2BA61Y_RESET_CMD_SIZE] = { 0x20, 0x00, 0x00, 0x24, 0x81 };
    NTSTATUS Status;

    Status = FtsWrite(DevCtx, &Cmd, 1, ResetSeq, sizeof(ResetSeq));
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    FtsSleepMs(30);
    return FtsWaitForReady(DevCtx);
}

static NTSTATUS
FtsReadPanelInfo(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    UCHAR Cmd = FTS2BA61Y_CMD_READ_PANEL_INFO;
    UCHAR Data[FTS2BA61Y_PANEL_INFO_SIZE];
    NTSTATUS Status;

    Status = FtsRead(DevCtx, &Cmd, 1, Data, sizeof(Data));
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    // Reject dead/unpowered SPI reads (all 0x00 or all 0xFF)
    {
        BOOLEAN AllZero = TRUE;
        BOOLEAN AllFF   = TRUE;
        ULONG k;
        for (k = 0; k < sizeof(Data); k++) {
            if (Data[k] != 0x00) AllZero = FALSE;
            if (Data[k] != 0xFF) AllFF   = FALSE;
        }
        if (AllZero || AllFF) {
            return STATUS_DEVICE_NOT_READY;
        }
    }

    DevCtx->MaxX = (USHORT)(((USHORT)Data[0] << 8) | Data[1]);
    DevCtx->MaxY = (USHORT)(((USHORT)Data[2] << 8) | Data[3]);
    DevCtx->TxCount = (Data[8] > 0) ? Data[8] : 10;

    if (DevCtx->MaxX < 100 || DevCtx->MaxX > 16384 ||
        DevCtx->MaxY < 100 || DevCtx->MaxY > 16384)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
FtsSetTouchFunc(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    UCHAR Cmd = FTS2BA61Y_CMD_TOUCHTYPE;
    UCHAR TouchType[2] = {
        (UCHAR)(FTS2BA61Y_TOUCHTYPE_DEFAULT & 0xFF),
        (UCHAR)((FTS2BA61Y_TOUCHTYPE_DEFAULT >> 8) & 0xFF)
    };

    return FtsWrite(DevCtx, &Cmd, 1, TouchType, sizeof(TouchType));
}

NTSTATUS
FtsHardwareInit(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS ResetStatus;
    NTSTATUS PanelStatus;
    NTSTATUS FuncStatus;

    DevCtx->HardwareReady = FALSE;

    if (DevCtx->VirtualMode) {
        DevCtx->MaxX = 1079;
        DevCtx->MaxY = 2339;
        DevCtx->TxCount = FTS_MAX_CONTACTS;
        DevCtx->HardwareReady = TRUE;
        return STATUS_SUCCESS;
    }

    TlmmSpiInitPins(DevCtx);
    FtsSleepMs(140); // Match Linux fts2ba61y_hw_power_on post-rail stabilization

    ResetStatus = FtsReset(DevCtx);
    if (!NT_SUCCESS(ResetStatus)) {
        FtsSleepMs(30);
        ResetStatus = FtsReset(DevCtx);
    }

    PanelStatus = FtsReadPanelInfo(DevCtx);
    if (!NT_SUCCESS(ResetStatus) && !NT_SUCCESS(PanelStatus)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "FTS2BA61Y: SPI comms failed (Reset=0x%08X, PanelInfo=0x%08X)\n",
            ResetStatus, PanelStatus));
        return STATUS_DEVICE_NOT_READY;
    }

    if (!NT_SUCCESS(PanelStatus)) {
        DevCtx->MaxX = 1079;
        DevCtx->MaxY = 2339;
        DevCtx->TxCount = FTS_MAX_CONTACTS;
    }

    FuncStatus = FtsSetTouchFunc(DevCtx);
    if (!NT_SUCCESS(FuncStatus)) {
        return FuncStatus;
    }

    DevCtx->HardwareReady = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
FtsSetSensing(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Enable
    )
{
    UCHAR Cmd;
    NTSTATUS Status;

    if (DevCtx->VirtualMode) {
        InterlockedExchange(&DevCtx->SensingEnabled, Enable ? 1 : 0);
        return STATUS_SUCCESS;
    }

    if (Enable) {
        Cmd = FTS2BA61Y_CMD_CLEAR_EVENTS;
        (VOID)FtsWrite(DevCtx, &Cmd, 1, NULL, 0);

        Cmd = FTS2BA61Y_CMD_SENSE_ON;
        Status = FtsWrite(DevCtx, &Cmd, 1, NULL, 0);
        if (NT_SUCCESS(Status)) {
            InterlockedExchange(&DevCtx->SensingEnabled, 1);
        }
        return Status;
    } else {
        InterlockedExchange(&DevCtx->SensingEnabled, 0);
        Cmd = FTS2BA61Y_CMD_SENSE_OFF;
        Status = FtsWrite(DevCtx, &Cmd, 1, NULL, 0);
        return Status;
    }
}

NTSTATUS
FtsPollEvents(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _Out_ PBOOLEAN HasUpdate
    )
{
    UCHAR *Buffer = DevCtx->EventBuf;
    UCHAR Cmd = FTS2BA61Y_CMD_READ_EVENT;
    ULONG LeftEvents = 0;
    ULONG i;
    NTSTATUS Status;

    *HasUpdate = FALSE;

    if (DevCtx->VirtualMode ||
        !DevCtx->HardwareReady ||
        InterlockedCompareExchange(&DevCtx->SensingEnabled, 0, 0) == 0)
    {
        return STATUS_SUCCESS;
    }

    if (!TlmmIsIrqAsserted(DevCtx)) {
        return STATUS_SUCCESS;
    }

    Status = FtsRead(DevCtx, &Cmd, 1, Buffer, FTS2BA61Y_EVENT_BUFF_SIZE);
    if (!NT_SUCCESS(Status)) {
        // Release any active fingers so a transient SPI error never leaves a stuck touch (T-1)
        FtsReleaseAllActiveSlots(DevCtx, HasUpdate);
        return Status;
    }
    if (Buffer[0] == 0) {
        return STATUS_SUCCESS;
    }

    // Verified verbatim against Linux drivers/input/touchscreen/fts2ba61y.c (FTS2BA61Y_MASK_LEFT_EVENTS = GENMASK(4,0))
    LeftEvents = Buffer[7] & 0x1F;
    if (LeftEvents >= FTS2BA61Y_EVENT_COUNT) {
        UCHAR ClearCmd = FTS2BA61Y_CMD_CLEAR_EVENTS;
        (VOID)FtsWrite(DevCtx, &ClearCmd, 1, NULL, 0);
        FtsReleaseAllActiveSlots(DevCtx, HasUpdate);
        return STATUS_DATA_OVERRUN;
    }

    if (LeftEvents > 0) {
        Status = FtsRead(
            DevCtx,
            &Cmd,
            1,
            Buffer + FTS2BA61Y_EVENT_BUFF_SIZE,
            LeftEvents * FTS2BA61Y_EVENT_BUFF_SIZE);
        if (!NT_SUCCESS(Status)) {
            FtsReleaseAllActiveSlots(DevCtx, HasUpdate);
            return Status;
        }
    }

    WdfSpinLockAcquire(DevCtx->StateLock);

    {
        USHORT PressedInBurstMask = 0;

        for (i = 0; i <= LeftEvents; i++) {
            UCHAR *Ev = Buffer + (i * FTS2BA61Y_EVENT_BUFF_SIZE);
            UCHAR EventId = Ev[0] & 0x03;

            if (EventId == FTS2BA61Y_COORDINATE_EVENT) {
                UCHAR Tid = (Ev[0] >> 2) & 0x0F;
                UCHAR Action = (Ev[0] >> 6) & 0x03;

                if (Tid < FTS_MAX_CONTACTS) {
                    if (Action == FTS2BA61Y_ACTION_PRESS || Action == FTS2BA61Y_ACTION_MOVE) {
                        // Verified verbatim against Linux fts2ba61y.c (TTYPE_3_2 = Ev[6][7:6], TTYPE_1_0 = Ev[7][1:0])
                        UCHAR TType = (UCHAR)((((Ev[6] >> 6) & 0x03) << 2) | (Ev[7] & 0x03));
                        if (TType == FTS2BA61Y_TOUCHTYPE_NORMAL ||
                            TType == FTS2BA61Y_TOUCHTYPE_PALM ||
                            TType == FTS2BA61Y_TOUCHTYPE_WET ||
                            TType == FTS2BA61Y_TOUCHTYPE_GLOVE)
                        {
                            USHORT RawX = (USHORT)(((USHORT)Ev[1] << 4) | ((Ev[3] >> 4) & 0x0F));
                            USHORT RawY = (USHORT)(((USHORT)Ev[2] << 4) | (Ev[3] & 0x0F));
                            ULONG ScaledX = (DevCtx->MaxX > 0)
                                ? (((ULONG)RawX * FTS_LOGICAL_MAX_X) / DevCtx->MaxX)
                                : (ULONG)RawX;
                            ULONG ScaledY = (DevCtx->MaxY > 0)
                                ? (((ULONG)RawY * FTS_LOGICAL_MAX_Y) / DevCtx->MaxY)
                                : (ULONG)RawY;
                            UCHAR RawPress = Ev[6] & 0x3F;

                            // Clamp to HID Logical Maximum [0..4095] (T-2)
                            if (ScaledX > FTS_LOGICAL_MAX_X) {
                                ScaledX = FTS_LOGICAL_MAX_X;
                            }
                            if (ScaledY > FTS_LOGICAL_MAX_Y) {
                                ScaledY = FTS_LOGICAL_MAX_Y;
                            }

                            if (!DevCtx->Slots[Tid].Active) {
                                PressedInBurstMask |= (USHORT)(1U << Tid);
                            }
                            DevCtx->Slots[Tid].Active         = TRUE;
                            DevCtx->Slots[Tid].ReleasePending = FALSE;
                            DevCtx->Slots[Tid].Confident      = (TType != FTS2BA61Y_TOUCHTYPE_PALM);
                            DevCtx->Slots[Tid].X              = (USHORT)ScaledX;
                            DevCtx->Slots[Tid].Y              = (USHORT)ScaledY;
                            DevCtx->Slots[Tid].Pressure       = (RawPress > 0) ? RawPress : 1;
                            *HasUpdate = TRUE;
                        }
                    } else if (Action == FTS2BA61Y_ACTION_RELEASE) {
                        if (DevCtx->Slots[Tid].Active) {
                            // If PRESS and RELEASE arrive in the same SPI burst, flush the PRESS
                            // report into the FIFO first so fast taps are never collapsed/lost (T-9).
                            if ((PressedInBurstMask & (USHORT)(1U << Tid)) != 0) {
                                DevCtx->ScanTime = FtsQueryScanTime100Us(DevCtx);
                                FtsBuildAndEnqueueReportLocked(DevCtx);
                                PressedInBurstMask &= (USHORT)~(1U << Tid);
                            }
                            DevCtx->Slots[Tid].Active         = FALSE;
                            DevCtx->Slots[Tid].ReleasePending = TRUE;
                            DevCtx->Slots[Tid].Pressure       = 0;
                            *HasUpdate = TRUE;
                        }
                    }
                }
            }
        }
    }

    if (*HasUpdate) {
        DevCtx->ScanTime = FtsQueryScanTime100Us(DevCtx);
        FtsBuildAndEnqueueReportLocked(DevCtx);
    }

    WdfSpinLockRelease(DevCtx->StateLock);
    return Status;
}

BOOLEAN
FtsDequeueHidReport(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _Out_ PFTS_HID_TOUCH_REPORT Report
    )
{
    BOOLEAN Found = FALSE;

    WdfSpinLockAcquire(DevCtx->StateLock);
    if (DevCtx->ReportCount > 0) {
        *Report = DevCtx->ReportQueue[DevCtx->ReportHead];
        DevCtx->ReportHead = (DevCtx->ReportHead + 1U) % FTS_REPORT_QUEUE_SIZE;
        DevCtx->ReportCount--;
        Found = TRUE;
    }
    WdfSpinLockRelease(DevCtx->StateLock);

    return Found;
}

VOID
FtsClearReportState(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    WdfSpinLockAcquire(DevCtx->StateLock);
    RtlZeroMemory(DevCtx->Slots, sizeof(DevCtx->Slots));
    DevCtx->ReportHead  = 0;
    DevCtx->ReportTail  = 0;
    DevCtx->ReportCount = 0;
    WdfSpinLockRelease(DevCtx->StateLock);
}
