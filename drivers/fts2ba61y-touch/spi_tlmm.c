#include "fts2ba61y.h"

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
TlmmSetCfg(
    _In_ volatile UCHAR *Base,
    _In_ ULONG PinIdx,
    _In_ ULONG LowBits
    )
{
    // Preserve upper bits [31:10] (including SM8450 EGPIO_ENABLE bit 12)
    ULONG OldCfg = TlmmReadReg(Base, PinIdx, TLMM_GPIO_CFG_OFFSET);
    ULONG NewCfg = (OldCfg & ~0x3FFU) | (LowBits & 0x3FFU);
    TlmmWriteReg(Base, PinIdx, TLMM_GPIO_CFG_OFFSET, NewCfg);
}

static __forceinline VOID
TlmmSetOut(
    _In_ volatile UCHAR *Base,
    _In_ ULONG PinIdx,
    _In_ BOOLEAN High
    )
{
    TlmmWriteReg(Base, PinIdx, TLMM_GPIO_IN_OUT_OFFSET, High ? TLMM_IO_OUT_BIT : 0);
}

static __forceinline BOOLEAN
TlmmGetIn(
    _In_ volatile UCHAR *Base,
    _In_ ULONG PinIdx
    )
{
    return (TlmmReadReg(Base, PinIdx, TLMM_GPIO_IN_OUT_OFFSET) & TLMM_IO_IN_BIT) != 0;
}

static __forceinline VOID
SpiHalfBitDelay(VOID)
{
    // ~1 us half-cycle (~500 kHz conservative SPI bit-bang, well within 5 MHz max)
    KeStallExecutionProcessor(1);
}

VOID
TlmmSpiInitPins(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    if (DevCtx->VirtualMode || DevCtx->SpiGpioBase == NULL || DevCtx->IrqGpioBase == NULL) {
        return;
    }

    // Idle state: CS HIGH (inactive), SCK LOW (SPI Mode 0), MOSI LOW
    TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_CS, TRUE);
    TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_SCK, FALSE);
    TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_MOSI, FALSE);

    // Configure GPIO 28 (MISO) as Input, GPIO func, 6mA, no pull (RMW preserves EGPIO_ENABLE)
    TlmmSetCfg(DevCtx->SpiGpioBase, TLMM_PIN_IDX_MISO,
        TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA);

    // Configure GPIO 29 (MOSI), 30 (SCK), 31 (CS) as Output, GPIO func, 6mA, no pull
    TlmmSetCfg(DevCtx->SpiGpioBase, TLMM_PIN_IDX_MOSI,
        TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_OE);
    TlmmSetCfg(DevCtx->SpiGpioBase, TLMM_PIN_IDX_SCK,
        TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_OE);
    TlmmSetCfg(DevCtx->SpiGpioBase, TLMM_PIN_IDX_CS,
        TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_OE);

    // Configure GPIO 46 (TSP_INT) as Input, GPIO func, 2mA, no pull
    TlmmSetCfg(DevCtx->IrqGpioBase, 0,
        TLMM_CFG_PULL_NONE | TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA);
}

BOOLEAN
TlmmIsIrqAsserted(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    if (DevCtx->VirtualMode || DevCtx->IrqGpioBase == NULL) {
        return FALSE;
    }

    // FTS2BA61Y IRQ is active-low (IRQ_TYPE_LEVEL_LOW)
    return !TlmmGetIn(DevCtx->IrqGpioBase, 0);
}

NTSTATUS
TlmmSpiTransferLocked(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_reads_bytes_(Length) const UCHAR *TxBuf,
    _Out_writes_bytes_opt_(Length) UCHAR *RxBuf,
    _In_ ULONG Length
    )
{
    ULONG i;
    INT bit;

    if (DevCtx->VirtualMode) {
        if (RxBuf != NULL) {
            RtlZeroMemory(RxBuf, Length);
        }
        return STATUS_SUCCESS;
    }

    if (DevCtx->SpiGpioBase == NULL || TxBuf == NULL || Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    // Assert CS (active low)
    TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_SCK, FALSE);
    TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_CS, FALSE);
    SpiHalfBitDelay();

    for (i = 0; i < Length; i++) {
        UCHAR txByte = TxBuf[i];
        UCHAR rxByte = 0;

        for (bit = 7; bit >= 0; bit--) {
            // Mode 0 (CPOL=0, CPHA=0): set MOSI while SCK is low
            TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_MOSI, ((txByte >> bit) & 1U) != 0);
            SpiHalfBitDelay();

            // Rising edge: slave samples MOSI, master samples MISO
            TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_SCK, TRUE);
            if (TlmmGetIn(DevCtx->SpiGpioBase, TLMM_PIN_IDX_MISO)) {
                rxByte |= (UCHAR)(1U << bit);
            }
            SpiHalfBitDelay();

            // Falling edge
            TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_SCK, FALSE);
        }

        if (RxBuf != NULL) {
            RxBuf[i] = rxByte;
        }
    }

    // Deassert CS (inactive high)
    SpiHalfBitDelay();
    TlmmSetOut(DevCtx->SpiGpioBase, TLMM_PIN_IDX_CS, TRUE);
    SpiHalfBitDelay();

    return STATUS_SUCCESS;
}

NTSTATUS
TlmmSpiTransfer(
    _In_ PFTS_DEVICE_CONTEXT DevCtx,
    _In_reads_bytes_(Length) const UCHAR *TxBuf,
    _Out_writes_bytes_opt_(Length) UCHAR *RxBuf,
    _In_ ULONG Length
    )
{
    NTSTATUS Status;

    if (DevCtx->VirtualMode) {
        if (RxBuf != NULL) {
            RtlZeroMemory(RxBuf, Length);
        }
        return STATUS_SUCCESS;
    }

    if (DevCtx->SpiGpioBase == NULL || TxBuf == NULL || Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(DevCtx->SpiLock, NULL);
    Status = TlmmSpiTransferLocked(DevCtx, TxBuf, RxBuf, Length);
    WdfWaitLockRelease(DevCtx->SpiLock);
    return Status;
}
