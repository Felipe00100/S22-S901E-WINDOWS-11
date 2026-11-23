#include "max77705usbc.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD S22UsbCEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE S22UsbCEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE S22UsbCEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY S22UsbCEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT S22UsbCEvtD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL S22UsbCEvtIoDeviceControl;
EVT_WDF_TIMER S22UsbCEvtPollTimer;

VOID
S22UsbCEvtPollTimer(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PS22_USBC_CONTEXT Ctx = S22UsbCGetContext(Device);

    S22UsbCHwPoll(Ctx);
    WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(250));
}

VOID
S22UsbCEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PS22_USBC_CONTEXT Ctx = S22UsbCGetContext(Device);
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Info = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_S22_USBC_GET_STATE:
    {
        PS22_USBC_STATE Out = NULL;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(S22_USBC_STATE), (PVOID *)&Out, NULL);
        if (NT_SUCCESS(Status)) {
            RtlCopyMemory(Out, &Ctx->State, sizeof(S22_USBC_STATE));
            Info = sizeof(S22_USBC_STATE);
        }
        break;
    }

    case IOCTL_S22_USBC_SET_ROLE:
    {
        PULONG RoleIn = NULL;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID *)&RoleIn, NULL);
        if (NT_SUCCESS(Status)) {
            if (*RoleIn <= S22UsbRoleHost) {
                WdfWaitLockAcquire(Ctx->HwLock, NULL);
                S22UsbCApplyRole(Ctx, (S22_USB_ROLE)(*RoleIn));
                WdfWaitLockRelease(Ctx->HwLock);
                Status = STATUS_SUCCESS;
            } else {
                Status = STATUS_INVALID_PARAMETER;
            }
        }
        break;
    }

    default:
        Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, Status, Info);
}

NTSTATUS
S22UsbCEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PS22_USBC_CONTEXT Ctx = S22UsbCGetContext(Device);
    ULONG Count = WdfCmResourceListGetCount(ResourcesTranslated);
    ULONG i, MemIdx = 0;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    Ctx->I2cGpioBase  = NULL;
    Ctx->QscratchBase = NULL;
    Ctx->QmpPhyBase   = NULL;
    Ctx->VirtualMode  = TRUE;

    for (i = 0; i < Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (Desc->Type == CmResourceTypeMemory) {
            PVOID Mapped = MmMapIoSpaceEx(
                Desc->u.Memory.Start,
                Desc->u.Memory.Length,
                PAGE_READWRITE | PAGE_NOCACHE);
            if (Mapped == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            if (MemIdx == 0) {
                Ctx->I2cGpioBase = (volatile UCHAR *)Mapped;
                Ctx->I2cGpioLen  = Desc->u.Memory.Length;
            } else if (MemIdx == 1) {
                Ctx->QscratchBase = (volatile UCHAR *)Mapped;
                Ctx->QscratchLen  = Desc->u.Memory.Length;
            } else if (MemIdx == 2) {
                Ctx->QmpPhyBase = (volatile UCHAR *)Mapped;
                Ctx->QmpPhyLen  = Desc->u.Memory.Length;
            }
            MemIdx++;
        }
    }

    if (Ctx->I2cGpioBase != NULL) {
        Ctx->VirtualMode = FALSE;
    }

    S22UsbCHwInit(Ctx);
    return STATUS_SUCCESS;
}

NTSTATUS
S22UsbCEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PS22_USBC_CONTEXT Ctx = S22UsbCGetContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (Ctx->I2cGpioBase != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->I2cGpioBase, Ctx->I2cGpioLen);
        Ctx->I2cGpioBase = NULL;
    }
    if (Ctx->QscratchBase != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->QscratchBase, Ctx->QscratchLen);
        Ctx->QscratchBase = NULL;
    }
    if (Ctx->QmpPhyBase != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->QmpPhyBase, Ctx->QmpPhyLen);
        Ctx->QmpPhyBase = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
S22UsbCEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PS22_USBC_CONTEXT Ctx = S22UsbCGetContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    if (!Ctx->VirtualMode && Ctx->PollTimer != NULL) {
        WdfTimerStart(Ctx->PollTimer, WDF_REL_TIMEOUT_IN_MS(250));
    }
    return STATUS_SUCCESS;
}

NTSTATUS
S22UsbCEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PS22_USBC_CONTEXT Ctx = S22UsbCGetContext(Device);
    UNREFERENCED_PARAMETER(TargetState);

    if (Ctx->PollTimer != NULL) {
        WdfTimerStop(Ctx->PollTimer, TRUE);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
S22UsbCEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS Status;
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attrs;
    WDF_IO_QUEUE_CONFIG QConfig;
    WDF_TIMER_CONFIG TConfig;
    WDFDEVICE Device;
    PS22_USBC_CONTEXT Ctx;
    DECLARE_CONST_UNICODE_STRING(SymLink, L"\\DosDevices\\S22UsbC");

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = S22UsbCEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = S22UsbCEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry         = S22UsbCEvtD0Entry;
    Pnp.EvtDeviceD0Exit          = S22UsbCEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attrs, S22_USBC_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attrs, &Device);
    if (!NT_SUCCESS(Status)) return Status;

    Ctx = S22UsbCGetContext(Device);
    Ctx->Device      = Device;
    Ctx->VirtualMode = TRUE;

    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Ctx->HwLock);
    if (!NT_SUCCESS(Status)) return Status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QConfig, WdfIoQueueDispatchSequential);
    QConfig.EvtIoDeviceControl = S22UsbCEvtIoDeviceControl;
    Status = WdfIoQueueCreate(Device, &QConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) return Status;

    WDF_TIMER_CONFIG_INIT(&TConfig, S22UsbCEvtPollTimer);
    TConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attrs);
    Attrs.ParentObject   = Device;
    Attrs.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfTimerCreate(&TConfig, &Attrs, &Ctx->PollTimer);
    if (!NT_SUCCESS(Status)) return Status;

    (VOID)WdfDeviceCreateSymbolicLink(Device, &SymLink);
    (VOID)WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_S22_USBC, NULL);
    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, S22UsbCEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
