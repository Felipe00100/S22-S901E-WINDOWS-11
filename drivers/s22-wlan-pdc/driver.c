#include "s22wlanpdc.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD S22WlanEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE S22WlanEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE S22WlanEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED S22WlanEvtD0EntryPostIrq;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL S22WlanEvtIoDeviceControl;
EVT_WDF_INTERRUPT_ISR S22PcieMsiIsr;

static VOID
S22ProgramDwcImsiRx(
    _In_ PS22_WLAN_CONTEXT Ctx
    )
{
    ULONG Bank;
    ULONG RpId;

    if (Ctx->DbiBase == NULL) {
        return;
    }

    RpId = READ_REGISTER_ULONG((PULONG)(Ctx->DbiBase + 0x0000U));
    Ctx->State.DbiRpId = RpId;
    if (RpId == 0xFFFFFFFFU || RpId == 0U) {
        return;
    }

    WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_ADDR_LO), S22_MSI_DOORBELL_ADDR_LO);
    WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_ADDR_HI), S22_MSI_DOORBELL_ADDR_HI);

    for (Bank = 0; Bank < PCIE_MSI_NUM_BANKS; Bank++) {
        ULONG Off = Bank * PCIE_MSI_CTRL_STRIDE;
        WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_STATUS + Off), 0xFFFFFFFFU);
        if ((Ctx->ConnectedBankMask & (1U << Bank)) != 0U) {
            WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_MASK + Off), 0x00000000U);
            WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_ENABLE + Off), 0xFFFFFFFFU);
        } else {
            WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_ENABLE + Off), 0x00000000U);
            WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_MASK + Off), 0xFFFFFFFFU);
        }
    }

    Ctx->State.DbiMsiAddrLo = READ_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_ADDR_LO));
    Ctx->State.BridgeActive = (Ctx->State.DbiMsiAddrLo == S22_MSI_DOORBELL_ADDR_LO);
}

BOOLEAN
S22PcieMsiIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
    PS22_WLAN_CONTEXT Ctx = S22WlanGetContext(Device);
    PS22_MSI_IRQ_CTX IrqCtx = S22MsiIrqGetContext(Interrupt);
    ULONG Bank = IrqCtx->Bank;
    ULONG Off;
    ULONG Status;
    BOOLEAN Claimed = FALSE;

    UNREFERENCED_PARAMETER(MessageID);

    if (Ctx->DbiBase == NULL || Bank >= PCIE_MSI_NUM_BANKS) {
        return FALSE;
    }

    Off = Bank * PCIE_MSI_CTRL_STRIDE;

    for (;;) {
        Status = READ_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_STATUS + Off));
        if (Status == 0U) {
            break;
        }

        Claimed = TRUE;

        // Acknowledge W1C status bit(s) in DesignWare iMSI-RX immediately
        WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_STATUS + Off), Status);

        // Forward enabled Bank 2 MSI vectors (INTID 64..95) directly into GICD_ISPENDR[2]
        if (Bank == PCIE_MSI_FRAME_BANK && Ctx->GicdBase != NULL) {
            ULONG En2 = READ_REGISTER_ULONG((PULONG)(Ctx->GicdBase + GICD_ISENABLER_OFFSET + PCIE_MSI_FRAME_BANK * 4U));
            ULONG ToInject = Status & En2;
            if (ToInject != 0U) {
                WRITE_REGISTER_ULONG((PULONG)(Ctx->GicdBase + GICD_ISPENDR_OFFSET + PCIE_MSI_FRAME_BANK * 4U), ToInject);
            }
        }

        InterlockedIncrement(&Ctx->State.MsiIrqCount);
        InterlockedOr(&Ctx->BankOrMask[Bank], (LONG)Status);
    }

    return Claimed;
}

VOID
S22WlanEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PS22_WLAN_CONTEXT Ctx = S22WlanGetContext(Device);
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Info = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode == IOCTL_S22_WLAN_GET_STATE) {
        PS22_WLAN_PDC_STATE Out = NULL;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(S22_WLAN_PDC_STATE), (PVOID *)&Out, NULL);
        if (NT_SUCCESS(Status)) {
            if (Ctx->GicdBase != NULL) {
                Ctx->State.GicdIsenabler2 = READ_REGISTER_ULONG((PULONG)(Ctx->GicdBase + GICD_ISENABLER_OFFSET + 2U * 4U));
            }
            Ctx->State.LastMsiBank = PCIE_MSI_FRAME_BANK;
            Ctx->State.LastMsiMask = (ULONG)InterlockedCompareExchange(&Ctx->BankOrMask[PCIE_MSI_FRAME_BANK], 0, 0);
            RtlCopyMemory(Out, &Ctx->State, sizeof(S22_WLAN_PDC_STATE));
            Info = sizeof(S22_WLAN_PDC_STATE);
        }
    }

    WdfRequestCompleteWithInformation(Request, Status, Info);
}

NTSTATUS
S22WlanEvtD0EntryPostIrq(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PS22_WLAN_CONTEXT Ctx = S22WlanGetContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    if (!Ctx->VirtualMode) {
        S22ProgramDwcImsiRx(Ctx);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
S22WlanEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PS22_WLAN_CONTEXT Ctx = S22WlanGetContext(Device);
    ULONG Count = WdfCmResourceListGetCount(ResourcesTranslated);
    ULONG i;
    ULONG IrqIdx = 0;
    PHYSICAL_ADDRESS PhysAddr;

    RtlZeroMemory(&Ctx->State, sizeof(Ctx->State));
    RtlZeroMemory(Ctx->BankOrMask, sizeof(Ctx->BankOrMask));
    Ctx->State.Version     = 15;
    Ctx->DbiBase           = NULL;
    Ctx->GicdBase          = NULL;
    Ctx->InterruptCount    = 0;
    Ctx->ConnectedBankMask = 0;
    Ctx->VirtualMode       = TRUE;

    for (i = 0; i < Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (Desc->Type == CmResourceTypeInterrupt) {
            Ctx->VirtualMode = FALSE;
            break;
        }
    }
    Ctx->State.VirtualMode = Ctx->VirtualMode;

    if (Ctx->VirtualMode) {
        Ctx->State.LastStatus = (ULONG)STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }

    PhysAddr.QuadPart = (LONGLONG)S22_PCIE0_DBI_PHYS;
    Ctx->DbiBase = (volatile UCHAR *)MmMapIoSpaceEx(PhysAddr, S22_PCIE0_DBI_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->DbiBase == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PhysAddr.QuadPart = (LONGLONG)S22_GICD_PHYS;
    Ctx->GicdBase = (volatile UCHAR *)MmMapIoSpaceEx(PhysAddr, S22_GICD_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->GicdBase == NULL) {
        MmUnmapIoSpace((PVOID)Ctx->DbiBase, S22_PCIE0_DBI_SIZE);
        Ctx->DbiBase = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < Count && IrqIdx < PCIE_MSI_NUM_BANKS; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR RawDesc   = WdfCmResourceListGetDescriptor(ResourcesRaw, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR TransDesc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (TransDesc->Type == CmResourceTypeInterrupt) {
            WDF_INTERRUPT_CONFIG IrqCfg;
            WDF_OBJECT_ATTRIBUTES IrqAttrs;
            WDFINTERRUPT WdfIrq = NULL;
            NTSTATUS Status;
            ULONG Bank = IrqIdx;

            if (RawDesc != NULL &&
                RawDesc->u.Interrupt.Vector >= PCIE_MSI_BASE_GSIV &&
                RawDesc->u.Interrupt.Vector < (PCIE_MSI_BASE_GSIV + PCIE_MSI_NUM_BANKS)) {
                Bank = RawDesc->u.Interrupt.Vector - PCIE_MSI_BASE_GSIV;
            }

            WDF_INTERRUPT_CONFIG_INIT(&IrqCfg, S22PcieMsiIsr, NULL);
            IrqCfg.AutomaticSerialization = FALSE;
            IrqCfg.InterruptRaw           = RawDesc;
            IrqCfg.InterruptTranslated    = TransDesc;

            WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&IrqAttrs, S22_MSI_IRQ_CTX);
            Status = WdfInterruptCreate(Device, &IrqCfg, &IrqAttrs, &WdfIrq);
            if (NT_SUCCESS(Status) && WdfIrq != NULL) {
                PS22_MSI_IRQ_CTX IrqCtx = S22MsiIrqGetContext(WdfIrq);
                IrqCtx->Bank = Bank;
                Ctx->Interrupts[IrqIdx] = WdfIrq;
                Ctx->ConnectedBankMask |= (1U << Bank);
                IrqIdx++;
            } else {
                Ctx->State.LastStatus = (ULONG)Status;
            }
        }
    }

    Ctx->InterruptCount = IrqIdx;
    Ctx->State.IrqCountConnected = IrqIdx;
    if (IrqIdx > 0) {
        Ctx->State.LastStatus = (ULONG)STATUS_SUCCESS;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
S22WlanEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PS22_WLAN_CONTEXT Ctx = S22WlanGetContext(Device);
    ULONG i;
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (Ctx->DbiBase != NULL) {
        for (i = 0; i < PCIE_MSI_NUM_BANKS; i++) {
            ULONG Off = i * PCIE_MSI_CTRL_STRIDE;
            WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_ENABLE + Off), 0x00000000U);
            WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_MASK + Off), 0xFFFFFFFFU);
            WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_STATUS + Off), 0xFFFFFFFFU);
        }
    }

    for (i = 0; i < Ctx->InterruptCount; i++) {
        if (Ctx->Interrupts[i] != NULL) {
            WdfObjectDelete(Ctx->Interrupts[i]);
            Ctx->Interrupts[i] = NULL;
        }
    }
    Ctx->InterruptCount = 0;
    Ctx->ConnectedBankMask = 0;

    if (Ctx->DbiBase != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->DbiBase, S22_PCIE0_DBI_SIZE);
        Ctx->DbiBase = NULL;
    }
    if (Ctx->GicdBase != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->GicdBase, S22_GICD_SIZE);
        Ctx->GicdBase = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
S22WlanEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS Status;
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attrs;
    WDF_IO_QUEUE_CONFIG QConfig;
    WDFDEVICE Device;
    PS22_WLAN_CONTEXT Ctx;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware              = S22WlanEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware              = S22WlanEvtReleaseHardware;
    Pnp.EvtDeviceD0EntryPostInterruptsEnabled = S22WlanEvtD0EntryPostIrq;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attrs, S22_WLAN_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attrs, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ctx = S22WlanGetContext(Device);
    Ctx->Device      = Device;
    Ctx->VirtualMode = TRUE;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QConfig, WdfIoQueueDispatchSequential);
    QConfig.EvtIoDeviceControl = S22WlanEvtIoDeviceControl;
    Status = WdfIoQueueCreate(Device, &QConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    {
        DECLARE_CONST_UNICODE_STRING(SymLink, L"\\DosDevices\\S22WlanPdc");
        (VOID)WdfDeviceCreateSymbolicLink(Device, &SymLink);
    }
    (VOID)WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_S22_WLAN_PDC, NULL);
    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, S22WlanEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
