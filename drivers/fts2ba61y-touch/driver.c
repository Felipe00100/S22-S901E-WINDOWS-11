#include "fts2ba61y.h"

//
// Standard Windows 10/11 Multitouch Digitizer HID Report Descriptor (10 fingers)
//
#define FTS_FINGER_COLLECTION \
    0x09, 0x22,                         /* Usage (Finger) */ \
    0xA1, 0x02,                         /* Collection (Logical) */ \
    0x09, 0x42,                         /*   Usage (Tip Switch) */ \
    0x09, 0x32,                         /*   Usage (In Range) */ \
    0x09, 0x47,                         /*   Usage (Confidence) */ \
    0x15, 0x00,                         /*   Logical Minimum (0) */ \
    0x25, 0x01,                         /*   Logical Maximum (1) */ \
    0x75, 0x01,                         /*   Report Size (1) */ \
    0x95, 0x03,                         /*   Report Count (3) */ \
    0x81, 0x02,                         /*   Input (Data,Var,Abs) */ \
    0x95, 0x05,                         /*   Report Count (5) */ \
    0x81, 0x03,                         /*   Input (Const,Var,Abs) */ \
    0x15, 0x00,                         /*   Logical Minimum (0) */ \
    0x26, 0xFF, 0x00,                   /*   Logical Maximum (255) */ \
    0x75, 0x08,                         /*   Report Size (8) */ \
    0x09, 0x51,                         /*   Usage (Contact Identifier) */ \
    0x95, 0x01,                         /*   Report Count (1) */ \
    0x81, 0x02,                         /*   Input (Data,Var,Abs) */ \
    0x15, 0x00,                         /*   Logical Minimum (0) */ \
    0x25, 0x3F,                         /*   Logical Maximum (63) */ \
    0x75, 0x08,                         /*   Report Size (8) */ \
    0x09, 0x30,                         /*   Usage (Tip Pressure) */ \
    0x95, 0x01,                         /*   Report Count (1) */ \
    0x81, 0x02,                         /*   Input (Data,Var,Abs) */ \
    0x05, 0x01,                         /*   Usage Page (Generic Desktop) */ \
    0x15, 0x00,                         /*   Logical Minimum (0) */ \
    0x26, 0xFF, 0x0F,                   /*   Logical Maximum (4095) */ \
    0x75, 0x10,                         /*   Report Size (16) */ \
    0x55, 0x0E,                         /*   Unit Exponent (-2) */ \
    0x65, 0x11,                         /*   Unit (cm) */ \
    0x09, 0x30,                         /*   Usage (X) */ \
    0x35, 0x00,                         /*   Physical Minimum (0) */ \
    0x46, 0xB8, 0x02,                   /*   Physical Maximum (696 -> 6.96 cm) */ \
    0x81, 0x02,                         /*   Input (Data,Var,Abs) */ \
    0x09, 0x31,                         /*   Usage (Y) */ \
    0x46, 0xDE, 0x05,                   /*   Physical Maximum (1502 -> 15.02 cm) */ \
    0x81, 0x02,                         /*   Input (Data,Var,Abs) */ \
    0x55, 0x00,                         /*   Unit Exponent (0) */ \
    0x65, 0x00,                         /*   Unit (None) */ \
    0x35, 0x00,                         /*   Physical Minimum (0) */ \
    0x45, 0x00,                         /*   Physical Maximum (0) */ \
    0x05, 0x0D,                         /*   Usage Page (Digitizers) */ \
    0xC0                                /* End Collection */

static const UCHAR g_FtsHidReportDescriptor[] = {
    0x05, 0x0D,                         // Usage Page (Digitizers)
    0x09, 0x04,                         // Usage (Touch Screen)
    0xA1, 0x01,                         // Collection (Application)
    0x85, REPORTID_TOUCH,               //   Report ID (1)
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    FTS_FINGER_COLLECTION,
    0x05, 0x0D,                         //   Usage Page (Digitizers)
    0x25, FTS_MAX_CONTACTS,             //   Logical Maximum (10)
    0x75, 0x08,                         //   Report Size (8)
    0x95, 0x01,                         //   Report Count (1)
    0x09, 0x54,                         //   Usage (Contact Count)
    0x81, 0x02,                         //   Input (Data,Var,Abs)
    0x55, 0x0C,                         //   Unit Exponent (-4)
    0x66, 0x01, 0x10,                   //   Unit (Seconds)
    0x47, 0xFF, 0xFF, 0x00, 0x00,       //   Physical Maximum (65535)
    0x27, 0xFF, 0xFF, 0x00, 0x00,       //   Logical Maximum (65535)
    0x75, 0x10,                         //   Report Size (16)
    0x95, 0x01,                         //   Report Count (1)
    0x09, 0x56,                         //   Usage (Scan Time)
    0x81, 0x02,                         //   Input (Data,Var,Abs)
    0x55, 0x00,                         //   Unit Exponent (0)
    0x65, 0x00,                         //   Unit (None)
    0x35, 0x00,                         //   Physical Minimum (0)
    0x45, 0x00,                         //   Physical Maximum (0)
    // Feature: Contact Count Maximum
    0x85, REPORTID_MAX_COUNT,           //   Report ID (2)
    0x09, 0x55,                         //   Usage (Contact Count Maximum)
    0x15, 0x00,                         //   Logical Minimum (0)
    0x25, FTS_MAX_CONTACTS,             //   Logical Maximum (10)
    0x75, 0x08,                         //   Report Size (8)
    0x95, 0x01,                         //   Report Count (1)
    0xB1, 0x02,                         //   Feature (Data,Var,Abs)
    // Feature: Microsoft THQA Certification Blob
    0x85, REPORTID_FEATURE_THQA,        //   Report ID (3)
    0x06, 0x00, 0xFF,                   //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0xC5,                         //   Usage (0xC5)
    0x15, 0x00,                         //   Logical Minimum (0)
    0x26, 0xFF, 0x00,                   //   Logical Maximum (255)
    0x75, 0x08,                         //   Report Size (8)
    0x96, 0x00, 0x01,                   //   Report Count (256)
    0xB1, 0x02,                         //   Feature (Data,Var,Abs)
    0xC0                                // End Collection
};

static const HID_DESCRIPTOR g_FtsHidDescriptor = {
    sizeof(HID_DESCRIPTOR),
    HID_HID_DESCRIPTOR_TYPE,
    0x0100,
    0x00,
    0x01,
    {
        HID_REPORT_DESCRIPTOR_TYPE,
        sizeof(g_FtsHidReportDescriptor)
    }
};

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD FtsEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE FtsEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE FtsEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY FtsEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT FtsEvtDeviceD0Exit;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL FtsEvtIoInternalDeviceControl;
EVT_WDF_TIMER FtsEvtPollTimer;

static VOID
FtsCompletePendingReadReport(
    _In_ PFTS_DEVICE_CONTEXT DevCtx
    )
{
    WDFREQUEST Request;
    NTSTATUS Status;
    PVOID OutBuf;
    SIZE_T OutLen;
    FTS_HID_TOUCH_REPORT Report;

    Status = WdfIoQueueRetrieveNextRequest(DevCtx->ReadReportQueue, &Request);
    if (!NT_SUCCESS(Status)) {
        return;
    }

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(Report), &OutBuf, &OutLen);
    if (NT_SUCCESS(Status)) {
        FtsBuildHidReport(DevCtx, &Report);
        RtlCopyMemory(OutBuf, &Report, sizeof(Report));
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(Report));
    } else {
        WdfRequestComplete(Request, Status);
    }
}

VOID
FtsEvtPollTimer(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PFTS_DEVICE_CONTEXT DevCtx = FtsGetDeviceContext(Device);
    BOOLEAN HasUpdate = FALSE;

    if (DevCtx->VirtualMode ||
        InterlockedCompareExchange(&DevCtx->SensingEnabled, 0, 0) == 0)
    {
        return;
    }

    if (NT_SUCCESS(FtsPollEvents(DevCtx, &HasUpdate)) && HasUpdate) {
        FtsCompletePendingReadReport(DevCtx);
    }

    if (InterlockedCompareExchange(&DevCtx->SensingEnabled, 0, 0) != 0) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(4));
    }
}

VOID
FtsEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PFTS_DEVICE_CONTEXT DevCtx = FtsGetDeviceContext(Device);
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG_PTR BytesReturned = 0;
    WDFMEMORY Memory = NULL;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
        if (NT_SUCCESS(Status)) {
            Status = WdfMemoryCopyFromBuffer(Memory, 0, (PVOID)&g_FtsHidDescriptor, sizeof(g_FtsHidDescriptor));
            if (NT_SUCCESS(Status)) {
                BytesReturned = sizeof(g_FtsHidDescriptor);
            }
        }
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
        if (NT_SUCCESS(Status)) {
            Status = WdfMemoryCopyFromBuffer(Memory, 0, (PVOID)g_FtsHidReportDescriptor, sizeof(g_FtsHidReportDescriptor));
            if (NT_SUCCESS(Status)) {
                BytesReturned = sizeof(g_FtsHidReportDescriptor);
            }
        }
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
    {
        HID_DEVICE_ATTRIBUTES Attrs;
        RtlZeroMemory(&Attrs, sizeof(Attrs));
        Attrs.Size          = sizeof(HID_DEVICE_ATTRIBUTES);
        Attrs.VendorID      = FTS_HID_VID;
        Attrs.ProductID     = FTS_HID_PID;
        Attrs.VersionNumber = FTS_HID_VERSION;

        Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
        if (NT_SUCCESS(Status)) {
            Status = WdfMemoryCopyFromBuffer(Memory, 0, &Attrs, sizeof(Attrs));
            if (NT_SUCCESS(Status)) {
                BytesReturned = sizeof(Attrs);
            }
        }
        break;
    }

    case IOCTL_HID_READ_REPORT:
        Status = WdfRequestForwardToIoQueue(Request, DevCtx->ReadReportQueue);
        if (NT_SUCCESS(Status)) {
            if (DevCtx->ReportDirty) {
                FtsCompletePendingReadReport(DevCtx);
            }
            return; // Request is now owned by ReadReportQueue
        }
        break;

    case IOCTL_HID_GET_FEATURE:
    {
        PHID_XFER_PACKET Packet = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
        if (Packet == NULL || Packet->reportBuffer == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (Packet->reportId == REPORTID_MAX_COUNT &&
            Packet->reportBufferLen >= sizeof(FTS_HID_MAX_COUNT_REPORT))
        {
            PFTS_HID_MAX_COUNT_REPORT Rep = (PFTS_HID_MAX_COUNT_REPORT)Packet->reportBuffer;
            Rep->ReportId = REPORTID_MAX_COUNT;
            Rep->MaxCount = FTS_MAX_CONTACTS;
            BytesReturned = sizeof(FTS_HID_MAX_COUNT_REPORT);
        } else if (Packet->reportId == REPORTID_FEATURE_THQA &&
                   Packet->reportBufferLen >= sizeof(FTS_HID_THQA_REPORT))
        {
            PFTS_HID_THQA_REPORT Rep = (PFTS_HID_THQA_REPORT)Packet->reportBuffer;
            RtlZeroMemory(Rep, sizeof(*Rep));
            Rep->ReportId = REPORTID_FEATURE_THQA;
            BytesReturned = sizeof(FTS_HID_THQA_REPORT);
        } else {
            Status = STATUS_INVALID_DEVICE_REQUEST;
        }
        break;
    }

    case IOCTL_HID_GET_STRING:
    {
        WDF_REQUEST_PARAMETERS Params;
        ULONG StringId;
        const WCHAR *Str = NULL;
        SIZE_T StrBytes = 0;

        WDF_REQUEST_PARAMETERS_INIT(&Params);
        WdfRequestGetParameters(Request, &Params);
        StringId = (ULONG)(ULONG_PTR)Params.Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF;

        switch (StringId) {
        case HID_STRING_ID_IMANUFACTURER:
            Str = L"STMicroelectronics";
            break;
        case HID_STRING_ID_IPRODUCT:
            Str = L"FTS2BA61Y Touchscreen (Galaxy S22 r0q)";
            break;
        case HID_STRING_ID_ISERIALNUMBER:
            Str = L"S901E-FTS2BA61Y-0";
            break;
        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (Str != NULL) {
            StrBytes = (wcslen(Str) + 1) * sizeof(WCHAR);
            Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
            if (NT_SUCCESS(Status)) {
                Status = WdfMemoryCopyFromBuffer(Memory, 0, (PVOID)Str, StrBytes);
                if (NT_SUCCESS(Status)) {
                    BytesReturned = StrBytes;
                }
            }
        }
        break;
    }

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        Status = STATUS_SUCCESS;
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestCompleteWithInformation(Request, Status, BytesReturned);
}

static BOOLEAN
FtsIsRootEnumeratedDevice(
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

NTSTATUS
FtsEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PFTS_DEVICE_CONTEXT DevCtx = FtsGetDeviceContext(Device);
    ULONG Count = WdfCmResourceListGetCount(ResourcesTranslated);
    ULONG i;
    ULONG MemCount = 0;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    DevCtx->SpiGpioBase   = NULL;
    DevCtx->SpiGpioLen    = 0;
    DevCtx->IrqGpioBase   = NULL;
    DevCtx->IrqGpioLen    = 0;
    DevCtx->VirtualMode   = FALSE;
    DevCtx->HardwareReady = FALSE;

    for (i = 0; i < Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (Desc->Type == CmResourceTypeMemory) {
            ULONGLONG PhysStart = (ULONGLONG)Desc->u.Memory.Start.QuadPart;
            SIZE_T    Len       = (SIZE_T)Desc->u.Memory.Length;
            BOOLEAN   IsSpi     = FALSE;
            BOOLEAN   IsIrq     = FALSE;
            PVOID     Mapped;

            MemCount++;

            // Identify SPI (4 pages, GPIO 28..31) vs IRQ (1 page, GPIO 46) explicitly
            // by physical address or size rather than assuming ACPI _CRS ordering.
            if (PhysStart == TLMM_SPI_PHYS_BASE) {
                IsSpi = (Len >= 4 * TLMM_GPIO_STRIDE);
            } else if (PhysStart == TLMM_IRQ_PHYS_BASE) {
                IsIrq = (Len >= TLMM_GPIO_STRIDE);
            } else if (Len >= 4 * TLMM_GPIO_STRIDE && DevCtx->SpiGpioBase == NULL) {
                IsSpi = TRUE;
            } else if (Len >= TLMM_GPIO_STRIDE && DevCtx->IrqGpioBase == NULL) {
                IsIrq = TRUE;
            }

            if (!IsSpi && !IsIrq) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                    "FTS2BA61Y: Ignoring unknown/extra MMIO resource [%lu] base=0x%llx len=0x%Ix\n",
                    i, PhysStart, Len));
                continue;
            }

            if ((IsSpi && DevCtx->SpiGpioBase != NULL) ||
                (IsIrq && DevCtx->IrqGpioBase != NULL))
            {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                    "FTS2BA61Y: Duplicate %s MMIO resource base=0x%llx len=0x%Ix\n",
                    IsSpi ? "SPI" : "IRQ", PhysStart, Len));
                goto CleanupConfigError;
            }

            Mapped = MmMapIoSpaceEx(
                Desc->u.Memory.Start,
                Desc->u.Memory.Length,
                PAGE_READWRITE | PAGE_NOCACHE);
            if (Mapped == NULL) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                    "FTS2BA61Y: MmMapIoSpaceEx failed for base=0x%llx len=0x%Ix\n",
                    PhysStart, Len));
                goto CleanupInsufficientResources;
            }

            if (IsSpi) {
                DevCtx->SpiGpioBase = (volatile UCHAR *)Mapped;
                DevCtx->SpiGpioLen  = Len;
            } else {
                DevCtx->IrqGpioBase = (volatile UCHAR *)Mapped;
                DevCtx->IrqGpioLen  = Len;
            }
        }
    }

    if (MemCount == 0) {
        if (FtsIsRootEnumeratedDevice(Device)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                "FTS2BA61Y: ROOT-enumerated device with 0 MMIO resources; entering VirtualMode\n"));
            DevCtx->VirtualMode = TRUE;
        } else {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "FTS2BA61Y: Missing ACPI MMIO resources on non-ROOT device (MemCount=0)\n"));
            goto CleanupConfigError;
        }
    } else if (DevCtx->SpiGpioBase != NULL && DevCtx->IrqGpioBase != NULL) {
        DevCtx->VirtualMode = FALSE;
    } else {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "FTS2BA61Y: Incomplete MMIO resources (SPI=%p IRQ=%p MemCount=%lu)\n",
            DevCtx->SpiGpioBase, DevCtx->IrqGpioBase, MemCount));
        goto CleanupConfigError;
    }

    return FtsHardwareInit(DevCtx);

CleanupInsufficientResources:
    if (DevCtx->SpiGpioBase != NULL) {
        MmUnmapIoSpace((PVOID)DevCtx->SpiGpioBase, DevCtx->SpiGpioLen);
        DevCtx->SpiGpioBase = NULL;
        DevCtx->SpiGpioLen  = 0;
    }
    if (DevCtx->IrqGpioBase != NULL) {
        MmUnmapIoSpace((PVOID)DevCtx->IrqGpioBase, DevCtx->IrqGpioLen);
        DevCtx->IrqGpioBase = NULL;
        DevCtx->IrqGpioLen  = 0;
    }
    return STATUS_INSUFFICIENT_RESOURCES;

CleanupConfigError:
    if (DevCtx->SpiGpioBase != NULL) {
        MmUnmapIoSpace((PVOID)DevCtx->SpiGpioBase, DevCtx->SpiGpioLen);
        DevCtx->SpiGpioBase = NULL;
        DevCtx->SpiGpioLen  = 0;
    }
    if (DevCtx->IrqGpioBase != NULL) {
        MmUnmapIoSpace((PVOID)DevCtx->IrqGpioBase, DevCtx->IrqGpioLen);
        DevCtx->IrqGpioBase = NULL;
        DevCtx->IrqGpioLen  = 0;
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

NTSTATUS
FtsEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PFTS_DEVICE_CONTEXT DevCtx = FtsGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (DevCtx->SpiGpioBase != NULL) {
        MmUnmapIoSpace((PVOID)DevCtx->SpiGpioBase, DevCtx->SpiGpioLen);
        DevCtx->SpiGpioBase = NULL;
    }

    if (DevCtx->IrqGpioBase != NULL) {
        MmUnmapIoSpace((PVOID)DevCtx->IrqGpioBase, DevCtx->IrqGpioLen);
        DevCtx->IrqGpioBase = NULL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
FtsEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PFTS_DEVICE_CONTEXT DevCtx = FtsGetDeviceContext(Device);
    NTSTATUS Status;

    if (!DevCtx->VirtualMode) {
        // On resume from sleep/Dx (or if hardware was not ready), re-configure TLMM
        // SPI/IRQ pins and reset/re-initialize the FTS2BA61Y controller in case rails dropped.
        if (PreviousState != WdfPowerDeviceD3Final || !DevCtx->HardwareReady) {
            WdfSpinLockAcquire(DevCtx->StateLock);
            RtlZeroMemory(DevCtx->Slots, sizeof(DevCtx->Slots));
            DevCtx->ReportDirty = FALSE;
            WdfSpinLockRelease(DevCtx->StateLock);

            Status = FtsHardwareInit(DevCtx);
            if (!NT_SUCCESS(Status)) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                    "FTS2BA61Y: D0Entry re-init failed after state %d (0x%08X)\n",
                    PreviousState, Status));
                return Status;
            }
        }
    }

    Status = FtsSetSensing(DevCtx, TRUE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (!DevCtx->VirtualMode && DevCtx->PollTimer != NULL) {
        WdfTimerStart(DevCtx->PollTimer, WDF_REL_TIMEOUT_IN_MS(4));
    }

    return STATUS_SUCCESS;
}

NTSTATUS
FtsEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PFTS_DEVICE_CONTEXT DevCtx = FtsGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    // 1. Disarm SensingEnabled FIRST so an in-flight PollTimer callback cannot re-arm itself.
    InterlockedExchange(&DevCtx->SensingEnabled, 0);

    // 2. Wait for any running PollTimer callback to drain completely.
    if (DevCtx->PollTimer != NULL) {
        WdfTimerStop(DevCtx->PollTimer, TRUE);
    }

    // 3. Send SENSE_OFF now that the timer is guaranteed stopped.
    (VOID)FtsSetSensing(DevCtx, FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS
FtsEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS Status;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_TIMER_CONFIG TimerConfig;
    WDFDEVICE Device;
    PFTS_DEVICE_CONTEXT DevCtx;

    UNREFERENCED_PARAMETER(Driver);

    // Lower filter beneath mshidkmdf.sys
    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = FtsEvtDevicePrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = FtsEvtDeviceReleaseHardware;
    PnpCallbacks.EvtDeviceD0Entry         = FtsEvtDeviceD0Entry;
    PnpCallbacks.EvtDeviceD0Exit          = FtsEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, FTS_DEVICE_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    DevCtx = FtsGetDeviceContext(Device);
    DevCtx->Device      = Device;
    DevCtx->VirtualMode = TRUE;

    Status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &DevCtx->StateLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &DevCtx->SpiLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    // Default parallel queue for HID internal IOCTLs
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = FtsEvtIoInternalDeviceControl;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &DevCtx->IoQueue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    // Manual queue holding pending IOCTL_HID_READ_REPORT requests
    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &DevCtx->ReadReportQueue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    // 4ms (~250 Hz) passive-level one-shot timer (KMDF requires Period=0 for passive timers)
    WDF_TIMER_CONFIG_INIT(&TimerConfig, FtsEvtPollTimer);
    TimerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
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
    WDF_DRIVER_CONFIG_INIT(&Config, FtsEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
