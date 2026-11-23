#include "max77705batt.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Max77705EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Max77705EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Max77705EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY Max77705EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT Max77705EvtD0Exit;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS Max77705EvtWdmDeviceControl;
EVT_WDF_TIMER Max77705EvtPollTimer;

static ULONG
BuildPowerStateFlags(
    _In_ const MAX77705_TELEMETRY *Tel
    )
{
    ULONG Flags = 0;

    if (Tel->UsbOnline) {
        Flags |= BATTERY_POWER_ON_LINE;
        if (Tel->Charging && Tel->SocPercent < 100) {
            Flags |= BATTERY_CHARGING;
        }
    } else {
        Flags |= BATTERY_DISCHARGING;
    }

    if (!Tel->UsbOnline && Tel->SocPercent <= 5) {
        Flags |= BATTERY_CRITICAL;
    }

    return Flags;
}

NTSTATUS
Max77705QueryTag(
    _In_ PVOID Context,
    _Out_ PULONG BatteryTag
    )
{
    PMAX77705_DEVICE_CONTEXT DevCtx = (PMAX77705_DEVICE_CONTEXT)Context;
    *BatteryTag = DevCtx->BatteryTag;
    return STATUS_SUCCESS;
}

NTSTATUS
Max77705QueryInformation(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ BATTERY_QUERY_INFORMATION_LEVEL Level,
    _In_ LONG AtRate,
    _Out_writes_bytes_to_(BufferLength, *ReturnedLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG ReturnedLength
    )
{
    PMAX77705_DEVICE_CONTEXT DevCtx = (PMAX77705_DEVICE_CONTEXT)Context;
    MAX77705_TELEMETRY Tel;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(AtRate);

    if (BatteryTag != DevCtx->BatteryTag) {
        return STATUS_NO_SUCH_DEVICE;
    }

    *ReturnedLength = 0;

    if (!NT_SUCCESS(Max77705ReadTelemetry(DevCtx, &Tel))) {
        Tel = DevCtx->LastTelemetry;
    } else {
        DevCtx->LastTelemetry = Tel;
    }

    switch (Level) {
    case BatteryInformation:
        if (BufferLength < sizeof(BATTERY_INFORMATION)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        {
            PBATTERY_INFORMATION Info = (PBATTERY_INFORMATION)Buffer;
            RtlZeroMemory(Info, sizeof(*Info));
            Info->Capabilities        = BATTERY_SYSTEM_BATTERY;
            Info->Technology          = 1; // Rechargeable Li-Ion
            Info->Chemistry[0]        = 'L';
            Info->Chemistry[1]        = 'I';
            Info->Chemistry[2]        = 'O';
            Info->Chemistry[3]        = 'N';
            Info->DesignedCapacity    = S22_DESIGN_CAPACITY_MWH;
            Info->FullChargedCapacity = (Tel.FullCapMwh > 0) ? Tel.FullCapMwh : S22_DESIGN_CAPACITY_MWH;
            Info->DefaultAlert1       = S22_ALERT_WARN_MWH;
            Info->DefaultAlert2       = S22_ALERT_LOW_MWH;
            Info->CriticalBias        = S22_CRITICAL_BIAS_MWH;
            Info->CycleCount          = Tel.CycleCount;
            *ReturnedLength           = sizeof(BATTERY_INFORMATION);
        }
        break;

    case BatteryTemperature:
        if (BufferLength < sizeof(ULONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)Buffer = (ULONG)((Tel.TempTenthsK > 0) ? Tel.TempTenthsK : 3000);
        *ReturnedLength = sizeof(ULONG);
        break;

    case BatteryEstimatedTime:
        if (BufferLength < sizeof(ULONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)Buffer = BATTERY_UNKNOWN_TIME;
        *ReturnedLength = sizeof(ULONG);
        break;

    case BatteryDeviceName:
    {
        const WCHAR Name[] = L"MAX77705 Galaxy S22";
        if (BufferLength < sizeof(Name)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlCopyMemory(Buffer, Name, sizeof(Name));
        *ReturnedLength = sizeof(Name);
        break;
    }

    case BatteryManufactureName:
    {
        const WCHAR Mfg[] = L"Samsung / Maxim";
        if (BufferLength < sizeof(Mfg)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlCopyMemory(Buffer, Mfg, sizeof(Mfg));
        *ReturnedLength = sizeof(Mfg);
        break;
    }

    case BatteryUniqueID:
    {
        const WCHAR Uid[] = L"S901E-MAX77705-BATT0";
        if (BufferLength < sizeof(Uid)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlCopyMemory(Buffer, Uid, sizeof(Uid));
        *ReturnedLength = sizeof(Uid);
        break;
    }

    default:
        Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return Status;
}

NTSTATUS
Max77705SetInformation(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ BATTERY_SET_INFORMATION_LEVEL Level,
    _In_opt_ PVOID Buffer
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Buffer);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
Max77705QueryStatus(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _Out_ PBATTERY_STATUS BatteryStatus
    )
{
    PMAX77705_DEVICE_CONTEXT DevCtx = (PMAX77705_DEVICE_CONTEXT)Context;
    MAX77705_TELEMETRY Tel;

    if (BatteryTag != DevCtx->BatteryTag) {
        return STATUS_NO_SUCH_DEVICE;
    }

    if (!NT_SUCCESS(Max77705ReadTelemetry(DevCtx, &Tel))) {
        Tel = DevCtx->LastTelemetry;
    } else {
        DevCtx->LastTelemetry = Tel;
    }

    RtlZeroMemory(BatteryStatus, sizeof(*BatteryStatus));
    BatteryStatus->PowerState = BuildPowerStateFlags(&Tel);
    BatteryStatus->Capacity   = Tel.RemCapMwh;
    BatteryStatus->Voltage    = (Tel.VoltageMv > 0) ? Tel.VoltageMv : BATTERY_UNKNOWN_VOLTAGE;
    BatteryStatus->Rate       = Tel.PowerMw; // Signed mW (+ charging, - discharging)

    DevCtx->LastPowerState = BatteryStatus->PowerState;
    return STATUS_SUCCESS;
}

NTSTATUS
Max77705SetStatusNotify(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ PBATTERY_NOTIFY BatteryNotify
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(BatteryNotify);
    return STATUS_SUCCESS;
}

NTSTATUS
Max77705DisableStatusNotify(
    _In_ PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
    return STATUS_SUCCESS;
}

VOID
Max77705EvtPollTimer(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PMAX77705_DEVICE_CONTEXT DevCtx = Max77705GetContext(Device);
    MAX77705_TELEMETRY Tel;

    if (DevCtx->ClassHandle == NULL) {
        return;
    }

    if (NT_SUCCESS(Max77705ReadTelemetry(DevCtx, &Tel))) {
        ULONG NewState = BuildPowerStateFlags(&Tel);
        BOOLEAN Changed = (NewState != DevCtx->LastPowerState) ||
                          (Tel.SocPercent != DevCtx->LastTelemetry.SocPercent);

        DevCtx->LastTelemetry  = Tel;
        DevCtx->LastPowerState = NewState;

        if (Changed) {
            BatteryClassStatusNotify(DevCtx->ClassHandle);
        }
    }

    WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_SEC(5));
}

//
// Official KMDF pattern (per Microsoft simbatt): intercept IRP_MJ_DEVICE_CONTROL
// before WDF sees it, pass to BatteryClassIoctl, and only forward to WDF when
// BatteryClassIoctl returns STATUS_NOT_SUPPORTED (prevents double-completion BSOD).
//
NTSTATUS
Max77705EvtWdmDeviceControl(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp
    )
{
    PMAX77705_DEVICE_CONTEXT DevCtx = Max77705GetContext(Device);
    NTSTATUS Status;

    if (DevCtx->ClassHandle != NULL) {
        Status = BatteryClassIoctl(DevCtx->ClassHandle, Irp);
        if (Status != STATUS_NOT_SUPPORTED) {
            // battc.sys already completed Irp; do NOT touch Irp again
            return Status;
        }
    }

    // Do NOT call IoSkipCurrentIrpStackLocation before WdfDeviceWdmDispatchPreprocessedIrp!
    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

NTSTATUS
Max77705EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PMAX77705_DEVICE_CONTEXT DevCtx = Max77705GetContext(Device);
    BATTERY_MINIPORT_INFO_V1_1 BattInfo;
    ULONG Count = WdfCmResourceListGetCount(ResourcesTranslated);
    ULONG i;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    DevCtx->I2cGpioBase = NULL;
    DevCtx->VirtualMode = TRUE;

    for (i = 0; i < Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (Desc->Type == CmResourceTypeMemory && DevCtx->I2cGpioBase == NULL) {
            PVOID Mapped = MmMapIoSpaceEx(
                Desc->u.Memory.Start,
                Desc->u.Memory.Length,
                PAGE_READWRITE | PAGE_NOCACHE);
            if (Mapped == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            DevCtx->I2cGpioBase = (volatile UCHAR *)Mapped;
            DevCtx->I2cGpioLen  = Desc->u.Memory.Length;
            DevCtx->VirtualMode = FALSE;
        }
    }

    TlmmI2cInitPins(DevCtx);
    Max77705InitHardwareQuirks(DevCtx);
    (VOID)Max77705ReadTelemetry(DevCtx, &DevCtx->LastTelemetry);

    if (DevCtx->ClassHandle == NULL) {
        RtlZeroMemory(&BattInfo, sizeof(BattInfo));
        BattInfo.MajorVersion        = BATTERY_CLASS_MAJOR_VERSION;
        BattInfo.MinorVersion        = BATTERY_CLASS_MINOR_VERSION_1;
        BattInfo.Context             = DevCtx;
        BattInfo.QueryTag            = Max77705QueryTag;
        BattInfo.QueryInformation    = Max77705QueryInformation;
        BattInfo.SetInformation      = Max77705SetInformation;
        BattInfo.QueryStatus         = Max77705QueryStatus;
        BattInfo.SetStatusNotify     = Max77705SetStatusNotify;
        BattInfo.DisableStatusNotify = Max77705DisableStatusNotify;
        BattInfo.Pdo                 = WdfDeviceWdmGetPhysicalDevice(Device);
        BattInfo.DeviceName          = NULL;
        BattInfo.Fdo                 = WdfDeviceWdmGetDeviceObject(Device);

        Status = BatteryClassInitializeDevice((PBATTERY_MINIPORT_INFO)&BattInfo, &DevCtx->ClassHandle);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Max77705EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PMAX77705_DEVICE_CONTEXT DevCtx = Max77705GetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (DevCtx->ClassHandle != NULL) {
        BatteryClassUnload(DevCtx->ClassHandle);
        DevCtx->ClassHandle = NULL;
    }

    if (DevCtx->I2cGpioBase != NULL) {
        MmUnmapIoSpace((PVOID)DevCtx->I2cGpioBase, DevCtx->I2cGpioLen);
        DevCtx->I2cGpioBase = NULL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Max77705EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PMAX77705_DEVICE_CONTEXT DevCtx = Max77705GetContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    if (DevCtx->PollTimer != NULL) {
        WdfTimerStart(DevCtx->PollTimer, WDF_REL_TIMEOUT_IN_SEC(5));
    }
    return STATUS_SUCCESS;
}

NTSTATUS
Max77705EvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PMAX77705_DEVICE_CONTEXT DevCtx = Max77705GetContext(Device);
    UNREFERENCED_PARAMETER(TargetState);

    if (DevCtx->PollTimer != NULL) {
        WdfTimerStop(DevCtx->PollTimer, TRUE);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
Max77705EvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS Status;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_TIMER_CONFIG TimerConfig;
    WDFDEVICE Device;
    PMAX77705_DEVICE_CONTEXT DevCtx;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = Max77705EvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = Max77705EvtReleaseHardware;
    PnpCallbacks.EvtDeviceD0Entry         = Max77705EvtD0Entry;
    PnpCallbacks.EvtDeviceD0Exit          = Max77705EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        Max77705EvtWdmDeviceControl,
        IRP_MJ_DEVICE_CONTROL,
        NULL,
        0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, MAX77705_DEVICE_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    DevCtx = Max77705GetContext(Device);
    DevCtx->Device                    = Device;
    DevCtx->BatteryTag                = 1;
    DevCtx->VirtualMode               = TRUE;
    DevCtx->ChargeControlEndThreshold = 100;

    // Read optional ChargeControlEndThreshold (50..100%) from driver Parameters key
    {
        WDFKEY ParamsKey = NULL;
        DECLARE_CONST_UNICODE_STRING(ValName, L"ChargeControlEndThreshold");
        ULONG RegLimit = 100;
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &ParamsKey))) {
            if (NT_SUCCESS(WdfRegistryQueryULong(ParamsKey, &ValName, &RegLimit))) {
                if (RegLimit >= 50 && RegLimit <= 100) {
                    DevCtx->ChargeControlEndThreshold = RegLimit;
                }
            }
            WdfRegistryClose(ParamsKey);
        }
    }

    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &DevCtx->I2cLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    // Passive-level one-shot timer (KMDF requires Period=0 for WdfExecutionLevelPassive)
    WDF_TIMER_CONFIG_INIT(&TimerConfig, Max77705EvtPollTimer);
    TimerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject   = Device;
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfTimerCreate(&TimerConfig, &Attributes, &DevCtx->PollTimer);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, Max77705EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
