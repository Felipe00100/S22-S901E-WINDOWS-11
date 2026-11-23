#include "s22wlanpdc.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD S22WlanEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE S22WlanEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE S22WlanEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED S22WlanEvtD0EntryPostIrq;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED S22WlanEvtD0ExitPreIrq;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL S22WlanEvtIoDeviceControl;
EVT_WDF_INTERRUPT_ISR S22PcieMsiIsr;

static BOOLEAN
S22IsRootEnumeratedDevice(
    _In_ WDFDEVICE Device
    )
{
    WCHAR EnumName[32];
    ULONG RequiredLen = 0;
    NTSTATUS Status;

    RtlZeroMemory(EnumName, sizeof(EnumName));
    Status = WdfDeviceQueryProperty(
        Device,
        DevicePropertyEnumeratorName,
        sizeof(EnumName) - sizeof(WCHAR),
        EnumName,
        &RequiredLen);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }

    return (_wcsicmp(EnumName, L"ROOT") == 0);
}

static VOID
S22MaskAllDwcImsiRx(
    _In_ PS22_WLAN_CONTEXT Ctx
    )
{
    ULONG Bank;

    if (Ctx->DbiBase == NULL) {
        return;
    }

    for (Bank = 0; Bank < PCIE_MSI_NUM_BANKS; Bank++) {
        ULONG Off = Bank * PCIE_MSI_CTRL_STRIDE;
        WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_ENABLE + Off), 0x00000000U);
        WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_MASK + Off), 0xFFFFFFFFU);
        WRITE_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_STATUS + Off), 0xFFFFFFFFU);
    }
}

static VOID
S22ProgramDwcImsiRx(
    _In_ PS22_WLAN_CONTEXT Ctx
    )
{
    ULONG Bank;
    ULONG RpId;

    Ctx->State.BridgeActive = FALSE;

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
    ULONG Iter;
    BOOLEAN Claimed = FALSE;

    UNREFERENCED_PARAMETER(MessageID);

    // Guard against Dx / unprogrammed / unmapped DBI access in DIRQL
    if (InterlockedCompareExchange(&Ctx->Armed, 0, 0) == 0 ||
        Ctx->DbiBase == NULL ||
        Bank >= PCIE_MSI_NUM_BANKS)
    {
        return FALSE;
    }

    Off = Bank * PCIE_MSI_CTRL_STRIDE;

    for (Iter = 0; Iter < S22_MSI_ISR_MAX_ITERS; Iter++) {
        ULONG Status = READ_REGISTER_ULONG((PULONG)(Ctx->DbiBase + PCIE_MSI_INTR0_STATUS + Off));

        if (Status == 0U) {
            break;
        }

        // 0xFFFFFFFF indicates PCIe0 clock/link drop or bus error — never W1C or spin!
        if (Status == 0xFFFFFFFFU) {
            InterlockedIncrement(&Ctx->State.BusErrorCount);
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

    if (Iter == S22_MSI_ISR_MAX_ITERS) {
        InterlockedIncrement(&Ctx->State.IsrSaturatedCount);
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
            if (Ctx->GicdBase != NULL && InterlockedCompareExchange(&Ctx->Armed, 0, 0) != 0) {
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
        if (Ctx->State.BridgeActive) {
            InterlockedExchange(&Ctx->Armed, 1);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
S22WlanEvtD0ExitPreIrq(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PS22_WLAN_CONTEXT Ctx = S22WlanGetContext(Device);
    UNREFERENCED_PARAMETER(TargetState);

    // 1. Disarm ISR guard before touching hardware so concurrent ISRs bail out safely
    InterlockedExchange(&Ctx->Armed, 0);
    Ctx->State.BridgeActive = FALSE;

    // 2. Mask and disable all iMSI-RX banks before PCIe0 enters Dx
    if (!Ctx->VirtualMode) {
        S22MaskAllDwcImsiRx(Ctx);
    }

    return STATUS_SUCCESS;
}

static VOID
S22CleanupHardwareResources(
    _In_ PS22_WLAN_CONTEXT Ctx
    )
{
    ULONG i;

    InterlockedExchange(&Ctx->Armed, 0);
    Ctx->State.BridgeActive = FALSE;

    S22MaskAllDwcImsiRx(Ctx);

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
    ULONG IrqResCount = 0;
    ULONG IrqIdx = 0;
    NTSTATUS LastIrqErr = STATUS_SUCCESS;
    PHYSICAL_ADDRESS PhysAddr;

    RtlZeroMemory(&Ctx->State, sizeof(Ctx->State));
    RtlZeroMemory(Ctx->BankOrMask, sizeof(Ctx->BankOrMask));
    InterlockedExchange(&Ctx->Armed, 0);
    Ctx->State.Version     = 16;
    Ctx->DbiBase           = NULL;
    Ctx->GicdBase          = NULL;
    Ctx->InterruptCount    = 0;
    Ctx->ConnectedBankMask = 0;
    Ctx->VirtualMode       = FALSE;

    for (i = 0; i < Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (Desc->Type == CmResourceTypeInterrupt) {
            IrqResCount++;
        }
    }

    // Strict VirtualMode check (P-4): ONLY allow VirtualMode on ROOT-enumerated test devices.
    if (IrqResCount == 0) {
        if (S22IsRootEnumeratedDevice(Device)) {
            Ctx->VirtualMode = TRUE;
            Ctx->State.VirtualMode = TRUE;
            Ctx->State.LastStatus = (ULONG)STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        Ctx->State.LastStatus = (ULONG)STATUS_DEVICE_CONFIGURATION_ERROR;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Ctx->State.VirtualMode = FALSE;

    PhysAddr.QuadPart = (LONGLONG)S22_PCIE0_DBI_PHYS;
    Ctx->DbiBase = (volatile UCHAR *)MmMapIoSpaceEx(PhysAddr, S22_PCIE0_DBI_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->DbiBase == NULL) {
        Ctx->State.LastStatus = (ULONG)STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PhysAddr.QuadPart = (LONGLONG)S22_GICD_PHYS;
    Ctx->GicdBase = (volatile UCHAR *)MmMapIoSpaceEx(PhysAddr, S22_GICD_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->GicdBase == NULL) {
        S22CleanupHardwareResources(Ctx);
        Ctx->State.LastStatus = (ULONG)STATUS_INSUFFICIENT_RESOURCES;
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
                RawDesc->u.Interrupt.Vector < (PCIE_MSI_BASE_GSIV + PCIE_MSI_NUM_BANKS))
            {
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
                LastIrqErr = Status;
            }
        }
    }

    Ctx->InterruptCount = IrqIdx;
    Ctx->State.IrqCountConnected = IrqIdx;

    // Bank 2 (APIC 0x0D MSI Frame: INTID 64..95) is mandatory for WCN6855 MSI delivery.
    if ((Ctx->ConnectedBankMask & (1U << PCIE_MSI_FRAME_BANK)) == 0U) {
        NTSTATUS FailStatus = !NT_SUCCESS(LastIrqErr) ? LastIrqErr : STATUS_DEVICE_CONFIGURATION_ERROR;
        S22CleanupHardwareResources(Ctx);
        Ctx->State.LastStatus = (ULONG)FailStatus;
        return FailStatus;
    }

    Ctx->State.LastStatus = (ULONG)(NT_SUCCESS(LastIrqErr) ? STATUS_SUCCESS : LastIrqErr);
    return STATUS_SUCCESS;
}

NTSTATUS
S22WlanEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PS22_WLAN_CONTEXT Ctx = S22WlanGetContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    S22CleanupHardwareResources(Ctx);
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
    DECLARE_CONST_UNICODE_STRING(SddlAdminOnly, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    UNREFERENCED_PARAMETER(Driver);

    // Restrict IOCTL device access to SYSTEM and Administrators only (P-6)
    Status = WdfDeviceInitAssignSDDLString(DeviceInit, &SddlAdminOnly);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware              = S22WlanEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware              = S22WlanEvtReleaseHardware;
    Pnp.EvtDeviceD0EntryPostInterruptsEnabled = S22WlanEvtD0EntryPostIrq;
    Pnp.EvtDeviceD0ExitPreInterruptsDisabled  = S22WlanEvtD0ExitPreIrq;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attrs, S22_WLAN_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attrs, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ctx = S22WlanGetContext(Device);
    Ctx->Device      = Device;
    Ctx->VirtualMode = FALSE;
    InterlockedExchange(&Ctx->Armed, 0);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QConfig, WdfIoQueueDispatchSequential);
    QConfig.EvtIoDeviceControl = S22WlanEvtIoDeviceControl;
    Status = WdfIoQueueCreate(Device, &QConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) {
        return Status;
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
