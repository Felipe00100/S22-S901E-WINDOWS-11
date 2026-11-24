/* SPDX-License-Identifier: MIT */
/*
 * adreno730_wddm.sys (staged) - render-only WDDM miniport for Adreno 730 on S22.
 */
#define DXGKDDI_INTERFACE_VERSION 0x5023 /* DXGKDDI_INTERFACE_VERSION_WDDM2_0 */

#include <ntddk.h>
#include <windef.h>
#include <winerror.h>
#include <wingdi.h>
#include <ntddvdeo.h>
#include <d3dkmddi.h>
#include <d3dkmthk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>

#include "adreno/adreno_gpu.h"
#include "adreno/adreno_pm4.h"
#include "adreno/adreno_wddm.h"
#include "adreno730_hw.h"
#include "adreno_wddm_escape.h"

#define ADRENO_WDDM_TAG 'D730'
#define ADRENO_WDDM_MAX_BYTES (256ull * 1024ull * 1024ull)
#define ADRENO_WDDM_MAX_HWQUEUES 8
#define ADRENO_WDDM_HWQUEUE_MAGIC 0x48513733u /* "HQ73" */

#define WLog(...) AdrenoWddmLogLine("adreno730_wddm", __VA_ARGS__)

typedef struct ADRENO_WDDM_HOST_ALLOC {
    void *host;
    uint32_t size;
} ADRENO_WDDM_HOST_ALLOC;

typedef struct ADRENO_WDDM_DMA_MAP {
    PHYSICAL_ADDRESS pa;
    void *host;
    uint32_t size;
} ADRENO_WDDM_DMA_MAP;

typedef struct ADRENO_WDDM_HWQUEUE ADRENO_WDDM_HWQUEUE;

typedef struct ADRENO_WDDM_ADAPTER {
    PDEVICE_OBJECT pdo;
    DXGKRNL_INTERFACE dxgk;
    HANDLE device_handle;
    adreno_wddm wddm;
    ADRENO_WDDM_HW hw;
    ADRENO_WDDM_HOST_ALLOC host_allocs[ADRENO_WDDM_MAX_ALLOCS];
    ADRENO_WDDM_DMA_MAP dma_maps[32];
    ADRENO_WDDM_HWQUEUE *hw_queues[ADRENO_WDDM_MAX_HWQUEUES];
    KSPIN_LOCK hw_queue_lock;
    KMUTEX hw_submit_lock;
    SIZE_T allocated_bytes;
    volatile LONG pending_fence;
    uint64_t *aperture_pa;
    uint32_t aperture_pages;
    BOOLEAN hw_ready;
    BOOLEAN wddm_ready;
    BOOLEAN started;
} ADRENO_WDDM_ADAPTER, *PADRENO_WDDM_ADAPTER;

typedef struct ADRENO_WDDM_DEVICE {
    PADRENO_WDDM_ADAPTER adapter;
    HANDLE hDevice;
} ADRENO_WDDM_DEVICE, *PADRENO_WDDM_DEVICE;

typedef struct ADRENO_WDDM_CONTEXT {
    PADRENO_WDDM_ADAPTER adapter;
    HANDLE hContext;
} ADRENO_WDDM_CONTEXT, *PADRENO_WDDM_CONTEXT;

struct ADRENO_WDDM_HWQUEUE {
    uint32_t magic;
    PADRENO_WDDM_ADAPTER adapter;
    HANDLE h_context;
    D3DKMT_HANDLE progress_fence;
    volatile UINT64 *fence_cpu;
    D3DGPU_VIRTUAL_ADDRESS fence_gpu;
    UINT64 last_fence_id;
};

typedef ADRENO_WDDM_HWQUEUE *PADRENO_WDDM_HWQUEUE;

static ULONG g_wddm_version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;

/* ---- Allocation & Log callbacks ---------------------------------------- */

static bool WddmAllocContig(void *ctx, uint32_t size, void **host,
                            uint64_t *pa, uint32_t *backend_handle)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)ctx;
    PHYSICAL_ADDRESS low, high, boundary;
    void *memory;
    ULONG i;

    if (!a || !host || !pa || size == 0)
        return false;
    *backend_handle = 0;
    if (a->allocated_bytes + size > ADRENO_WDDM_MAX_BYTES)
        return false;
    for (i = 0; i < ADRENO_WDDM_MAX_ALLOCS; i++) {
        if (!a->host_allocs[i].host)
            break;
    }
    if (i == ADRENO_WDDM_MAX_ALLOCS)
        return false;
    low.QuadPart = 0;
    high.QuadPart = 0xffffffffull;
    boundary.QuadPart = 0;
    memory = MmAllocateContiguousMemorySpecifyCacheNode(
        size, low, high, boundary, MmWriteCombined, MM_ANY_NODE_OK);
    if (!memory) {
        high.QuadPart = 0xffffffffffffull;
        memory = MmAllocateContiguousMemorySpecifyCacheNode(
            size, low, high, boundary, MmWriteCombined, MM_ANY_NODE_OK);
    }
    if (!memory) {
        WLog("contiguous alloc failed (%u bytes)", size);
        return false;
    }
    RtlZeroMemory(memory, size);
    a->host_allocs[i].host = memory;
    a->host_allocs[i].size = size;
    a->allocated_bytes += size;
    *host = memory;
    *pa = MmGetPhysicalAddress(memory).QuadPart;
    return true;
}

static void WddmFreeContig(void *ctx, void *host)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)ctx;
    ULONG i;

    if (!host)
        return;
    MmFreeContiguousMemory(host);
    if (!a)
        return;
    for (i = 0; i < ADRENO_WDDM_MAX_ALLOCS; i++) {
        if (a->host_allocs[i].host == host) {
            if (a->allocated_bytes >= a->host_allocs[i].size)
                a->allocated_bytes -= a->host_allocs[i].size;
            RtlZeroMemory(&a->host_allocs[i], sizeof(a->host_allocs[i]));
            break;
        }
    }
}

static void WddmLog(void *ctx, const char *message)
{
    (void)ctx;
    if (message)
        WLog("%s", message);
}

/* ---- PnP --------------------------------------------------------------- */

static NTSTATUS APIENTRY WddmAddDevice(PDEVICE_OBJECT pdo, PVOID *out)
{
    PADRENO_WDDM_ADAPTER a;

    if (!out)
        return STATUS_INVALID_PARAMETER;
    a = (PADRENO_WDDM_ADAPTER)ExAllocatePoolZero(NonPagedPoolNx, sizeof(*a),
                                                 ADRENO_WDDM_TAG);
    if (!a)
        return STATUS_INSUFFICIENT_RESOURCES;
    a->pdo = pdo;
    KeInitializeSpinLock(&a->hw_queue_lock);
    KeInitializeMutex(&a->hw_submit_lock, 0);
    *out = a;
    WLog("AddDevice pdo=%p adapter=%p", pdo, a);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmStartDevice(PVOID context, PVOID start_info,
                                         PVOID interface, PULONG sources,
                                         PULONG children)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    PDXGKRNL_INTERFACE dxgk = (PDXGKRNL_INTERFACE)interface;
    adreno_wddm_ops ops;
    enum adreno_status st;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(start_info);
    if (!a || !sources || !children || !dxgk)
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(&a->dxgk, dxgk, sizeof(a->dxgk));
    a->device_handle = dxgk->DeviceHandle;

    RtlZeroMemory(&ops, sizeof(ops));
    ops.ctx = a;
    ops.alloc = WddmAllocContig;
    ops.free = WddmFreeContig;
    ops.log = WddmLog;
    st = adreno_wddm_init(&a->wddm, &ops);
    if (st != ADRENO_OK)
        return STATUS_INSUFFICIENT_RESOURCES;
    a->wddm_ready = TRUE;
    a->aperture_pages = (512u * 1024u * 1024u) / 4096u;
    a->aperture_pa = (uint64_t *)ExAllocatePoolZero(
        NonPagedPoolNx, (SIZE_T)a->aperture_pages * sizeof(uint64_t), ADRENO_WDDM_TAG);

    status = AdrenoWddmHwStart(&a->hw, a->device_handle, &a->dxgk, TRUE);
    if (NT_SUCCESS(status)) {
        a->hw_ready = TRUE;
        WLog("A730 hardware booted");
    } else {
        a->hw_ready = FALSE;
        WLog("A730 hardware not started (0x%x); reference CP remains active",
             status);
        AdrenoWddmHwStop(&a->hw);
    }
    a->started = TRUE;

    *sources = 0;
    *children = 0;
    WLog("StartDevice complete (render-only, hw_ready=%u, wddm_ver=0x%x)",
         a->hw_ready ? 1u : 0u, g_wddm_version);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmStopDevice(PVOID context)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    WLog("StopDevice adapter=%p", a);
    if (a) {
        if (a->hw_ready) {
            AdrenoWddmHwStop(&a->hw);
            a->hw_ready = FALSE;
        }
        if (a->wddm_ready) {
            adreno_wddm_fini(&a->wddm);
            a->wddm_ready = FALSE;
            a->allocated_bytes = 0;
        }
        if (a->aperture_pa) {
            ExFreePoolWithTag(a->aperture_pa, ADRENO_WDDM_TAG);
            a->aperture_pa = NULL;
            a->aperture_pages = 0;
        }
        a->started = FALSE;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmRemoveDevice(PVOID context)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    WLog("RemoveDevice adapter=%p", a);
    if (a) {
        if (a->hw_ready) {
            AdrenoWddmHwStop(&a->hw);
            a->hw_ready = FALSE;
        }
        if (a->wddm_ready) {
            adreno_wddm_fini(&a->wddm);
            a->wddm_ready = FALSE;
        }
        if (a->aperture_pa) {
            ExFreePoolWithTag(a->aperture_pa, ADRENO_WDDM_TAG);
            a->aperture_pa = NULL;
            a->aperture_pages = 0;
        }
        ExFreePoolWithTag(a, ADRENO_WDDM_TAG);
    }
    return STATUS_SUCCESS;
}

static VOID APIENTRY WddmUnload(VOID)
{
    WLog("Unload");
}

static VOID APIENTRY WddmResetDevice(PVOID context)
{
    UNREFERENCED_PARAMETER(context);
}

static NTSTATUS APIENTRY WddmDispatchIoRequest(PVOID context, ULONG ulVidPnSourceId,
                                               PVIDEO_REQUEST_PACKET pPacket)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(ulVidPnSourceId);
    UNREFERENCED_PARAMETER(pPacket);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY WddmQueryChildRelations(PVOID context,
                                                 PDXGK_CHILD_DESCRIPTOR relations,
                                                 ULONG size)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(relations);
    UNREFERENCED_PARAMETER(size);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmQueryChildStatus(PVOID context,
                                              PDXGK_CHILD_STATUS child_status,
                                              BOOLEAN non_destructive)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(child_status);
    UNREFERENCED_PARAMETER(non_destructive);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY WddmQueryDeviceDescriptor(PVOID context, ULONG child_uid,
                                                   PDXGK_DEVICE_DESCRIPTOR desc)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(child_uid);
    UNREFERENCED_PARAMETER(desc);
    return STATUS_NOT_SUPPORTED;
}

/* ---- Adapter information ----------------------------------------------- */

static NTSTATUS APIENTRY WddmQueryAdapterInfo(
    PVOID context, const DXGKARG_QUERYADAPTERINFO *q)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;

    if (!a || !q || !q->pOutputData)
        return STATUS_INVALID_PARAMETER;

    WLog("QueryAdapterInfo: type=%u in=%u out=%u", (UINT)q->Type,
         q->InputDataSize, q->OutputDataSize);

    switch (q->Type) {
    case DXGKQAITYPE_UMDRIVERPRIVATE:
        RtlZeroMemory(q->pOutputData, q->OutputDataSize);
        return STATUS_SUCCESS;
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *caps;
        if (q->OutputDataSize < sizeof(*caps)) {
            WLog("DRIVERCAPS buffer too small: got=%u need=%u",
                 q->OutputDataSize, (UINT)sizeof(*caps));
            return STATUS_BUFFER_TOO_SMALL;
        }
        caps = (DXGK_DRIVERCAPS *)q->pOutputData;
        RtlZeroMemory(caps, q->OutputDataSize);
        caps->WDDMVersion =
            (g_wddm_version >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
                ? DXGKDDI_WDDMv2
                : DXGKDDI_WDDMv1_3;
        caps->HighestAcceptableAddress.QuadPart = -1ll;
        caps->MaxAllocationListSlotId = 16;
        caps->ApertureSegmentCommitLimit = 256ull * 1024 * 1024;
        caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        caps->SupportNonVGA = TRUE;
        caps->SupportPerEngineTDR = TRUE;
        caps->FlipCaps.FlipOnVSyncMmIo = TRUE;
        caps->PresentationCaps.SupportSoftwareDeviceBitmaps = TRUE;
        caps->PresentationCaps.NoScreenToScreenBlt = TRUE;
        caps->PresentationCaps.NoOverlapScreenBlt = TRUE;
        caps->SchedulingCaps.MultiEngineAware = 1;
        caps->SchedulingCaps.PreemptionAware = 1;
        caps->SchedulingCaps.CancelCommandAware = 1;
        caps->PreemptionCaps.GraphicsPreemptionGranularity =
            D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->PreemptionCaps.ComputePreemptionGranularity =
            D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
        if (g_wddm_version >= DXGKDDI_INTERFACE_VERSION_WDDM2_0) {
            caps->SchedulingCaps.NoDmaPatching = 1;
            caps->SupportSurpriseRemoval = TRUE;
            caps->MemoryManagementCaps.VirtualAddressingSupported = 1;
            caps->MemoryManagementCaps.GpuMmuSupported = 1;
            caps->MemoryManagementCaps.IoMmuSupported = 0;
            caps->MemoryManagementCaps.CrossAdapterResource = 1;
        }
#endif
        WLog("QueryAdapterInfo(DRIVERCAPS) -> OK (WDDM=0x%x, 1 node, sizeof=%u)",
             (UINT)caps->WDDMVersion, (UINT)sizeof(*caps));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT3: {
        DXGK_QUERYSEGMENTOUT3 *out = (DXGK_QUERYSEGMENTOUT3 *)q->pOutputData;
        DXGK_SEGMENTDESCRIPTOR3 *seg;
        if (q->OutputDataSize < sizeof(*out))
            return STATUS_BUFFER_TOO_SMALL;
        if (!out->pSegmentDescriptor) {
            out->NbSegment = 1;
            out->PagingBufferSegmentId = 0;
            out->PagingBufferSize = 64 * 1024;
            out->PagingBufferPrivateDataSize = 0;
            return STATUS_SUCCESS;
        }
        if (out->NbSegment < 1)
            return STATUS_BUFFER_TOO_SMALL;
        seg = &out->pSegmentDescriptor[0];
        RtlZeroMemory(seg, sizeof(*seg));
        seg->Flags.Aperture = 1;
        seg->Flags.CpuVisible = 1;
        seg->Flags.CacheCoherent = 0;
        seg->Flags.PopulatedFromSystemMemory = 1;
        seg->Size = 512ull * 1024 * 1024;
        seg->CommitLimit = seg->Size;
        out->NbSegment = 1;
        out->PagingBufferSegmentId = 0;
        out->PagingBufferSize = 64 * 1024;
        out->PagingBufferPrivateDataSize = 0;
        WLog("QueryAdapterInfo(QUERYSEGMENT3) -> 1 aperture 512 MiB");
        return STATUS_SUCCESS;
    }
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    case DXGKQAITYPE_QUERYSEGMENT4: {
        DXGK_QUERYSEGMENTOUT4 *out = (DXGK_QUERYSEGMENTOUT4 *)q->pOutputData;
        DXGK_SEGMENTDESCRIPTOR4 *seg;
        if (q->OutputDataSize < sizeof(*out))
            return STATUS_BUFFER_TOO_SMALL;
        if (!out->pSegmentDescriptor) {
            out->NbSegment = 1;
            out->PagingBufferSegmentId = 0;
            out->PagingBufferSize = 64 * 1024;
            out->PagingBufferPrivateDataSize = 0;
            return STATUS_SUCCESS;
        }
        if (out->NbSegment < 1)
            return STATUS_BUFFER_TOO_SMALL;
        seg = (DXGK_SEGMENTDESCRIPTOR4 *)out->pSegmentDescriptor;
        RtlZeroMemory(seg, sizeof(*seg));
        seg->Flags.Aperture = 1;
        seg->Flags.CpuVisible = 1;
        seg->Flags.CacheCoherent = 0;
        seg->Flags.PopulatedFromSystemMemory = 1;
        seg->Size = 512ull * 1024 * 1024;
        seg->CommitLimit = seg->Size;
        out->NbSegment = 1;
        out->PagingBufferSegmentId = 0;
        out->PagingBufferSize = 64 * 1024;
        out->PagingBufferPrivateDataSize = 0;
        WLog("QueryAdapterInfo(QUERYSEGMENT4) -> 1 aperture 512 MiB");
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_GPUMMUCAPS: {
        DXGK_GPUMMUCAPS mc;
        SIZE_T copy_sz;
        RtlZeroMemory(&mc, sizeof(mc));
        mc.ReadOnlyMemorySupported = 1;
        mc.ZeroInPteSupported = 1;
        mc.CacheCoherentMemorySupported = 1;
        mc.PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
        mc.VirtualAddressBitCount = 39;
        mc.PageTableLevelCount = 3;
        copy_sz = (q->OutputDataSize < sizeof(mc)) ? q->OutputDataSize : sizeof(mc);
        RtlZeroMemory(q->pOutputData, q->OutputDataSize);
        RtlCopyMemory(q->pOutputData, &mc, copy_sz);
        WLog("QueryAdapterInfo(GPUMMUCAPS) -> 39-bit 3-level CPU_VIRTUAL (out=%u sz=%u)",
             q->OutputDataSize, (UINT)sizeof(mc));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_PAGETABLELEVELDESC: {
        DXGK_PAGE_TABLE_LEVEL_DESC desc;
        SIZE_T copy_sz;
        RtlZeroMemory(&desc, sizeof(desc));
        desc.PageTableIndexBitCount = 9;
        desc.PageTableSegmentId = 0;
        desc.PagingProcessPageTableSegmentId = 0;
        desc.PageTableSizeInBytes = 4096;
        desc.PageTableAlignmentInBytes = 4096;
        copy_sz = (q->OutputDataSize < sizeof(desc)) ? q->OutputDataSize : sizeof(desc);
        RtlZeroMemory(q->pOutputData, q->OutputDataSize);
        RtlCopyMemory(q->pOutputData, &desc, copy_sz);
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_HISTORYBUFFERPRECISION: {
        UINT n = q->OutputDataSize / sizeof(DXGKARG_HISTORYBUFFERPRECISION);
        UINT i;
        DXGKARG_HISTORYBUFFERPRECISION *hp =
            (DXGKARG_HISTORYBUFFERPRECISION *)q->pOutputData;
        for (i = 0; i < n; i++)
            hp[i].PrecisionBits = 64;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_PHYSICALADAPTERCAPS: {
        DXGK_PHYSICALADAPTERCAPS pc;
        SIZE_T copy_sz;
        RtlZeroMemory(&pc, sizeof(pc));
        pc.NumExecutionNodes = 1;
        pc.PagingNodeIndex = 0;
        pc.DxgkPhysicalAdapterHandle = a->device_handle;
        pc.Flags.GpuMmuSupported = 1;
        pc.Flags.IoMmuSupported = 0;
        copy_sz = (q->OutputDataSize < sizeof(pc)) ? q->OutputDataSize : sizeof(pc);
        RtlZeroMemory(q->pOutputData, q->OutputDataSize);
        RtlCopyMemory(q->pOutputData, &pc, copy_sz);
        WLog("QueryAdapterInfo(PHYSICALADAPTERCAPS) -> 1 node GpuMmu=1 (out=%u sz=%u offFlags=%u)",
             q->OutputDataSize, (UINT)sizeof(pc),
             (UINT)FIELD_OFFSET(DXGK_PHYSICALADAPTERCAPS, Flags));
        return STATUS_SUCCESS;
    }
#endif
    case 24: /* DXGKQAITYPE_NODEPERFDATA (44 bytes) */ {
        struct {
            ULONGLONG Frequency;
            ULONGLONG MaxFrequency;
            ULONGLONG MaxFrequencyOC;
            ULONG Voltage;
            ULONG VoltageMax;
            ULONG VoltageMaxOC;
            ULONGLONG MaxTransitionLatency;
        } np;
        SIZE_T copy_sz;
        RtlZeroMemory(&np, sizeof(np));
        np.Frequency = 525000000ull;
        np.MaxFrequency = 818000000ull;
        np.MaxFrequencyOC = 818000000ull;
        copy_sz = (q->OutputDataSize < sizeof(np)) ? q->OutputDataSize : sizeof(np);
        RtlZeroMemory(q->pOutputData, q->OutputDataSize);
        RtlCopyMemory(q->pOutputData, &np, copy_sz);
        return STATUS_SUCCESS;
    }
    case 25: /* DXGKQAITYPE_ADAPTERPERFDATA (53 bytes) */ {
        struct {
            ULONGLONG MemoryFrequency;
            ULONGLONG MaxMemoryFrequency;
            ULONGLONG MaxMemoryFrequencyOC;
            ULONGLONG MemoryBandwidth;
            ULONGLONG PCIEBandwidth;
            ULONG FanRPM;
            ULONG Power;
            ULONG Temperature;
            UCHAR PowerStateOverride;
        } ap;
        SIZE_T copy_sz;
        RtlZeroMemory(&ap, sizeof(ap));
        ap.MemoryFrequency = 3200000000ull;
        ap.MaxMemoryFrequency = 3200000000ull;
        ap.MaxMemoryFrequencyOC = 3200000000ull;
        copy_sz = (q->OutputDataSize < sizeof(ap)) ? q->OutputDataSize : sizeof(ap);
        RtlZeroMemory(q->pOutputData, q->OutputDataSize);
        RtlCopyMemory(q->pOutputData, &ap, copy_sz);
        return STATUS_SUCCESS;
    }
    default:
        WLog("QueryAdapterInfo(type=%u) -> STATUS_NOT_SUPPORTED", (UINT)q->Type);
        return STATUS_NOT_SUPPORTED;
    }
}

/* ---- Objects ----------------------------------------------------------- */

static NTSTATUS APIENTRY WddmCreateDevice(PVOID context,
                                          DXGKARG_CREATEDEVICE *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    PADRENO_WDDM_DEVICE d;

    if (!a || !arg)
        return STATUS_INVALID_PARAMETER;
    d = (PADRENO_WDDM_DEVICE)ExAllocatePoolZero(
        NonPagedPoolNx, sizeof(*d), ADRENO_WDDM_TAG);
    if (!d)
        return STATUS_INSUFFICIENT_RESOURCES;
    d->adapter = a;
    d->hDevice = (HANDLE)d;
    arg->hDevice = d;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmDestroyDevice(PVOID object)
{
    PADRENO_WDDM_DEVICE d = (PADRENO_WDDM_DEVICE)object;
    if (d)
        ExFreePoolWithTag(d, ADRENO_WDDM_TAG);
    return STATUS_SUCCESS;
}

static void WddmReleaseAlloc(PADRENO_WDDM_ADAPTER a,
                             adreno_wddm_alloc *alloc)
{
    if (!a || !alloc || !alloc->in_use)
        return;
    if (!alloc->host && alloc->pa == 0) {
        ExFreePoolWithTag(alloc, ADRENO_WDDM_TAG);
        return;
    }
    if (a->hw_ready && AdrenoWddmHwSmmuReady(&a->hw) && alloc->mapped)
        AdrenoWddmHwUnmapIova(&a->hw, alloc->iova, alloc->size);
    adreno_wddm_free_buf(&a->wddm, alloc);
}

static NTSTATUS APIENTRY WddmCreateAllocation(
    PVOID context, DXGKARG_CREATEALLOCATION *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    static LONG s_alloc_logs = 0;
    UINT i;

    if (!a || !a->wddm_ready || !arg || !arg->pAllocationInfo ||
        arg->NumAllocations == 0)
        return STATUS_INVALID_PARAMETER;
    for (i = 0; i < arg->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *info = &arg->pAllocationInfo[i];
        adreno_wddm_alloc *alloc = NULL;
        SIZE_T req_size = info->Size;

        if (info->pPrivateDriverData &&
            info->PrivateDriverDataSize >= sizeof(adreno_wddm_alloc_private)) {
            const adreno_wddm_alloc_private *priv =
                (const adreno_wddm_alloc_private *)info->pPrivateDriverData;
            if (priv->magic == ADRENO_WDDM_ALLOC_PRIVATE_MAGIC && priv->size)
                req_size = priv->size;
        } else if (arg->pPrivateDriverData &&
                   arg->PrivateDriverDataSize >= sizeof(adreno_wddm_alloc_private)) {
            const adreno_wddm_alloc_private *priv =
                (const adreno_wddm_alloc_private *)arg->pPrivateDriverData;
            if (priv->magic == ADRENO_WDDM_ALLOC_PRIVATE_MAGIC && priv->size)
                req_size = priv->size;
        }
        if (req_size == 0)
            req_size = 4096;
        req_size = (req_size + 4095u) & ~(SIZE_T)4095u;

        if (req_size > 0xffffffffull) {
            while (i--)
                WddmReleaseAlloc(a, arg->pAllocationInfo[i].hAllocation);
            return STATUS_INVALID_PARAMETER;
        }
        if (g_wddm_version >= DXGKDDI_INTERFACE_VERSION_WDDM2_0) {
            alloc = (adreno_wddm_alloc *)ExAllocatePoolZero(
                NonPagedPoolNx, sizeof(*alloc), ADRENO_WDDM_TAG);
            if (!alloc) {
                while (i--)
                    WddmReleaseAlloc(a, arg->pAllocationInfo[i].hAllocation);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            alloc->size = (uint32_t)req_size;
            alloc->in_use = true;
        } else {
            enum adreno_status st = adreno_wddm_alloc_buf(
                &a->wddm, (uint32_t)req_size, 4096, &alloc);
            if (st != ADRENO_OK) {
                while (i--)
                    WddmReleaseAlloc(a, arg->pAllocationInfo[i].hAllocation);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            if (a->hw_ready && AdrenoWddmHwSmmuReady(&a->hw)) {
                uint64_t iova = AdrenoWddmHwAllocIova(&a->hw, alloc->size);
                if (!iova ||
                    AdrenoWddmHwMapIova(&a->hw, iova, alloc->pa, alloc->size) != ADRENO_OK) {
                    WddmReleaseAlloc(a, alloc);
                    while (i--)
                        WddmReleaseAlloc(a, arg->pAllocationInfo[i].hAllocation);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                alloc->iova = iova;
                alloc->mapped = TRUE;
            }
        }
        info->hAllocation = alloc;
        info->Size = alloc->size;
        info->PitchAlignedSize = alloc->size;
        info->Alignment = 4096;
        info->PreferredSegment.Value = 0;
        info->PreferredSegment.SegmentId0 = 1;
        info->SupportedReadSegmentSet = 0x1;
        info->SupportedWriteSegmentSet = 0x1;
        info->EvictionSegmentSet = 0x1;
        info->PhysicalAdapterIndex = 0;
        info->Flags.Value = 0;
        info->Flags.CpuVisible = 1;
        info->Flags.Cached = 0;
        info->AllocationPriority = 0;
        if (InterlockedIncrement(&s_alloc_logs) <= 16)
            WLog("CreateAllocation: size=%u h=%p", alloc->size, alloc);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmDestroyAllocation(
    PVOID context, const DXGKARG_DESTROYALLOCATION *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    UINT i;

    if (!a || !a->wddm_ready || !arg || !arg->pAllocationList)
        return STATUS_INVALID_PARAMETER;
    for (i = 0; i < arg->NumAllocations; i++)
        WddmReleaseAlloc(a, (adreno_wddm_alloc *)arg->pAllocationList[i]);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmOpenAllocation(
    PVOID hDevice, const DXGKARG_OPENALLOCATION *arg)
{
    UINT i;
    UNREFERENCED_PARAMETER(hDevice);
    if (!arg || !arg->pOpenAllocation)
        return STATUS_INVALID_PARAMETER;
    for (i = 0; i < arg->NumAllocations; i++)
        arg->pOpenAllocation[i].hDeviceSpecificAllocation =
            (HANDLE)(ULONG_PTR)arg->pOpenAllocation[i].hAllocation;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmCloseAllocation(
    PVOID hDevice, const DXGKARG_CLOSEALLOCATION *arg)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(arg);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmGetStandardAllocationDriverData(
    PVOID context, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *arg)
{
    UNREFERENCED_PARAMETER(context);
    if (!arg)
        return STATUS_INVALID_PARAMETER;
    arg->AllocationPrivateDriverDataSize = 0;
    arg->ResourcePrivateDriverDataSize = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmDescribeAllocation(
    PVOID context, DXGKARG_DESCRIBEALLOCATION *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    adreno_wddm_alloc *alloc;

    if (!a || !arg)
        return STATUS_INVALID_PARAMETER;
    alloc = (adreno_wddm_alloc *)arg->hAllocation;
    if (!alloc || !alloc->in_use)
        return STATUS_INVALID_PARAMETER;
    arg->Width = alloc->size;
    arg->Height = 1;
    arg->Format = (D3DDDIFORMAT)0;
    arg->MultisampleMethod.NumSamples = 1;
    arg->MultisampleMethod.NumQualityLevels = 1;
    arg->RefreshRate.Numerator = 60;
    arg->RefreshRate.Denominator = 1;
    arg->PrivateDriverFormatAttribute = 0;
    arg->Flags.Value = 0;
    arg->Rotation = D3DDDI_ROTATION_IDENTITY;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmCreateContext(PVOID context,
                                           DXGKARG_CREATECONTEXT *arg)
{
    PADRENO_WDDM_DEVICE d = (PADRENO_WDDM_DEVICE)context;
    PADRENO_WDDM_ADAPTER a = d ? d->adapter : NULL;
    PADRENO_WDDM_CONTEXT c;

    if (!a || !arg)
        return STATUS_INVALID_PARAMETER;
    c = (PADRENO_WDDM_CONTEXT)ExAllocatePoolZero(
        NonPagedPoolNx, sizeof(*c), ADRENO_WDDM_TAG);
    if (!c)
        return STATUS_INSUFFICIENT_RESOURCES;
    c->adapter = a;
    c->hContext = (HANDLE)c;
    arg->hContext = c;
    RtlZeroMemory(&arg->ContextInfo, sizeof(arg->ContextInfo));
    arg->ContextInfo.DmaBufferSize = 64 * 1024;
    arg->ContextInfo.DmaBufferSegmentSet = 0x1;
    arg->ContextInfo.DmaBufferPrivateDataSize = 16;
    arg->ContextInfo.AllocationListSize = 64;
    arg->ContextInfo.PatchLocationListSize = 64;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmDestroyContext(PVOID object)
{
    if (object)
        ExFreePoolWithTag(object, ADRENO_WDDM_TAG);
    return STATUS_SUCCESS;
}

/* ---- Submission -------------------------------------------------------- */

#define ADRENO_WDDM_DMA_MAGIC 0x44373130u

typedef struct ADRENO_WDDM_DMA_PRIVATE {
    uint32_t magic;
    uint32_t dwords;
} ADRENO_WDDM_DMA_PRIVATE;

static void *WddmMapMdl(PMDL mdl)
{
    if (!mdl)
        return NULL;
    return MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
}

static void WddmTrackDma(PADRENO_WDDM_ADAPTER a, PHYSICAL_ADDRESS pa,
                         void *host, uint32_t size)
{
    ULONG i;
    if (!a || !host || size == 0)
        return;
    for (i = 0; i < ADRENO_ARRAY_SIZE(a->dma_maps); i++) {
        if (a->dma_maps[i].pa.QuadPart == pa.QuadPart) {
            a->dma_maps[i].host = host;
            a->dma_maps[i].size = size;
            return;
        }
    }
    for (i = 0; i < ADRENO_ARRAY_SIZE(a->dma_maps); i++) {
        if (!a->dma_maps[i].host) {
            a->dma_maps[i].pa = pa;
            a->dma_maps[i].host = host;
            a->dma_maps[i].size = size;
            return;
        }
    }
    a->dma_maps[0].pa = pa;
    a->dma_maps[0].host = host;
    a->dma_maps[0].size = size;
}

static void *WddmFindDmaHost(PADRENO_WDDM_ADAPTER a, PHYSICAL_ADDRESS pa,
                             uint32_t offset, uint32_t size)
{
    ULONG i;
    if (!a)
        return NULL;
    for (i = 0; i < ADRENO_ARRAY_SIZE(a->dma_maps); i++) {
        ADRENO_WDDM_DMA_MAP *map = &a->dma_maps[i];
        if (!map->host)
            continue;
        if (pa.QuadPart >= map->pa.QuadPart &&
            pa.QuadPart + offset + size <= map->pa.QuadPart + map->size)
            return (uint8_t *)map->host +
                   (pa.QuadPart - map->pa.QuadPart) + offset;
    }
    return NULL;
}

static NTSTATUS APIENTRY WddmRender(PVOID context, DXGKARG_RENDER *arg)
{
    PADRENO_WDDM_CONTEXT ctx = (PADRENO_WDDM_CONTEXT)context;
    PADRENO_WDDM_ADAPTER a = ctx ? ctx->adapter : NULL;

    if (!a || !arg || !arg->pDmaBuffer || !arg->pCommand ||
        arg->CommandLength == 0)
        return STATUS_INVALID_PARAMETER;
    if (arg->CommandLength > arg->DmaSize)
        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(arg->pDmaBuffer, arg->pCommand, arg->CommandLength);
    arg->DmaSize = arg->CommandLength;
    WddmTrackDma(a, arg->DmaBufferPhysicalAddress, arg->pDmaBuffer,
                 arg->DmaSize);
    if (arg->pDmaBufferPrivateData &&
        arg->DmaBufferPrivateDataSize >= sizeof(ADRENO_WDDM_DMA_PRIVATE)) {
        ADRENO_WDDM_DMA_PRIVATE *priv =
            (ADRENO_WDDM_DMA_PRIVATE *)arg->pDmaBufferPrivateData;
        priv->magic = ADRENO_WDDM_DMA_MAGIC;
        priv->dwords = arg->CommandLength / 4;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmPresent(PVOID context, DXGKARG_PRESENT *arg)
{
    UNREFERENCED_PARAMETER(context);
    if (arg) {
        arg->DmaSize = 0;
        arg->MultipassOffset = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmPatch(PVOID context, const DXGKARG_PATCH *arg)
{
    PADRENO_WDDM_CONTEXT ctx = (PADRENO_WDDM_CONTEXT)context;
    PADRENO_WDDM_ADAPTER a = ctx ? ctx->adapter : NULL;
    UINT i, start, count;

    if (!a || !arg || !arg->pDmaBuffer)
        return STATUS_INVALID_PARAMETER;
    if (!arg->pPatchLocationList || !arg->pAllocationList)
        return STATUS_SUCCESS;
    start = arg->PatchLocationListSubmissionStart;
    count = arg->PatchLocationListSubmissionLength;
    if (start > arg->PatchLocationListSize ||
        count > arg->PatchLocationListSize - start)
        return STATUS_INVALID_PARAMETER;
    for (i = 0; i < count; i++) {
        const D3DDDI_PATCHLOCATIONLIST *patch =
            &arg->pPatchLocationList[start + i];
        adreno_wddm_alloc *alloc;
        uint64_t value;
        if (patch->AllocationIndex >= arg->AllocationListSize)
            return STATUS_INVALID_PARAMETER;
        alloc = (adreno_wddm_alloc *)
            arg->pAllocationList[patch->AllocationIndex]
                .hDeviceSpecificAllocation;
        if (!alloc || !alloc->in_use)
            return STATUS_INVALID_PARAMETER;
        if ((uint64_t)patch->AllocationOffset + 8 > alloc->size)
            return STATUS_INVALID_PARAMETER;
        if ((uint64_t)patch->PatchOffset + 8 > arg->DmaBufferSize)
            return STATUS_INVALID_PARAMETER;
        value = alloc->iova + patch->AllocationOffset;
        RtlCopyMemory((uint8_t *)arg->pDmaBuffer + patch->PatchOffset, &value, 8);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmBuildPagingBuffer(PVOID context,
                                               DXGKARG_BUILDPAGINGBUFFER *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    adreno_wddm_alloc *alloc;
    enum adreno_status st = ADRENO_OK;

    if (!a || !a->wddm_ready || !arg)
        return STATUS_INVALID_PARAMETER;

    switch (arg->Operation) {
    case DXGK_OPERATION_TRANSFER: {
        void *mdl_data;
        alloc = (adreno_wddm_alloc *)arg->Transfer.hAllocation;
        if (!alloc || !alloc->in_use ||
            arg->Transfer.TransferSize > 0xffffffffull)
            return STATUS_INVALID_PARAMETER;
        if (arg->Transfer.Source.pMdl) {
            mdl_data = WddmMapMdl(arg->Transfer.Source.pMdl);
            if (!mdl_data)
                return STATUS_INSUFFICIENT_RESOURCES;
            st = adreno_wddm_paging_transfer(
                &a->wddm, alloc, arg->Transfer.TransferOffset,
                (uint8_t *)mdl_data + arg->Transfer.MdlOffset,
                (uint32_t)arg->Transfer.TransferSize);
        } else if (arg->Transfer.Destination.pMdl) {
            mdl_data = WddmMapMdl(arg->Transfer.Destination.pMdl);
            if (!mdl_data)
                return STATUS_INSUFFICIENT_RESOURCES;
            RtlCopyMemory((uint8_t *)mdl_data + arg->Transfer.MdlOffset,
                          (uint8_t *)alloc->host + arg->Transfer.TransferOffset,
                          arg->Transfer.TransferSize);
        } else {
            uint64_t src_va =
                (uint64_t)arg->Transfer.Source.SegmentAddress.QuadPart;
            uint64_t dst_va =
                (uint64_t)arg->Transfer.Destination.SegmentAddress.QuadPart;
            adreno_wddm_alloc *src = adreno_wddm_find_iova(
                &a->wddm, src_va, (uint32_t)arg->Transfer.TransferSize);
            adreno_wddm_alloc *dst = adreno_wddm_find_iova(
                &a->wddm, dst_va, (uint32_t)arg->Transfer.TransferSize);
            if (!src || !dst) {
                st = ADRENO_ERR_RANGE;
                break;
            }
            RtlMoveMemory((uint8_t *)dst->host + (dst_va - dst->iova),
                          (uint8_t *)src->host + (src_va - src->iova),
                          arg->Transfer.TransferSize);
        }
        break;
    }
    case DXGK_OPERATION_FILL: {
        uint64_t dst;
        alloc = (adreno_wddm_alloc *)arg->Fill.hAllocation;
        dst = (uint64_t)arg->Fill.Destination.SegmentAddress.QuadPart;
        if (!alloc || !alloc->in_use || dst < alloc->iova ||
            arg->Fill.FillSize > 0xffffffffull)
            return STATUS_INVALID_PARAMETER;
        st = adreno_wddm_paging_fill(
            &a->wddm, alloc, (uint32_t)(dst - alloc->iova),
            (uint32_t)arg->Fill.FillSize, arg->Fill.FillPattern);
        break;
    }
    case DXGK_OPERATION_DISCARD_CONTENT:
        alloc = (adreno_wddm_alloc *)arg->DiscardContent.hAllocation;
        if (!alloc || !alloc->in_use)
            return STATUS_INVALID_PARAMETER;
        st = adreno_wddm_paging_discard(&a->wddm, alloc);
        break;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    case DXGK_OPERATION_MAP_APERTURE_SEGMENT: {
        SIZE_T j;
        if (a->aperture_pa && arg->MapApertureSegment.pMdl) {
            PPFN_NUMBER pfns = MmGetMdlPfnArray(arg->MapApertureSegment.pMdl);
            SIZE_T base = arg->MapApertureSegment.OffsetInPages;
            SIZE_T cnt = arg->MapApertureSegment.NumberOfPages;
            ULONG mdl_off = arg->MapApertureSegment.MdlOffset;
            for (j = 0; j < cnt; j++) {
                if (base + j < a->aperture_pages)
                    a->aperture_pa[base + j] = ((uint64_t)pfns[mdl_off + j]) << 12;
            }
        }
        break;
    }
    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT: {
        SIZE_T j;
        if (a->aperture_pa) {
            SIZE_T base = arg->UnmapApertureSegment.OffsetInPages;
            SIZE_T cnt = arg->UnmapApertureSegment.NumberOfPages;
            for (j = 0; j < cnt; j++) {
                if (base + j < a->aperture_pages)
                    a->aperture_pa[base + j] = 0;
            }
        }
        break;
    }
    case DXGK_OPERATION_UPDATE_PAGE_TABLE: {
        const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *u = &arg->UpdatePageTable;
        static LONG s_pt_logs = 0;
        if (u->PageTableLevel == 0 && u->pPageTableEntries &&
            a->hw_ready && AdrenoWddmHwSmmuReady(&a->hw)) {
            uint32_t idx;
            bool touched = false;
            for (idx = 0; idx < u->NumPageTableEntries; idx++) {
                const DXGK_PTE *pte = &u->pPageTableEntries[u->Flags.Repeat ? 0 : idx];
                uint64_t va = u->FirstPteVirtualAddress + ((uint64_t)idx << 12);
                uint64_t pa = 0;
                bool safe_va = (va >= 0x80000000ull) ||
                               (va >= 0x20000000ull && va < 0x60000000ull);
                if (pte->Valid) {
                    if (pte->Segment == 0) {
                        /* On ARM64/Windows 11 dxgkrnl, DXGK_PTE.PageAddress is a PFN! */
                        pa = ((uint64_t)pte->PageAddress) << 12;
                    } else if (pte->Segment == 1 && a->aperture_pa) {
                        uint32_t ap_idx = (uint32_t)pte->PageAddress;
                        if (ap_idx < a->aperture_pages)
                            pa = a->aperture_pa[ap_idx];
                    }
                }
                if (idx == 0 && InterlockedIncrement(&s_pt_logs) <= 16) {
                    WLog("UPDATE_PT L0: va=0x%llx cnt=%u valid=%u seg=%u raw=0x%llx pa=0x%llx safe=%u",
                         va, u->NumPageTableEntries, (UINT)pte->Valid, (UINT)pte->Segment,
                         (uint64_t)pte->PageAddress, pa, safe_va ? 1u : 0u);
                }
                if (!safe_va)
                    continue;
                if (pte->Valid && pa) {
                    enum adreno_status mst;
                    (void)adreno_pt_unmap(&a->hw.gpu_pt, va, 4096u);
                    mst = adreno_pt_map(&a->hw.gpu_pt, va, pa, 4096u, false);
                    if (mst != ADRENO_OK && idx == 0)
                        WLog("UPDATE_PT map FAILED st=%d va=0x%llx pa=0x%llx", (int)mst, va, pa);
                    touched = true;
                } else {
                    (void)adreno_pt_unmap(&a->hw.gpu_pt, va, 4096u);
                    touched = true;
                }
            }
            if (touched) {
                KeMemoryBarrier();
                adreno_smmu_tlb_invalidate(&a->hw.smmu);
            }
        }
        break;
    }
    case DXGK_OPERATION_FLUSH_TLB:
        if (a->hw_ready && AdrenoWddmHwSmmuReady(&a->hw))
            adreno_smmu_tlb_invalidate(&a->hw.smmu);
        break;
    case DXGK_OPERATION_VIRTUAL_FILL:
    case DXGK_OPERATION_VIRTUAL_TRANSFER:
        break;
#endif
    default:
        break;
    }
    if (st != ADRENO_OK)
        return STATUS_UNSUCCESSFUL;
    arg->DmaSize = 0;
    arg->MultipassOffset = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmPreemptCommand(PVOID context,
                                            const DXGKARG_PREEMPTCOMMAND *arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arg);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmSubmitCommand(
    PVOID context, const DXGKARG_SUBMITCOMMAND *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    uint64_t iova;
    uint32_t bytes;
    enum adreno_status st;
    void *dma;

    if (!a || !a->wddm_ready || !arg)
        return STATUS_INVALID_PARAMETER;
    if (arg->Flags.Paging) {
        a->pending_fence = (LONG)arg->SubmissionFenceId;
        a->dxgk.DxgkCbQueueDpc(a->device_handle);
        return STATUS_SUCCESS;
    }
    if (arg->DmaBufferSubmissionEndOffset <=
            arg->DmaBufferSubmissionStartOffset ||
        arg->DmaBufferSubmissionEndOffset > arg->DmaBufferSize)
        return STATUS_INVALID_PARAMETER;
    bytes = arg->DmaBufferSubmissionEndOffset -
            arg->DmaBufferSubmissionStartOffset;
    if (bytes & 3)
        return STATUS_INVALID_PARAMETER;
    iova = arg->DmaBufferVirtualAddress
               ? arg->DmaBufferVirtualAddress +
                     arg->DmaBufferSubmissionStartOffset
               : arg->DmaBufferPhysicalAddress.QuadPart +
                     arg->DmaBufferSubmissionStartOffset;
    dma = WddmFindDmaHost(a, arg->DmaBufferPhysicalAddress,
                          arg->DmaBufferSubmissionStartOffset, bytes);
    if (a->hw_ready && AdrenoWddmHwCpStarted(&a->hw)) {
        adreno_wddm_alloc *alloc = adreno_wddm_find_pa(
            &a->wddm, arg->DmaBufferPhysicalAddress.QuadPart, bytes);
        adreno_ib ib;
        if (!alloc)
            return STATUS_INVALID_PARAMETER;
        ib.iova = alloc->iova + arg->DmaBufferSubmissionStartOffset;
        ib.size_dwords = bytes / 4;
        ib.handle = 0;
        st = AdrenoWddmHwSubmit(&a->hw, &ib, 1, arg->SubmissionFenceId);
        if (st != ADRENO_OK)
            return STATUS_UNSUCCESSFUL;
        if (!a->hw.has_irq)
            (void)AdrenoWddmHwWaitFence(&a->hw, arg->SubmissionFenceId, 500);
        a->pending_fence = (LONG)arg->SubmissionFenceId;
        a->dxgk.DxgkCbQueueDpc(a->device_handle);
        return STATUS_SUCCESS;
    }
    if (!dma)
        return STATUS_INVALID_PARAMETER;
    st = adreno_wddm_submit_dma(&a->wddm, iova, dma, bytes,
                                arg->SubmissionFenceId);
    if (st != ADRENO_OK)
        return STATUS_UNSUCCESSFUL;
    a->pending_fence = (LONG)arg->SubmissionFenceId;
    a->dxgk.DxgkCbQueueDpc(a->device_handle);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmQueryCurrentFence(
    PVOID context, DXGKARG_QUERYCURRENTFENCE *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;

    if (!a || !arg)
        return STATUS_INVALID_PARAMETER;
    if (a->hw_ready && AdrenoWddmHwCpStarted(&a->hw))
        arg->CurrentFence = AdrenoWddmHwFence(&a->hw);
    else
        arg->CurrentFence = a->wddm_ready
                                ? adreno_wddm_completed_fence(&a->wddm)
                                : 0;
    return STATUS_SUCCESS;
}

/* ---- Interrupts & DPC -------------------------------------------------- */

static BOOLEAN APIENTRY WddmInterruptRoutine(PVOID context, ULONG message)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    uint32_t status;

    UNREFERENCED_PARAMETER(message);
    if (!a || !a->hw_ready || !a->hw.has_irq)
        return FALSE;
    status = AdrenoWddmHwIrqStatus(&a->hw);
    if (!status)
        return FALSE;
    if (status & 0x00100000u) {
        DXGKARGCB_NOTIFY_INTERRUPT_DATA data;
        uint32_t fence = AdrenoWddmHwFence(&a->hw);
        RtlZeroMemory(&data, sizeof(data));
        data.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
        data.DmaCompleted.SubmissionFenceId = fence;
        data.DmaCompleted.NodeOrdinal = 0;
        data.DmaCompleted.EngineOrdinal = 0;
        a->pending_fence = (LONG)fence;
        a->dxgk.DxgkCbNotifyInterrupt(a->device_handle, &data);
    }
    a->dxgk.DxgkCbQueueDpc(a->device_handle);
    return TRUE;
}

static VOID APIENTRY WddmDpcRoutine(PVOID context)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data;

    if (!a || !a->wddm_ready)
        return;
    RtlZeroMemory(&data, sizeof(data));
    data.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    data.DmaCompleted.SubmissionFenceId =
        (UINT)InterlockedCompareExchange(&a->pending_fence, 0, 0);
    data.DmaCompleted.NodeOrdinal = 0;
    data.DmaCompleted.EngineOrdinal = 0;
    a->dxgk.DxgkCbNotifyInterrupt(a->device_handle, &data);
    a->dxgk.DxgkCbNotifyDpc(a->device_handle);
}

/* ---- Power / TDR / Diagnostics / Misc ---------------------------------- */

static NTSTATUS APIENTRY WddmSetPowerState(PVOID context, ULONG hw_id,
                                           DEVICE_POWER_STATE state,
                                           POWER_ACTION action)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(hw_id);
    UNREFERENCED_PARAMETER(state);
    UNREFERENCED_PARAMETER(action);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmResetFromTimeout(PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmRestartFromTimeout(PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmEscape(PVOID context, const DXGKARG_ESCAPE *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    adreno_wddm_esc_hdr *hdr;

    if (!a || !arg || !arg->pPrivateDriverData ||
        arg->PrivateDriverDataSize < sizeof(adreno_wddm_esc_hdr))
        return STATUS_INVALID_PARAMETER;

    hdr = (adreno_wddm_esc_hdr *)arg->pPrivateDriverData;
    if (hdr->magic != ADRENO_WDDM_ESC_MAGIC)
        return STATUS_NOT_SUPPORTED;

    switch (hdr->cmd) {
    case ADRENO_WDDM_ESC_GET_INFO: {
        adreno_wddm_esc_info *out;
        if (arg->PrivateDriverDataSize < sizeof(*out))
            return STATUS_BUFFER_TOO_SMALL;
        out = (adreno_wddm_esc_info *)arg->pPrivateDriverData;
        out->hdr.version = ADRENO_WDDM_ESC_VERSION;
        out->hdr.status = 0;
        out->chip_id = 0x07030001u;
        out->gmem_size = 2u * 1024u * 1024u;
        out->gmem_base = 0x0c000000ull;
        out->va_start = 0x20000000ull;
        out->va_size = 0x30000000ull;
        out->highest_bank_bit = 16u;
        out->ubwc_swizzle = 0x6u;
        out->macrotile_mode = 1u;
        out->hw_ready = a->hw_ready ? 1u : 0u;
        out->cp_started = (a->hw_ready && AdrenoWddmHwCpStarted(&a->hw)) ? 1u : 0u;
        out->last_fence = a->hw_ready ? AdrenoWddmHwFence(&a->hw) : 0u;
        return STATUS_SUCCESS;
    }
    case ADRENO_WDDM_ESC_SUBMIT_IBS: {
        adreno_wddm_esc_submit *sub;
        adreno_ib ibs[ADRENO_WDDM_ESC_MAX_IBS];
        uint32_t i, n, hw_seq;
        enum adreno_status st;
        if (arg->PrivateDriverDataSize < sizeof(*sub))
            return STATUS_BUFFER_TOO_SMALL;
        sub = (adreno_wddm_esc_submit *)arg->pPrivateDriverData;
        if (!a->hw_ready || !AdrenoWddmHwCpStarted(&a->hw) ||
            sub->num_ibs == 0 || sub->num_ibs > ADRENO_WDDM_ESC_MAX_IBS) {
            sub->hdr.status = 1;
            return STATUS_INVALID_PARAMETER;
        }
        n = sub->num_ibs;
        for (i = 0; i < n; i++) {
            ibs[i].iova = sub->ibs[i].iova;
            ibs[i].size_dwords = sub->ibs[i].dwords;
            ibs[i].handle = 0;
        }
        KeWaitForSingleObject(&a->hw_submit_lock, Executive, KernelMode, FALSE, NULL);
        hw_seq = (uint32_t)InterlockedIncrement((volatile LONG *)&a->hw.seqno);
        st = AdrenoWddmHwSubmit(&a->hw, ibs, n, hw_seq);
        sub->out_fence = hw_seq;
        sub->completed = 0;
        if (st != ADRENO_OK) {
            KeReleaseMutex(&a->hw_submit_lock, FALSE);
            sub->hdr.status = (uint32_t)st;
            return STATUS_UNSUCCESSFUL;
        }
        if (sub->timeout_ms > 0) {
            bool done = AdrenoWddmHwWaitFence(&a->hw, hw_seq, sub->timeout_ms);
            sub->completed = done ? 1u : 0u;
            if (!done) {
                uint32_t cb0 = (a->hw.smmu.numpage ? a->hw.smmu.numpage : 32u) * 0x1000u;
                uint32_t fsr = adreno_read(&a->hw.gpu, ADRENO_SPACE_SMMU, (cb0 + 0x58u) / 4u);
                uint32_t far_lo = adreno_read(&a->hw.gpu, ADRENO_SPACE_SMMU, (cb0 + 0x60u) / 4u);
                uint32_t far_hi = adreno_read(&a->hw.gpu, ADRENO_SPACE_SMMU, (cb0 + 0x64u) / 4u);
                uint32_t rbbm = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_RBBM_STATUS);
                uint32_t rptr = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_RPTR);
                uint32_t wptr = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_WPTR);
                uint32_t int0 = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_RBBM_INT_0_STATUS);
                uint32_t cp_ib1_lo = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, 0x000008d8u);
                uint32_t cp_ib1_sz = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, 0x000008dau);
                uint32_t cp_ib2_lo = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, 0x000008dbu);
                uint32_t cp_ib2_sz = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, 0x000008ddu);
                uint32_t prot_err = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, 0x00000842u);
                WLog("Escape SUBMIT_IBS TIMEOUT n=%u ib0=0x%llx(%u dw) seq=%u fence=%u RBBM=0x%x INT0=0x%x RPTR=0x%x WPTR=0x%x CB0_FSR=0x%x FAR=0x%x_%08x IB1=0x%x(rem=%u) IB2=0x%x(rem=%u) CP_INT=0x%x",
                     n, ibs[0].iova, ibs[0].size_dwords, hw_seq, AdrenoWddmHwFence(&a->hw),
                     rbbm, int0, rptr, wptr, fsr, far_hi, far_lo,
                     cp_ib1_lo, cp_ib1_sz, cp_ib2_lo, cp_ib2_sz, prot_err);
            }
        }
        KeReleaseMutex(&a->hw_submit_lock, FALSE);
        sub->hdr.status = 0;
        return STATUS_SUCCESS;
    }
    case ADRENO_WDDM_ESC_WAIT_FENCE: {
        adreno_wddm_esc_wait *w;
        if (arg->PrivateDriverDataSize < sizeof(*w))
            return STATUS_BUFFER_TOO_SMALL;
        w = (adreno_wddm_esc_wait *)arg->pPrivateDriverData;
        if (!a->hw_ready) {
            w->hdr.status = 1;
            return STATUS_DEVICE_NOT_READY;
        }
        w->completed = AdrenoWddmHwWaitFence(&a->hw, w->fence,
                                             w->timeout_ms ? w->timeout_ms : 1000u) ? 1u : 0u;
        w->current_fence = AdrenoWddmHwFence(&a->hw);
        w->hdr.status = 0;
        return STATUS_SUCCESS;
    }
    case ADRENO_WDDM_ESC_GET_TIMESTAMP: {
        adreno_wddm_esc_timestamp *ts;
        if (arg->PrivateDriverDataSize < sizeof(*ts))
            return STATUS_BUFFER_TOO_SMALL;
        ts = (adreno_wddm_esc_timestamp *)arg->pPrivateDriverData;
        if (a->hw_ready) {
            uint32_t hi1, lo, hi2;
            do {
                hi1 = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_ALWAYS_ON_COUNTER + 1u);
                lo  = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_ALWAYS_ON_COUNTER);
                hi2 = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_ALWAYS_ON_COUNTER + 1u);
            } while (hi1 != hi2);
            ts->ticks = ((uint64_t)hi1 << 32) | lo;
        } else {
            ts->ticks = 0;
        }
        ts->frequency_hz = 19200000ull; /* 19.2 MHz XO always-on counter */
        ts->hdr.status = 0;
        return STATUS_SUCCESS;
    }
    default:
        hdr->status = 2;
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS APIENTRY WddmCollectDbgInfo(PVOID context,
                                            const DXGKARG_COLLECTDBGINFO *arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arg);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmQueryEngineStatus(PVOID context,
                                               DXGKARG_QUERYENGINESTATUS *arg)
{
    UNREFERENCED_PARAMETER(context);
    if (arg)
        arg->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmResetEngine(PVOID context,
                                         DXGKARG_RESETENGINE *arg)
{
    UNREFERENCED_PARAMETER(context);
    if (arg)
        arg->LastAbortedFenceId = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmQueryDependentEngineGroup(
    PVOID context, DXGKARG_QUERYDEPENDENTENGINEGROUP *arg)
{
    UNREFERENCED_PARAMETER(context);
    if (arg)
        arg->DependentNodeOrdinalMask = 1ull << arg->NodeOrdinal;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmSetPowerComponentFState(PVOID context,
                                                     UINT idx, UINT fstate)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(idx);
    UNREFERENCED_PARAMETER(fstate);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmControlInterrupt(PVOID context,
                                              const DXGK_INTERRUPT_TYPE type,
                                              BOOLEAN enable)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(type);
    UNREFERENCED_PARAMETER(enable);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmControlInterrupt2(
    PVOID context, const DXGKARG_CONTROLINTERRUPT2 arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arg);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmCancelCommand(PVOID context,
                                           const DXGKARG_CANCELCOMMAND *arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arg);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmQueryInterface(PVOID context,
                                            PQUERY_INTERFACE qi)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(qi);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY WddmGetNodeMetadata(PVOID context, UINT node_ordinal,
                                             DXGKARG_GETNODEMETADATA *arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(node_ordinal);
    if (!arg)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(arg, sizeof(*arg));
    arg->EngineType = DXGK_ENGINE_TYPE_3D;
    RtlStringCchCopyW(arg->FriendlyName, ADRENO_ARRAY_SIZE(arg->FriendlyName),
                      L"Adreno 730 3D");
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    if (g_wddm_version >= DXGKDDI_INTERFACE_VERSION_WDDM2_0) {
        arg->GpuMmuSupported = TRUE;
        arg->IoMmuSupported = FALSE;
    }
#endif
    WLog("GetNodeMetadata(node=%u) -> 3D GpuMmu=%u", node_ordinal,
         (UINT)arg->GpuMmuSupported);
    return STATUS_SUCCESS;
}

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
static NTSTATUS APIENTRY WddmFormatHistoryBuffer(
    HANDLE hContext, DXGKARG_FORMATHISTORYBUFFER *pFormatData)
{
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pFormatData);
    return STATUS_SUCCESS;
}

static VOID APIENTRY WddmSetStablePowerState(
    HANDLE hAdapter, const DXGKARG_SETSTABLEPOWERSTATE *pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
}

static NTSTATUS APIENTRY WddmMapCpuHostAperture(
    PVOID context, const DXGKARG_MAPCPUHOSTAPERTURE *arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arg);
    WLog("MapCpuHostAperture called");
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmUnmapCpuHostAperture(
    PVOID context, const DXGKARG_UNMAPCPUHOSTAPERTURE *arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arg);
    WLog("UnmapCpuHostAperture called");
    return STATUS_SUCCESS;
}
typedef struct ADRENO_WDDM_PROCESS {
    PADRENO_WDDM_ADAPTER adapter;
    HANDLE hDxgkProcess;
} ADRENO_WDDM_PROCESS, *PADRENO_WDDM_PROCESS;

static NTSTATUS APIENTRY WddmCreateProcess(PVOID context,
                                           DXGKARG_CREATEPROCESS *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    PADRENO_WDDM_PROCESS proc;
    if (!a || !arg)
        return STATUS_INVALID_PARAMETER;
    proc = (PADRENO_WDDM_PROCESS)ExAllocatePoolZero(NonPagedPoolNx, sizeof(*proc),
                                                    ADRENO_WDDM_TAG);
    if (!proc)
        return STATUS_INSUFFICIENT_RESOURCES;
    proc->adapter = a;
    proc->hDxgkProcess = arg->hDxgkProcess;
    arg->hKmdProcess = proc;
    WLog("CreateProcess -> %p (sys=%u)", proc, (UINT)arg->Flags.SystemProcess);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmDestroyProcess(PVOID context, HANDLE hKmdProcess)
{
    UNREFERENCED_PARAMETER(context);
    if (hKmdProcess)
        ExFreePoolWithTag(hKmdProcess, ADRENO_WDDM_TAG);
    return STATUS_SUCCESS;
}

static SIZE_T APIENTRY WddmGetRootPageTableSize(
    PVOID context, DXGKARG_GETROOTPAGETABLESIZE *arg)
{
    UNREFERENCED_PARAMETER(context);
    if (!arg)
        return 4096;
    if (arg->NumberOfPte == 0)
        arg->NumberOfPte = 512;
    WLog("GetRootPageTableSize(ptes=%u) -> %u", arg->NumberOfPte, arg->NumberOfPte * 8u);
    return (SIZE_T)arg->NumberOfPte * 8u;
}

static VOID APIENTRY WddmSetRootPageTable(
    PVOID context, const DXGKARG_SETROOTPAGETABLE *arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arg);
}

static NTSTATUS APIENTRY WddmSubmitCommandVirtual(
    PVOID context, const DXGKARG_SUBMITCOMMANDVIRTUAL *arg)
{
    PADRENO_WDDM_ADAPTER a = (PADRENO_WDDM_ADAPTER)context;
    if (!a || !a->wddm_ready || !arg)
        return STATUS_INVALID_PARAMETER;
    if (arg->Flags.Paging || arg->DmaBufferSize == 0) {
        a->pending_fence = (LONG)arg->SubmissionFenceId;
        a->dxgk.DxgkCbQueueDpc(a->device_handle);
        return STATUS_SUCCESS;
    }
    if (a->hw_ready && AdrenoWddmHwCpStarted(&a->hw) &&
        arg->DmaBufferVirtualAddress && (arg->DmaBufferSize & 3u) == 0) {
        adreno_ib ib;
        enum adreno_status st;
        ib.iova = arg->DmaBufferVirtualAddress;
        ib.size_dwords = arg->DmaBufferSize / 4u;
        ib.handle = 0;
        uint32_t hw_seq = (uint32_t)InterlockedIncrement((volatile LONG *)&a->hw.seqno);
        st = AdrenoWddmHwSubmit(&a->hw, &ib, 1, hw_seq);
        if (st == ADRENO_OK && !a->hw.has_irq) {
            bool done = AdrenoWddmHwWaitFence(&a->hw, hw_seq, 1000);
            if (!done) {
                uint32_t cb0 = (a->hw.smmu.numpage ? a->hw.smmu.numpage : 32u) * 0x1000u;
                uint32_t fsr = adreno_read(&a->hw.gpu, ADRENO_SPACE_SMMU, (cb0 + 0x58u) / 4u);
                uint32_t far_lo = adreno_read(&a->hw.gpu, ADRENO_SPACE_SMMU, (cb0 + 0x60u) / 4u);
                uint32_t far_hi = adreno_read(&a->hw.gpu, ADRENO_SPACE_SMMU, (cb0 + 0x64u) / 4u);
                uint32_t rbbm = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_RBBM_STATUS);
                uint32_t rptr = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_RPTR);
                uint32_t wptr = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_WPTR);
                uint32_t int0 = adreno_read(&a->hw.gpu, ADRENO_SPACE_GPU, REG_A6XX_RBBM_INT_0_STATUS);
                WLog("SubmitCommandVirtual TIMEOUT hw_seq=%u fence=%u RBBM=0x%x INT0=0x%x RPTR=0x%x WPTR=0x%x CB0_FSR=0x%x FAR=0x%x_%08x",
                     hw_seq, AdrenoWddmHwFence(&a->hw), rbbm, int0, rptr, wptr, fsr, far_hi, far_lo);
            }
        }
    }
    a->pending_fence = (LONG)arg->SubmissionFenceId;
    a->dxgk.DxgkCbQueueDpc(a->device_handle);
    return STATUS_SUCCESS;
}
#endif

static NTSTATUS APIENTRY WddmCalibrateGpuClock(PVOID context, UINT node_ordinal,
                                               UINT engine_ordinal,
                                               DXGKARG_CALIBRATEGPUCLOCK *arg)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(node_ordinal);
    UNREFERENCED_PARAMETER(engine_ordinal);
    if (!arg)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(arg, sizeof(*arg));
    arg->CpuClockCounter = KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY WddmNotifySurpriseRemoval(
    PVOID context, DXGK_SURPRISE_REMOVAL_TYPE type)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(type);
    return STATUS_SUCCESS;
}

/* ---- DriverEntry ------------------------------------------------------- */

static void FillCommonDdi(DRIVER_INITIALIZATION_DATA *data, ULONG version)
{
    RtlZeroMemory(data, sizeof(*data));
    data->Version = version;
    data->DxgkDdiAddDevice = WddmAddDevice;
    data->DxgkDdiStartDevice = WddmStartDevice;
    data->DxgkDdiStopDevice = WddmStopDevice;
    data->DxgkDdiRemoveDevice = WddmRemoveDevice;
    data->DxgkDdiUnload = WddmUnload;
    data->DxgkDdiResetDevice = WddmResetDevice;
    data->DxgkDdiDispatchIoRequest = WddmDispatchIoRequest;
    data->DxgkDdiQueryChildRelations = WddmQueryChildRelations;
    data->DxgkDdiQueryChildStatus = WddmQueryChildStatus;
    data->DxgkDdiQueryDeviceDescriptor = WddmQueryDeviceDescriptor;
    data->DxgkDdiQueryAdapterInfo = WddmQueryAdapterInfo;
    data->DxgkDdiCreateDevice = WddmCreateDevice;
    data->DxgkDdiDestroyDevice = WddmDestroyDevice;
    data->DxgkDdiCreateAllocation = WddmCreateAllocation;
    data->DxgkDdiDestroyAllocation = WddmDestroyAllocation;
    data->DxgkDdiOpenAllocation = WddmOpenAllocation;
    data->DxgkDdiCloseAllocation = WddmCloseAllocation;
    data->DxgkDdiGetStandardAllocationDriverData =
        WddmGetStandardAllocationDriverData;
    data->DxgkDdiDescribeAllocation = WddmDescribeAllocation;
    data->DxgkDdiCreateContext = WddmCreateContext;
    data->DxgkDdiDestroyContext = WddmDestroyContext;
    data->DxgkDdiRender = WddmRender;
    data->DxgkDdiRenderKm = WddmRender;
    data->DxgkDdiPresent = WddmPresent;
    data->DxgkDdiPatch = WddmPatch;
    data->DxgkDdiBuildPagingBuffer = WddmBuildPagingBuffer;
    data->DxgkDdiPreemptCommand = WddmPreemptCommand;
    data->DxgkDdiSubmitCommand = WddmSubmitCommand;
    data->DxgkDdiQueryCurrentFence = WddmQueryCurrentFence;
    data->DxgkDdiInterruptRoutine = WddmInterruptRoutine;
    data->DxgkDdiDpcRoutine = WddmDpcRoutine;
    data->DxgkDdiSetPowerState = WddmSetPowerState;
    data->DxgkDdiResetFromTimeout = WddmResetFromTimeout;
    data->DxgkDdiRestartFromTimeout = WddmRestartFromTimeout;
    data->DxgkDdiEscape = WddmEscape;
    data->DxgkDdiCollectDbgInfo = WddmCollectDbgInfo;
    data->DxgkDdiQueryEngineStatus = WddmQueryEngineStatus;
    data->DxgkDdiResetEngine = WddmResetEngine;
    data->DxgkDdiQueryDependentEngineGroup = WddmQueryDependentEngineGroup;
    data->DxgkDdiSetPowerComponentFState = WddmSetPowerComponentFState;
    data->DxgkDdiControlInterrupt = WddmControlInterrupt;
    data->DxgkDdiControlInterrupt2 = WddmControlInterrupt2;
    data->DxgkDdiCancelCommand = WddmCancelCommand;
    data->DxgkDdiQueryInterface = WddmQueryInterface;
    data->DxgkDdiNotifySurpriseRemoval = WddmNotifySurpriseRemoval;
    data->DxgkDdiGetNodeMetadata = WddmGetNodeMetadata;
    data->DxgkDdiCalibrateGpuClock = WddmCalibrateGpuClock;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    if (version >= DXGKDDI_INTERFACE_VERSION_WDDM2_0) {
        data->DxgkDdiFormatHistoryBuffer = WddmFormatHistoryBuffer;
        data->DxgkDdiSetStablePowerState = WddmSetStablePowerState;
        data->DxgkDdiCreateProcess = WddmCreateProcess;
        data->DxgkDdiDestroyProcess = WddmDestroyProcess;
        data->DxgkDdiGetRootPageTableSize = WddmGetRootPageTableSize;
        data->DxgkDdiSetRootPageTable = WddmSetRootPageTable;
        data->DxgkDdiSubmitCommandVirtual = WddmSubmitCommandVirtual;
    }
#endif
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driver,
                     _In_ PUNICODE_STRING registry_path)
{
    DRIVER_INITIALIZATION_DATA data;
    NTSTATUS status;

    WLog("DriverEntry starting (staged v1.0.0.23)");

    /* Try WDDM 2.0 (GpuMmu + D3D12) first, with clean fallback to WDDM 1.3 */
    g_wddm_version = DXGKDDI_INTERFACE_VERSION_WDDM2_0;
    FillCommonDdi(&data, g_wddm_version);
    status = DxgkInitialize(driver, registry_path, &data);
    WLog("DxgkInitialize(WDDM2_0=0x%x) -> 0x%08x", g_wddm_version, status);

    if (!NT_SUCCESS(status)) {
        g_wddm_version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
        FillCommonDdi(&data, g_wddm_version);
        status = DxgkInitialize(driver, registry_path, &data);
        WLog("DxgkInitialize(WDDM1_3=0x%x) -> 0x%08x", g_wddm_version, status);
    }

    return status;
}
