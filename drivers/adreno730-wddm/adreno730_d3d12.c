/* SPDX-License-Identifier: MIT */
/*
 * adreno730_d3d12.dll - D3D12 UMD for the Adreno 730 bring-up stack.
 *
 * Implemented here:
 *   - adapter open + version/caps reporting
 *   - device creation
 *   - heaps/resources backed by KMD ALLOC/SUBMIT_IB_LIST IOCTLs
 *   - command pools/lists, ExecuteCommandLists
 *   - D3D12 clear (UAV), CopyBufferRegion, WriteBufferImmediate
 *   - fences backed by KMD fence waits (CPU-synchronous first step)
 *
 * Not implemented yet: shaders, pipelines, descriptors, render targets,
 * textures, presentation. Those tables remain NULL/stubs on purpose; see
 * docs/UMD-D3D12.md. The backend is src/common/d3d12.c, shared with the
 * host simulator and the diagnostic tool.
 */
#include <windows.h>
#include <d3d12umddi.h>
#include <dxgiddi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "adreno/adreno_d3d12.h"
#include "adreno/adreno_a730_3d.h"
#include "adreno/adreno_dxbc_ir3.h"

#define UMD_MAX_RESOURCES 256

typedef struct UMD_ADAPTER {
    D3D12DDI_HRTADAPTER rt_adapter;
    const D3DDDI_ADAPTERCALLBACKS *callbacks;
    adreno_kmd_ops ops;
    BOOL has_ops;
    D3D12DDI_HRTTABLE cmdlist_rt_table;
} UMD_ADAPTER;

typedef struct UMD_DEVICE UMD_DEVICE;

typedef struct UMD_HEAP {
    UMD_DEVICE *device;
    adreno_d3d12_resource bo;
    BOOL owns_bo;
} UMD_HEAP;

typedef struct UMD_RESOURCE {
    UMD_DEVICE *device;
    adreno_d3d12_resource bo; /* base allocation (may be shared with a heap) */
    uint64_t bo_offset;       /* resource start inside bo */
    uint64_t gpu_va;          /* iova + bo_offset */
    uint32_t size;            /* resource size in bytes */
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    DXGI_FORMAT format;
    BOOL texture2d;
    BOOL owns_bo;
} UMD_RESOURCE;

#define UMD_DESCRIPTOR_SIZE 32u
#define UMD_UAV_DESC_MAGIC  0x55415644u /* "UAVD" */
#define UMD_RTV_DESC_MAGIC  0x52545644u /* "RTVD" */
#define UMD_DSV_DESC_MAGIC  0x44535644u /* "DSVD" */
#define UMD_SRV_DESC_MAGIC  0x53525644u /* "SRVD" */
#define UMD_SAMP_DESC_MAGIC 0x534D5044u /* "SMPD" */
#define UMD_CBV_DESC_MAGIC  0x43425644u /* "CBVD" */
#define UMD_REF_TRIANGLE_MAGIC 0x54524441u /* "ADRT" */

typedef struct UMD_UAV_DESC {
    uint32_t magic;
    uint32_t format;
    uint64_t gpu_va;
    uint64_t size;
    uint64_t reserved;
} UMD_UAV_DESC;

typedef struct UMD_RTV_DESC {
    uint32_t magic;
    uint32_t format;
    uint64_t gpu_va;
    uint64_t size;
    uint64_t resource;
} UMD_RTV_DESC;

typedef struct UMD_SRV_DESC {
    uint32_t magic;
    uint32_t format;
    uint64_t gpu_va;
    uint64_t size;
    uint64_t resource;
} UMD_SRV_DESC;

typedef struct UMD_SAMP_DESC {
    uint32_t magic;
    uint32_t reserved0;
    uint32_t dw[4];
    uint32_t reserved1[2];
} UMD_SAMP_DESC;

typedef struct UMD_CBV_DESC {
    uint32_t magic;
    uint32_t reserved0;
    uint64_t gpu_va;
    uint64_t size;
    uint64_t reserved1;
} UMD_CBV_DESC;

typedef struct UMD_CMDLIST {
    UMD_DEVICE *device;
    D3D12DDI_HRTCOMMANDLIST h_rt_cmdlist;
    adreno_d3d12_cmdlist cmd;
    adreno_d3d12_resource tex_desc_bo;
    uint32_t tex_desc_slot;
    BOOL initialized;
    BOOL closed;
    void *pso;
    void *root_signature;
    void *descriptor_heap;
    void *sampler_heap;
    uint64_t compute_uav_va;
    uint32_t compute_constants[4];
    BOOL has_compute_uav;
    void *rtv;
    D3D12DDI_VIEWPORT viewport;
    BOOL has_viewport;
    D3D12DDI_RECT scissor;
    BOOL has_scissor;
    uint32_t prim_type;
    D3D12DDI_VERTEX_BUFFER_VIEW vbufs[ADRENO_3D_MAX_VBUFS];
    UINT num_vbufs;
    D3D12DDI_INDEX_BUFFER_VIEW ibuf;
    BOOL has_ibuf;
    uint32_t gfx_constants[16];
    uint32_t gfx_const_vec4s;
    UMD_SRV_DESC gfx_srvs[ADRENO_3D_MAX_TEXTURES];
    uint32_t gfx_srv_mask;
    UMD_SAMP_DESC gfx_samplers[ADRENO_3D_MAX_SAMPLERS];
    uint32_t gfx_samp_mask;
} UMD_CMDLIST;

typedef struct UMD_POOL {
    UMD_DEVICE *device;
} UMD_POOL;

typedef struct UMD_QUEUE {
    UMD_DEVICE *device;
    uint32_t last_seqno;
} UMD_QUEUE;

typedef struct UMD_FENCE {
    UMD_DEVICE *device;
    UINT64 value;
    UINT64 pending_value;
    uint32_t pending_seqno;
    uint64_t value_va;
    uint64_t monitored_va;
} UMD_FENCE;

typedef struct UMD_WDDM_BRIDGE {
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050 *core;
    const D3DDDI_DEVICECALLBACKS *kt;
    D3D12DDI_HRTDEVICE h_rt_device;
    HANDLE h_device;
    HANDLE h_context;
    D3D12DDI_HRTCOMMANDQUEUE h_rt_queue;
    HANDLE h_hw_queue;
    VOID *hw_fence_cpu;
    D3DGPU_VIRTUAL_ADDRESS hw_fence_gpu;
    BOOL use_hw_queue;
    D3DKMT_HANDLE h_paging_queue;
    D3DKMT_HANDLE h_paging_sync;
    volatile UINT64 *paging_fence_cpu;
    D3DKMT_HANDLE h_submit_sync;
    volatile UINT64 *submit_fence_cpu;
    UINT64 submit_fence_val;
    uint32_t last_seqno;
    HANDLE pending_rt_resource;
    BOOL pending_primary;
    uint32_t pending_w;
    uint32_t pending_h;
    uint32_t pending_pitch;
    uint32_t pending_fmt;
    BOOL pending_tex2d;
} UMD_WDDM_BRIDGE;

struct UMD_DEVICE {
    UMD_ADAPTER *adapter;
    D3D12DDI_HRTDEVICE rt_device;
    const D3DDDI_DEVICECALLBACKS *kt_callbacks;
    adreno_d3d12_device gpu;
    BOOL gpu_ready;
    BOOL allocated;
    BOOL wddm_bridge;
    UMD_WDDM_BRIDGE wddm;
    UMD_RESOURCE *resources[UMD_MAX_RESOURCES];
    UINT resource_count;
    UINT32 next_pso_id;
};

typedef struct UMD_DESCRIPTOR_HEAP {
    UMD_DEVICE *device;
    D3D12DDI_DESCRIPTOR_HEAP_TYPE type;
    UINT num_descriptors;
    UINT descriptor_size;
    adreno_d3d12_resource bo;
    BOOL initialized;
} UMD_DESCRIPTOR_HEAP;

/* Pipeline / shader / element-layout state for native Adreno 730 IR3 execution. */
typedef struct UMD_ELEMENT_LAYOUT {
    UINT num_elements;
    D3D12DDIARG_INPUT_ELEMENT_DESC elements[ADRENO_3D_MAX_VATTRS];
} UMD_ELEMENT_LAYOUT;

typedef struct UMD_SHADER {
    uint32_t magic;
    uint32_t bytes;      /* total bytes copied after this struct */
    uint32_t code_bytes; /* shader code bytes (IR3 payload or DXBC) */
    BOOL has_ir3;
    BOOL has_dxbc_ir3;
    BOOL reference_triangle;
    adreno_d3d12_ir3_meta meta;
    adreno_dxbc_compiled *compiled;
} UMD_SHADER;

#define UMD_MAX_ROOT_PARAMS 16

typedef struct UMD_ROOT_PARAM {
    uint32_t type;
    uint32_t shader_register;
    uint32_t register_space;
    uint32_t num_32bit_values;
    uint32_t table_range_type;
    uint32_t table_base_reg;
    uint32_t table_num_descs;
} UMD_ROOT_PARAM;

typedef struct UMD_STATIC_SAMP {
    uint32_t shader_register;
    uint32_t dw[4];
} UMD_STATIC_SAMP;

typedef struct UMD_ROOTSIG {
    uint32_t version;
    UINT node_mask;
    UINT num_params;
    UMD_ROOT_PARAM params[UMD_MAX_ROOT_PARAMS];
    UINT num_static_samplers;
    UMD_STATIC_SAMP static_samplers[ADRENO_3D_MAX_SAMPLERS];
} UMD_ROOTSIG;

typedef struct UMD_PSO {
    uint32_t id;
    BOOL compute;
    BOOL reference_triangle;
    BOOL has_hw_gfx;
    DXGI_FORMAT rtv_format;
    UMD_SHADER *shader;
    UMD_SHADER *vs;
    UMD_SHADER *ps;
    UMD_ELEMENT_LAYOUT *layout;
    UMD_ROOTSIG *root_signature;
    BOOL has_ir3;
    adreno_d3d12_resource shader_bo;
    adreno_d3d12_resource vs_bo;
    adreno_d3d12_resource ps_bo;
    adreno_d3d12_ir3_meta meta;
} UMD_PSO;

#include <stdio.h>
#include <stdarg.h>
static void umd_trace(const char *fmt, ...)
{
    static int trace_enabled = -1;
    FILE *f;
    char buf[512];
    va_list ap;
    if (trace_enabled < 0) {
        char env[8];
        trace_enabled = (GetEnvironmentVariableA("ADRENO_UMD_TRACE", env, sizeof(env)) > 0 && env[0] == '1') ? 1 : 0;
    }
    if (!trace_enabled)
        return;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    if (fopen_s(&f, "C:\\Temp\\adreno730_umd.log", "a") == 0 && f) {
        fprintf(f, "[pid=%lu] %s\n", (unsigned long)GetCurrentProcessId(), buf);
        fflush(f);
        fclose(f);
    }
}
static void umd_log(const char *message)
{
    umd_trace("%s", message);
}

static void AdrenoArm64FlushDcache(const void *ptr, size_t bytes)
{
    /* Windows ARM64 traps EL0 dc cvac/civac (STATUS_ILLEGAL_INSTRUCTION).
     * All WDDM2 allocations use Cached=0 (Write-Combined), so a full memory
     * barrier drains the ARM64 write-combine buffers to DDR cleanly. */
    (void)ptr;
    (void)bytes;
    MemoryBarrier();
}

static void umd_flush_all_resources(UMD_DEVICE *dev)
{
    UINT i;
    if (!dev)
        return;
    for (i = 0; i < dev->resource_count; i++) {
        UMD_RESOURCE *r = dev->resources[i];
        if (r && r->bo.host && r->bo.size)
            AdrenoArm64FlushDcache(r->bo.host, r->bo.size);
    }
}

static UMD_DEVICE *umd_device(D3D12DDI_HDEVICE handle)
{
    return (UMD_DEVICE *)handle.pDrvPrivate;
}

static UMD_RESOURCE *umd_resource(D3D12DDI_HRESOURCE handle)
{
    return (UMD_RESOURCE *)handle.pDrvPrivate;
}

static int umd_device_add_resource(UMD_DEVICE *dev, UMD_RESOURCE *res)
{
    UINT i;
    for (i = 0; i < dev->resource_count; i++) {
        if (dev->resources[i] == res)
            return 1;
    }
    if (dev->resource_count >= UMD_MAX_RESOURCES)
        return 0;
    dev->resources[dev->resource_count++] = res;
    return 1;
}

static void umd_device_remove_resource(UMD_DEVICE *dev, UMD_RESOURCE *res)
{
    UINT i;
    for (i = 0; i < dev->resource_count; i++) {
        if (dev->resources[i] == res) {
            dev->resources[i] = dev->resources[--dev->resource_count];
            return;
        }
    }
}

static UMD_RESOURCE *umd_find_va(UMD_DEVICE *dev, uint64_t va, uint32_t size)
{
    UINT i;
    for (i = 0; i < dev->resource_count; i++) {
        UMD_RESOURCE *res = dev->resources[i];
        if (va >= res->gpu_va && va + size <= res->gpu_va + res->size)
            return res;
    }
    return NULL;
}

static uint32_t umd_resource_data_size(
    const D3D12DDIARG_CREATERESOURCE_0088 *resource)
{
    uint64_t row_pitch;

    if (!resource)
        return 0;
    if (resource->ResourceType == D3D12DDI_RT_BUFFER) {
        if (resource->Width == 0 || resource->Width > 256ull * 1024 * 1024)
            return 0;
        return (uint32_t)resource->Width;
    }
    if (resource->ResourceType == D3D12DDI_RT_TEXTURE2D) {
        uint32_t bpp = 4u;
        if (resource->Width == 0 || resource->Width > 16384 ||
            resource->Height == 0 || resource->Height > 16384 ||
            resource->DepthOrArraySize != 1 || resource->MipLevels > 16 ||
            resource->SampleDesc.Count != 1 ||
            (resource->Layout != D3D12DDI_TL_ROW_MAJOR &&
             resource->Layout != D3D12DDI_TL_UNDEFINED))
            return 0;
        switch (resource->Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_D32_FLOAT:
            bpp = 4u;
            break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            bpp = 8u;
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            bpp = 16u;
            break;
        default:
            return 0;
        }
        row_pitch = (resource->Width * (uint64_t)bpp + 255ull) & ~255ull;
        if (row_pitch * resource->Height > 256ull * 1024 * 1024)
            return 0;
        return (uint32_t)(row_pitch * resource->Height);
    }
    return 0;
}

/* ---- WDDM runtime bridge ------------------------------------------------ */

static void umd_wddm_free_handle(UMD_WDDM_BRIDGE *b, uint32_t handle)
{
    D3D12DDICB_DEALLOCATE_0022 dealloc;
    D3DKMT_HANDLE h = (D3DKMT_HANDLE)handle;

    if (!b || !b->core || !b->core->pfnDeallocateCb || handle == 0)
        return;
    memset(&dealloc, 0, sizeof(dealloc));
    dealloc.NumAllocations = 1;
    dealloc.HandleList = &h;
    dealloc.Flags = D3D12DDI_DEALLOCATE_FLAGS_0022_NONE;
    (void)b->core->pfnDeallocateCb(b->h_rt_device, &dealloc);
}

static enum adreno_status umd_wddm_alloc(void *ctx, uint32_t size,
                                         uint32_t *handle, uint64_t *iova,
                                         void **host)
{
    UMD_WDDM_BRIDGE *b = (UMD_WDDM_BRIDGE *)ctx;
    D3D12DDI_ALLOCATION_INFO_0022 info;
    D3D12DDICB_ALLOCATE_0022 alloc;
    adreno_wddm_alloc_private priv;
    uint32_t aligned_size;
    HRESULT hr;

    if (!b || !b->core || !b->core->pfnAllocateCb || !handle || !iova ||
        !host || size == 0)
        return ADRENO_ERR_INVALID;
    aligned_size = (size + 4095u) & ~4095u;
    memset(&info, 0, sizeof(info));
    memset(&alloc, 0, sizeof(alloc));
    memset(&priv, 0, sizeof(priv));
    priv.magic = ADRENO_WDDM_ALLOC_PRIVATE_MAGIC;
    priv.size = aligned_size;
    priv.width = b->pending_w;
    priv.height = b->pending_h;
    priv.row_pitch = b->pending_pitch;
    priv.format = b->pending_fmt;
    priv.texture2d = b->pending_tex2d ? 1u : 0u;
    info.pPrivateDriverData = &priv;
    info.PrivateDriverDataSize = sizeof(priv);
    if (b->pending_primary) {
        info.Flags = D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY;
        info.VidPnSourceId = 0;
    }
    alloc.pPrivateDriverData = &priv;
    alloc.PrivateDriverDataSize = sizeof(priv);
    alloc.hResource = b->pending_rt_resource;
    alloc.NumAllocations = 1;
    alloc.pAllocationInfo = &info;

    hr = b->core->pfnAllocateCb(b->h_rt_device, &alloc);
    umd_trace("umd_wddm_alloc(sz=%u hRT=%p pri=%d) AllocateCb hr=0x%08lx h=0x%x hKM=0x%x va=0x%llx",
              aligned_size, b->pending_rt_resource, (int)b->pending_primary,
              (unsigned long)hr, (unsigned)info.hAllocation, (unsigned)alloc.hKMResource,
              (unsigned long long)info.GpuVirtualAddress);
    if (FAILED(hr) || info.hAllocation == 0)
        return ADRENO_ERR_NOMEM;

    if (b->h_paging_queue && b->kt && b->kt->pfnMakeResidentCb &&
        b->kt->pfnMapGpuVirtualAddressCb && b->kt->pfnLock2Cb) {
        D3DDDI_MAKERESIDENT mr;
        D3DDDI_MAPGPUVIRTUALADDRESS mva;
        D3DDDICB_LOCK2 lock2;
        D3DKMT_HANDLE h_alloc = info.hAllocation;
        UINT64 target_fence = 0;

        memset(&mr, 0, sizeof(mr));
        mr.hPagingQueue = b->h_paging_queue;
        mr.NumAllocations = 1;
        mr.AllocationList = &h_alloc;
        hr = b->kt->pfnMakeResidentCb(b->h_device, &mr);
        if (FAILED(hr) && hr != E_PENDING) {
            umd_trace("  MakeResidentCb failed hr=0x%08lx", (unsigned long)hr);
            umd_wddm_free_handle(b, info.hAllocation);
            return ADRENO_ERR_NOMEM;
        }
        if (mr.PagingFenceValue > target_fence)
            target_fence = mr.PagingFenceValue;

        memset(&mva, 0, sizeof(mva));
        mva.hPagingQueue = b->h_paging_queue;
        mva.MinimumAddress = 0x20000000ull;
        mva.MaximumAddress = 0x50000000ull;
        mva.hAllocation = info.hAllocation;
        mva.OffsetInPages = 0;
        mva.SizeInPages = aligned_size >> 12;
        mva.Protection.Write = 1;
        hr = b->kt->pfnMapGpuVirtualAddressCb(b->h_device, &mva);
        if ((FAILED(hr) && hr != E_PENDING) || mva.VirtualAddress == 0) {
            umd_trace("  MapGpuVirtualAddressCb failed hr=0x%08lx va=0x%llx",
                      (unsigned long)hr, (unsigned long long)mva.VirtualAddress);
            umd_wddm_free_handle(b, info.hAllocation);
            return ADRENO_ERR_IO;
        }
        if (mva.PagingFenceValue > target_fence)
            target_fence = mva.PagingFenceValue;

        if (b->paging_fence_cpu && target_fence > 0) {
            uint64_t t0 = GetTickCount64();
            while (*b->paging_fence_cpu < target_fence) {
                if (GetTickCount64() - t0 > 2000)
                    break;
                Sleep(0);
            }
        }

        memset(&lock2, 0, sizeof(lock2));
        lock2.hAllocation = info.hAllocation;
        hr = b->kt->pfnLock2Cb(b->h_device, &lock2);
        if (FAILED(hr) || !lock2.pData) {
            umd_trace("  Lock2Cb failed hr=0x%08lx", (unsigned long)hr);
            umd_wddm_free_handle(b, info.hAllocation);
            return ADRENO_ERR_IO;
        }
        umd_trace("  WDDM2 alloc OK: h=0x%x va=0x%llx cpu=%p pfence=%llu/%llu",
                  (unsigned)info.hAllocation, (unsigned long long)mva.VirtualAddress,
                  lock2.pData,
                  b->paging_fence_cpu ? (unsigned long long)*b->paging_fence_cpu : 0ull,
                  (unsigned long long)target_fence);
        *handle = info.hAllocation;
        *iova = mva.VirtualAddress;
        *host = lock2.pData;
        return ADRENO_OK;
    }

    if (info.pSystemMem) {
        *host = (void *)(uintptr_t)info.pSystemMem;
    } else if (b->kt && b->kt->pfnLockCb) {
        D3DDDICB_LOCK lock;
        memset(&lock, 0, sizeof(lock));
        lock.hAllocation = info.hAllocation;
        hr = b->kt->pfnLockCb(b->h_device, &lock);
        if (FAILED(hr) || !lock.pData) {
            umd_wddm_free_handle(b, info.hAllocation);
            return ADRENO_ERR_IO;
        }
        *host = lock.pData;
        if (info.GpuVirtualAddress == 0)
            info.GpuVirtualAddress = lock.GpuVirtualAddress;
    } else {
        umd_wddm_free_handle(b, info.hAllocation);
        return ADRENO_ERR_UNSUPPORTED;
    }
    if (info.GpuVirtualAddress == 0) {
        umd_wddm_free_handle(b, info.hAllocation);
        return ADRENO_ERR_UNSUPPORTED;
    }
    *handle = info.hAllocation;
    *iova = info.GpuVirtualAddress;
    return ADRENO_OK;
}

static enum adreno_status umd_wddm_free(void *ctx, uint32_t handle)
{
    UMD_WDDM_BRIDGE *b = (UMD_WDDM_BRIDGE *)ctx;

    if (!b || handle == 0)
        return ADRENO_ERR_INVALID;
    umd_wddm_free_handle(b, handle);
    return ADRENO_OK;
}

static enum adreno_status umd_wddm_submit(void *ctx, const adreno_ib *ibs,
                                          uint32_t count, uint32_t seqno)
{
    UMD_WDDM_BRIDGE *b = (UMD_WDDM_BRIDGE *)ctx;
    uint32_t i;

    if (!b || !ibs || count == 0)
        return ADRENO_ERR_INVALID;

    if (b->use_hw_queue && b->kt && b->kt->pfnSubmitCommandToHwQueueCb) {
        for (i = 0; i < count; i++) {
            D3DDDICB_SUBMITCOMMANDTOHWQUEUE submit;
            HRESULT hr;
            if (ibs[i].size_dwords == 0)
                continue;
            memset(&submit, 0, sizeof(submit));
            submit.hHwQueue = b->h_hw_queue;
            submit.HwQueueProgressFenceId = seqno;
            submit.Commands = ibs[i].iova;
            submit.CommandLength = ibs[i].size_dwords * 4u;
            hr = b->kt->pfnSubmitCommandToHwQueueCb(b->h_device, &submit);
            if (FAILED(hr))
                return ADRENO_ERR_IO;
        }
        b->last_seqno = seqno;
        return ADRENO_OK;
    }

    if (!b->kt || !b->kt->pfnSubmitCommandCb || !b->h_context)
        return ADRENO_ERR_UNSUPPORTED;
    for (i = 0; i < count; i++) {
        D3DDDICB_SUBMITCOMMAND submit;
        HRESULT hr;
        if (ibs[i].size_dwords == 0)
            continue;
        memset(&submit, 0, sizeof(submit));
        submit.Commands = ibs[i].iova;
        submit.CommandLength = ibs[i].size_dwords * 4u;
        submit.BroadcastContextCount = 1;
        submit.BroadcastContext[0] = b->h_context;
        submit.pPrivateDriverData = &seqno;
        submit.PrivateDriverDataSize = sizeof(seqno);
        hr = b->kt->pfnSubmitCommandCb(b->h_device, &submit);
        umd_trace("umd_wddm_submit: va=0x%llx bytes=%u seq=%u -> hr=0x%08lx",
                  (unsigned long long)ibs[i].iova, ibs[i].size_dwords * 4u, seqno, (unsigned long)hr);
        if (FAILED(hr))
            return ADRENO_ERR_IO;
    }
    if (b->h_submit_sync && b->kt->pfnSignalSynchronizationObjectFromGpuCb) {
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU sig;
        UINT64 next_val = ++b->submit_fence_val;
        memset(&sig, 0, sizeof(sig));
        sig.hContext = b->h_context;
        sig.ObjectCount = 1;
        sig.ObjectHandleArray = &b->h_submit_sync;
        sig.MonitoredFenceValueArray = &next_val;
        (void)b->kt->pfnSignalSynchronizationObjectFromGpuCb(b->h_device, &sig);
    }
    b->last_seqno = seqno;
    return ADRENO_OK;
}

static enum adreno_status umd_wddm_wait(void *ctx, uint32_t seqno,
                                        uint32_t timeout_ms)
{
    UMD_WDDM_BRIDGE *b = (UMD_WDDM_BRIDGE *)ctx;

    if (!b || seqno == 0)
        return ADRENO_ERR_INVALID;
    if (b->use_hw_queue && b->hw_fence_cpu) {
        volatile uint64_t *fence = (volatile uint64_t *)b->hw_fence_cpu;
        uint64_t start = GetTickCount64();
        while (*fence < seqno) {
            if (GetTickCount64() - start >= timeout_ms)
                return ADRENO_ERR_TIMEOUT;
            Sleep(1);
        }
        return ADRENO_OK;
    }
    if (b->submit_fence_cpu && b->submit_fence_val > 0) {
        uint64_t start = GetTickCount64();
        while (*b->submit_fence_cpu < b->submit_fence_val) {
            if (GetTickCount64() - start >= timeout_ms)
                return ADRENO_ERR_TIMEOUT;
            Sleep(1);
        }
        return ADRENO_OK;
    }
    return ADRENO_OK;
}

static adreno_kmd_ops umd_wddm_ops(UMD_WDDM_BRIDGE *b)
{
    adreno_kmd_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.ctx = b;
    ops.alloc = umd_wddm_alloc;
    ops.free = umd_wddm_free;
    ops.submit = umd_wddm_submit;
    ops.wait = umd_wddm_wait;
    return ops;
}

/* ---- Adapter ------------------------------------------------------------ */

static const UINT64 supported_versions[] = {D3D12DDI_SUPPORTED_0092};

static HRESULT APIENTRY UmdGetSupportedVersions(
    D3D12DDI_HADAPTER hAdapter, UINT32 *puEntries,
    UINT64 *pSupportedDDIInterfaceVersions)
{
    (void)hAdapter;
    if (!puEntries)
        return E_INVALIDARG;
    umd_trace("GetSupportedVersions(val=%u out=%p)", *puEntries, (void*)pSupportedDDIInterfaceVersions);
    if (!pSupportedDDIInterfaceVersions) {
        *puEntries = (UINT32)(sizeof(supported_versions) /
                              sizeof(supported_versions[0]));
        return S_OK;
    }
    if (*puEntries < 1)
        return E_INVALIDARG;
    pSupportedDDIInterfaceVersions[0] = supported_versions[0];
    *puEntries = 1;
    return S_OK;
}

static HRESULT APIENTRY UmdGetCaps(D3D12DDI_HADAPTER hAdapter,
                                   const D3D12DDIARG_GETCAPS *caps)
{
    (void)hAdapter;
    umd_trace("GetCaps(type=%u dataSize=%u pData=%p)", caps ? (unsigned)caps->Type : 9999u, caps ? (unsigned)caps->DataSize : 0u, caps ? caps->pData : NULL);
    if (!caps || !caps->pData)
        return E_INVALIDARG;

    switch (caps->Type) {
    case D3D12DDICAPS_TYPE_ARCHITECTURE_INFO: {
        D3D12DDI_ARCHITECTURE_INFO_DATA *info;
        if (caps->DataSize < sizeof(*info))
            return E_INVALIDARG;
        info = (D3D12DDI_ARCHITECTURE_INFO_DATA *)caps->pData;
        info->TileBasedDeferredRenderer = TRUE;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT: {
        D3D12DDI_3DPIPELINELEVEL *level;
        if (caps->DataSize < sizeof(*level))
            return E_INVALIDARG;
        level = (D3D12DDI_3DPIPELINELEVEL *)caps->pData;
        *level = D3D12DDI_3DPIPELINELEVEL_11_0;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_GPUVA_CAPS: {
        D3D12DDI_GPUVA_CAPS_0004 *gpuva;
        if (caps->DataSize < sizeof(*gpuva))
            return E_INVALIDARG;
        gpuva = (D3D12DDI_GPUVA_CAPS_0004 *)caps->pData;
        gpuva->MaxGPUVirtualAddressBitsPerResource = 39;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS: {
        D3D12DDI_D3D12_OPTIONS_DATA_0089 options;
        UINT copy = caps->DataSize;
        memset(&options, 0, sizeof(options));
        options.ResourceBindingTier = D3D12DDI_RESOURCE_BINDING_TIER_1;
        options.ConservativeRasterizationTier =
            D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
        options.TiledResourcesTier =
            D3D12DDI_TILED_RESOURCES_TIER_NOT_SUPPORTED;
        options.CrossNodeSharingTier =
            D3D12DDI_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
        options.ResourceHeapTier = D3D12DDI_RESOURCE_HEAP_TIER_1;
        options.ProgrammableSamplePositionsTier =
            D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
        options.WriteBufferImmediateQueueFlags =
            D3D12DDI_COMMAND_QUEUE_FLAG_NONE;
        if (copy > sizeof(options))
            copy = sizeof(options);
        if (copy < sizeof(D3D12DDI_D3D12_OPTIONS_DATA_0032))
            return E_INVALIDARG;
        memcpy(caps->pData, &options, copy);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1: {
        D3D12DDI_3DPIPELINESUPPORT1_DATA_0081 *p;
        if (caps->DataSize < sizeof(*p))
            return E_INVALIDARG;
        p = (D3D12DDI_3DPIPELINESUPPORT1_DATA_0081 *)caps->pData;
        p->MaximumDriverSupportedFeatureLevel = D3D12DDI_3DPIPELINELEVEL_11_0;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE: {
        D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041 mem;
        UINT copy = caps->DataSize;
        memset(&mem, 0, sizeof(mem));
        mem.UMA = TRUE;
        mem.IOCoherent = TRUE;
        mem.CacheCoherent = TRUE;
        if (copy > sizeof(mem))
            copy = sizeof(mem);
        memset(caps->pData, 0, caps->DataSize);
        memcpy(caps->pData, &mem, copy);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_SHADER: {
        D3D12DDI_SHADER_CAPS_0084 sc;
        UINT copy = caps->DataSize;
        memset(&sc, 0, sizeof(sc));
        sc.MinPrecision = D3D12DDI_SHADER_MIN_PRECISION_16_BIT;
        sc.WaveOps = TRUE;
        sc.WaveLaneCountMin = 64;
        sc.WaveLaneCountMax = 64;
        sc.TotalLaneCount = 512;
        if (copy > sizeof(sc))
            copy = sizeof(sc);
        memset(caps->pData, 0, caps->DataSize);
        memcpy(caps->pData, &sc, copy);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0011_SHADER_MODELS: {
        D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 *sm;
        if (caps->DataSize < sizeof(*sm))
            return E_INVALIDARG;
        sm = (D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 *)caps->pData;
        if (sm->pNumShaderModelsSupported) {
            if (!sm->pShaderModelsSupported) {
                *sm->pNumShaderModelsSupported = 1;
            } else if (*sm->pNumShaderModelsSupported >= 1) {
                sm->pShaderModelsSupported[0] = D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011;
                *sm->pNumShaderModelsSupported = 1;
            } else {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
        }
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT: {
        D3D12DDI_TEXTURE_LAYOUT_CAPS_0026 tl;
        UINT copy = caps->DataSize;
        memset(&tl, 0, sizeof(tl));
        tl.SupportsRowMajorTexture = TRUE;
        if (copy > sizeof(tl))
            copy = sizeof(tl);
        memset(caps->pData, 0, caps->DataSize);
        memcpy(caps->pData, &tl, copy);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS: {
        D3D12DDI_ROW_MAJOR_LAYOUT_CAPS rm;
        const UINT *info2 = (const UINT *)caps->pInfo;
        UINT copy = caps->DataSize;
        umd_trace("  TEXTURE_LAYOUT_SETS pInfo=%p [%u, %u]", caps->pInfo, info2 ? info2[0] : 999u, info2 ? info2[1] : 999u);
        memset(&rm, 0, sizeof(rm));
        rm.SubCaps[0].MaxElementSize = 16;
        rm.SubCaps[0].BaseOffsetAlignment = 64;
        rm.SubCaps[0].PitchAlignment = 64;
        rm.SubCaps[0].DepthPitchAlignment = 64;
        rm.Flags = D3D12DDI_ROW_MAJOR_LAYOUT_FLAG_FLEXIBLE_DEPTH_PITCH;
        if (copy > sizeof(rm))
            copy = sizeof(rm);
        memset(caps->pData, 0, caps->DataSize);
        memcpy(caps->pData, &rm, copy);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0022_SWIZZLE_PATTERN:
    case D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES:
    case D3D12DDICAPS_TYPE_0023_UMD_BASED_COMMAND_QUEUE_PRIORITY:
    case D3D12DDICAPS_TYPE_0033_ADAPTER_COMPUTE_ONLY:
    case D3D12DDICAPS_TYPE_0050_HARDWARE_SCHEDULING_CAPS:
    case D3D12DDICAPS_TYPE_EXECUTECOMMANDLISTS_PARALLELISM:
    case D3D12DDICAPS_TYPE_SAMPLER_FEEDBACK_0073:
    case D3D12DDICAPS_TYPE_0073_SUPPORT_BATCHED_MARKERS:
    case D3D12DDICAPS_TYPE_0074_PROTECTED_RESOURCE_SESSION_TYPE_COUNT:
    case D3D12DDICAPS_TYPE_OPTIONS_0090:
    case D3D12DDICAPS_TYPE_OPTIONS_0091:
        memset(caps->pData, 0, caps->DataSize);
        return S_OK;
    default:
        umd_trace("  -> GetCaps UNHANDLED type=%u dataSize=%u -> E_NOTIMPL", (unsigned)caps->Type, (unsigned)caps->DataSize);
        return E_NOTIMPL;
    }
}

static HRESULT APIENTRY UmdGetOptionalDDITables(
    D3D12DDI_HADAPTER hAdapter, UINT32 *puEntries,
    D3D12DDI_TABLE_REQUEST *tables)
{
    (void)hAdapter;
    if (!puEntries)
        return E_INVALIDARG;
    (void)tables;
    umd_trace("GetOptionalDDITables(in=%u)", *puEntries);
    *puEntries = 0;
    return S_OK;
}

/* ---- Device core sizing / creation ------------------------------------- */

static SIZE_T APIENTRY UmdCalcPrivateDeviceSize(
    D3D12DDI_HADAPTER hAdapter,
    const D3D12DDIARG_CALCPRIVATEDEVICESIZE *size)
{
    (void)hAdapter;
    (void)size;
    return sizeof(UMD_DEVICE);
}

static HRESULT APIENTRY UmdCreateDevice(
    D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_CREATEDEVICE_0003 *args)
{
    UMD_ADAPTER *adapter;
    UMD_DEVICE *device;

    umd_trace("CreateDevice(iface=0x%x ver=0x%x kt=%p um0050=%p priv=%p)", args ? args->Interface : 0u, args ? args->Version : 0u, args ? (void*)args->pKTCallbacks : NULL, args ? (void*)args->p12UMCallbacks_0050 : NULL, args ? args->hDrvDevice.pDrvPrivate : NULL);
    if (!hAdapter.pDrvPrivate || !args)
        return E_INVALIDARG;
    adapter = (UMD_ADAPTER *)hAdapter.pDrvPrivate;

    /*
     * The runtime normally provides the private region calculated by
     * CalcPrivateDeviceSize. Accept NULL as well so the UMD can also be
     * driven by a standalone harness without a runtime allocator.
     */
    device = (UMD_DEVICE *)args->hDrvDevice.pDrvPrivate;
    if (device) {
        memset(device, 0, sizeof(*device));
    } else {
        device = (UMD_DEVICE *)calloc(1, sizeof(*device));
        if (!device)
            return E_OUTOFMEMORY;
        device->allocated = TRUE;
    }

    device->adapter = adapter;
    device->rt_device = args->hRTDevice;
    device->kt_callbacks = args->pKTCallbacks;
    if (adapter->has_ops) {
        if (adreno_d3d12_device_init(&device->gpu, &adapter->ops) !=
            ADRENO_OK) {
            if (device->allocated)
                free(device);
            return E_FAIL;
        }
        device->gpu_ready = TRUE;
    } else if (args->p12UMCallbacks_0050 && args->pKTCallbacks) {
        adreno_kmd_ops ops;
        memset(&device->wddm, 0, sizeof(device->wddm));
        device->wddm.core = args->p12UMCallbacks_0050;
        device->wddm.kt = args->pKTCallbacks;
        device->wddm.h_rt_device = args->hRTDevice;
        device->wddm.h_device = (HANDLE)args->hRTDevice.handle;
        if (device->wddm.kt->pfnCreatePagingQueueCb) {
            D3DDDICB_CREATEPAGINGQUEUE pq;
            HRESULT hr_pq;
            memset(&pq, 0, sizeof(pq));
            pq.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
            pq.PhysicalAdapterIndex = 0;
            hr_pq = device->wddm.kt->pfnCreatePagingQueueCb(device->wddm.h_device, &pq);
            if (SUCCEEDED(hr_pq) && pq.hPagingQueue) {
                device->wddm.h_paging_queue = pq.hPagingQueue;
                device->wddm.h_paging_sync = pq.hSyncObject;
                device->wddm.paging_fence_cpu = (volatile UINT64 *)pq.FenceValueCPUVirtualAddress;
                umd_trace("CreatePagingQueueCb OK: h=0x%x sync=0x%x cpu=%p",
                          (unsigned)pq.hPagingQueue, (unsigned)pq.hSyncObject,
                          pq.FenceValueCPUVirtualAddress);
            } else {
                umd_trace("CreatePagingQueueCb FAILED: hr=0x%08lx", (unsigned long)hr_pq);
            }
        }
        if (device->wddm.kt->pfnCreateSynchronizationObject2Cb) {
            D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 so;
            HRESULT hr_so;
            memset(&so, 0, sizeof(so));
            so.Info.Type = D3DDDI_MONITORED_FENCE;
            so.Info.MonitoredFence.InitialFenceValue = 0;
            so.Info.MonitoredFence.EngineAffinity = 1;
            hr_so = device->wddm.kt->pfnCreateSynchronizationObject2Cb(device->wddm.h_device, &so);
            if (SUCCEEDED(hr_so) && so.hSyncObject) {
                device->wddm.h_submit_sync = so.hSyncObject;
                device->wddm.submit_fence_cpu = (volatile UINT64 *)so.Info.MonitoredFence.FenceValueCPUVirtualAddress;
                umd_trace("CreateSyncObj2(MONITORED_FENCE) OK: h=0x%x cpu=%p",
                          (unsigned)so.hSyncObject, so.Info.MonitoredFence.FenceValueCPUVirtualAddress);
            }
        }
        ops = umd_wddm_ops(&device->wddm);
        if (adreno_d3d12_device_init(&device->gpu, &ops) != ADRENO_OK) {
            if (device->allocated)
                free(device);
            return E_FAIL;
        }
        device->wddm_bridge = TRUE;
        device->gpu_ready = TRUE;
        umd_log("adreno730_d3d12: WDDM callback bridge active\n");
    } else {
        umd_log("adreno730_d3d12: no KMD bridge or WDDM callbacks\n");
        if (device->allocated)
            free(device);
        return E_FAIL;
    }

    ((D3D12DDIARG_CREATEDEVICE_0003 *)args)->hDrvDevice.pDrvPrivate = device;
    umd_log("adreno730_d3d12: DDI device created\n");
    return S_OK;
}

static VOID APIENTRY UmdDestroyDevice(D3D12DDI_HDEVICE hDevice)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("DestroyDevice(dev=%p)", (void*)dev);
    UINT i;

    if (!dev)
        return;
    for (i = 0; i < dev->resource_count; i++) {
        UMD_RESOURCE *res = dev->resources[i];
        if (res && res->owns_bo)
            adreno_d3d12_resource_destroy(&res->bo);
        if (res)
            memset(res, 0, sizeof(*res));
    }
    dev->resource_count = 0;
    adreno_d3d12_device_finish(&dev->gpu);
    if (dev->allocated)
        free(dev);
}

static HRESULT APIENTRY UmdCloseAdapter(D3D12DDI_HADAPTER hAdapter)
{
    UMD_ADAPTER *adapter;

    umd_trace("CloseAdapter(priv=%p)", hAdapter.pDrvPrivate);
    if (!hAdapter.pDrvPrivate)
        return E_INVALIDARG;
    adapter = (UMD_ADAPTER *)hAdapter.pDrvPrivate;
    free(adapter);
    return S_OK;
}

/* ---- Heaps and resources ------------------------------------------------ */

static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY
UmdCalcPrivateHeapAndResourceSizes(
    D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATEHEAP_0001 *heap,
    const D3D12DDIARG_CREATERESOURCE_0088 *resource,
    D3D12DDI_HPROTECTEDRESOURCESESSION_0030 protected_session)
{
    D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes;

    (void)hDevice;
    (void)protected_session;
    sizes.Heap = heap ? sizeof(UMD_HEAP) : 0;
    sizes.Resource = resource ? sizeof(UMD_RESOURCE) : 0;
    return sizes;
}

static HRESULT APIENTRY UmdCreateHeapAndResource(
    D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATEHEAP_0001 *heap_desc,
    D3D12DDI_HHEAP hHeap, D3D12DDI_HRTRESOURCE hRTResource,
    const D3D12DDIARG_CREATERESOURCE_0088 *resource_desc,
    const D3D12DDI_CLEAR_VALUES *clear_values,
    D3D12DDI_HPROTECTEDRESOURCESESSION_0030 protected_session,
    D3D12DDI_HRESOURCE hResource)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    UMD_HEAP *heap = (UMD_HEAP *)hHeap.pDrvPrivate;
    UMD_RESOURCE *res = umd_resource(hResource);
    uint32_t res_size = 0;
    uint64_t heap_size = 0;
    enum adreno_status st;

    umd_trace("CreateHeapAndResource(hRT=%p hflags=0x%x rtype=%u w=%llu h=%u fmt=%u hsz=%llu)",
              hRTResource.handle,
              heap_desc ? (unsigned)heap_desc->Flags : 0u,
              resource_desc ? (unsigned)resource_desc->ResourceType : 99u,
              resource_desc ? (unsigned long long)resource_desc->Width : 0ull,
              resource_desc ? resource_desc->Height : 0u,
              resource_desc ? (unsigned)resource_desc->Format : 0u,
              heap_desc ? (unsigned long long)heap_desc->ByteSize : 0ull);

    (void)clear_values;
    (void)protected_session;

    if (!dev || !dev->gpu_ready || (!heap && !res))
        return E_INVALIDARG;
    if (heap_desc && !heap)
        return E_INVALIDARG;
    if (resource_desc) {
        res_size = umd_resource_data_size(resource_desc);
        if (!res || res_size == 0)
            return E_INVALIDARG; /* textures are not implemented yet */
    }

    if (heap) {
        memset(heap, 0, sizeof(*heap));
        heap->device = dev;
    }
    if (res) {
        memset(res, 0, sizeof(*res));
        res->device = dev;
    }

    if (heap_desc) {
        heap_size = heap_desc->ByteSize;
        if (heap_size == 0 || heap_size == ~0ull)
            heap_size = res_size;
        if (heap_size == 0 || heap_size > 256ull * 1024 * 1024)
            return E_INVALIDARG;
        dev->wddm.pending_rt_resource = hRTResource.handle;
        dev->wddm.pending_primary =
            (heap_desc->Flags & D3D12DDI_HEAP_FLAG_PRIMARY) ? TRUE : FALSE;
        if (resource_desc && resource_desc->ResourceType == D3D12DDI_RT_TEXTURE2D) {
            dev->wddm.pending_tex2d = TRUE;
            dev->wddm.pending_w = (uint32_t)resource_desc->Width;
            dev->wddm.pending_h = resource_desc->Height;
            dev->wddm.pending_pitch =
                (uint32_t)((resource_desc->Width * 4u + 255u) & ~255ull);
            dev->wddm.pending_fmt = (uint32_t)resource_desc->Format;
        } else {
            dev->wddm.pending_tex2d = FALSE;
            dev->wddm.pending_w = 0;
            dev->wddm.pending_h = 0;
            dev->wddm.pending_pitch = 0;
            dev->wddm.pending_fmt = 0;
        }
        st = adreno_d3d12_resource_create(&dev->gpu, (uint32_t)heap_size,
                                          &heap->bo);
        dev->wddm.pending_rt_resource = NULL;
        dev->wddm.pending_primary = FALSE;
        dev->wddm.pending_tex2d = FALSE;
        if (st != ADRENO_OK)
            return E_OUTOFMEMORY;
        heap->owns_bo = TRUE;
    }

    if (resource_desc) {
        uint64_t reuse_offset = 0;
        UMD_RESOURCE *base =
            umd_resource(resource_desc->ReuseBufferGPUVA.BaseAddress.UMD
                             .hResource);

        if (base) {
            res->bo = base->bo;
            reuse_offset =
                resource_desc->ReuseBufferGPUVA.BaseAddress.UMD.Offset;
            res->bo_offset = base->bo_offset + reuse_offset;
        } else if (heap && heap->owns_bo) {
            res->bo = heap->bo;
            res->bo_offset = 0;
        } else {
            st = adreno_d3d12_resource_create(&dev->gpu, res_size,
                                              &res->bo);
            if (st != ADRENO_OK) {
                if (heap && heap->owns_bo)
                    adreno_d3d12_resource_destroy(&heap->bo);
                return E_OUTOFMEMORY;
            }
            res->owns_bo = TRUE;
            res->bo_offset = 0;
        }
        if (res->bo_offset + res_size > res->bo.size) {
            if (res->owns_bo)
                adreno_d3d12_resource_destroy(&res->bo);
            if (heap && heap->owns_bo)
                adreno_d3d12_resource_destroy(&heap->bo);
            return E_INVALIDARG;
        }
        res->size = res_size;
        res->gpu_va = res->bo.iova + res->bo_offset;
        if (resource_desc->ResourceType == D3D12DDI_RT_TEXTURE2D) {
            res->texture2d = TRUE;
            res->width = (uint32_t)resource_desc->Width;
            res->height = resource_desc->Height;
            res->row_pitch =
                (uint32_t)((resource_desc->Width * 4u + 255u) & ~255ull);
            res->format = resource_desc->Format;
        }
        if (!umd_device_add_resource(dev, res)) {
            if (res->owns_bo)
                adreno_d3d12_resource_destroy(&res->bo);
            if (heap && heap->owns_bo)
                adreno_d3d12_resource_destroy(&heap->bo);
            memset(res, 0, sizeof(*res));
            return E_OUTOFMEMORY;
        }
    }
    return S_OK;
}

static VOID APIENTRY UmdDestroyHeapAndResource(D3D12DDI_HDEVICE hDevice,
                                               D3D12DDI_HHEAP hHeap,
                                               D3D12DDI_HRESOURCE hResource)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("DestroyHeapAndResource(dev=%p)", (void*)dev);
    UMD_HEAP *heap = (UMD_HEAP *)hHeap.pDrvPrivate;
    UMD_RESOURCE *res = umd_resource(hResource);

    if (!dev)
        return;
    if (res) {
        umd_device_remove_resource(dev, res);
        if (res->owns_bo)
            adreno_d3d12_resource_destroy(&res->bo);
        memset(res, 0, sizeof(*res));
    }
    if (heap) {
        if (heap->owns_bo)
            adreno_d3d12_resource_destroy(&heap->bo);
        memset(heap, 0, sizeof(*heap));
    }
}

static HRESULT APIENTRY UmdMapHeap(D3D12DDI_HDEVICE hDevice,
                                   D3D12DDI_HHEAP hHeap, VOID **ppData)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("MapHeap(dev=%p hh=%p)", (void*)dev, hHeap.pDrvPrivate);
    UMD_HEAP *heap = (UMD_HEAP *)hHeap.pDrvPrivate;

    (void)dev;
    if (!heap || !heap->owns_bo || !heap->bo.host || !ppData)
        return E_INVALIDARG;
    AdrenoArm64FlushDcache(heap->bo.host, heap->bo.size);
    *ppData = heap->bo.host;
    return S_OK;
}

static VOID APIENTRY UmdUnmapHeap(D3D12DDI_HDEVICE hDevice,
                                  D3D12DDI_HHEAP hHeap)
{
    UMD_HEAP *heap = (UMD_HEAP *)hHeap.pDrvPrivate;
    (void)hDevice;
    if (heap && heap->owns_bo && heap->bo.host)
        AdrenoArm64FlushDcache(heap->bo.host, heap->bo.size);
}

static D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY UmdCheckResourceVirtualAddress(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hResource)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("CheckResourceVA(hr=%p)", hResource.pDrvPrivate);
    UMD_RESOURCE *res = umd_resource(hResource);

    (void)dev;
    return res ? res->gpu_va : 0;
}

static D3DKMT_HANDLE APIENTRY UmdCheckResourceAllocationHandle(
    D3D12DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
    UMD_RESOURCE *res = (UMD_RESOURCE *)hResource.pDrvPrivate;
    D3DKMT_HANDLE h = res ? (D3DKMT_HANDLE)res->bo.handle : 0;
    (void)hDevice;
    umd_trace("CheckResourceAllocationHandle(hr=%p) -> 0x%x",
              hResource.pDrvPrivate, (unsigned)h);
    return h;
}

static UINT APIENTRY UmdGetPresentPrivateDriverDataSize(
    D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_PRESENT_0001 *args)
{
    (void)hDevice;
    (void)args;
    umd_trace("GetPresentPrivateDriverDataSize -> 0");
    return 0;
}

static HRESULT APIENTRY UmdMakeResident(
    D3D12DDI_HDEVICE hDevice, D3D12DDIARG_MAKERESIDENT_0001 *args)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    (void)dev;
    if (args) {
        args->WaitMask = 0;
        if (args->pPagingFenceValue && args->NumAdapters > 0)
            args->pPagingFenceValue[0] = 0;
    }
    return S_OK;
}

static HRESULT APIENTRY UmdEvict(
    D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_EVICT *args)
{
    (void)hDevice;
    (void)args;
    return S_OK;
}

static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY UmdCalcPrivateOpenedHeapAndResourceSizes(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_OPENHEAP_0003 *args,
    D3D12DDI_HPROTECTEDRESOURCESESSION_0030 protected_session)
{
    D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes;
    (void)hDevice; (void)args; (void)protected_session;
    sizes.Heap = sizeof(UMD_HEAP);
    sizes.Resource = sizeof(UMD_RESOURCE);
    return sizes;
}

static HRESULT APIENTRY UmdOpenHeapAndResource(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_OPENHEAP_0003 *args,
    D3D12DDI_HHEAP hHeap,
    D3D12DDI_HRTRESOURCE hRTResource,
    D3D12DDI_HPROTECTEDRESOURCESESSION_0030 protected_session,
    D3D12DDI_HRESOURCE hResource)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    UMD_HEAP *heap = (UMD_HEAP *)hHeap.pDrvPrivate;
    UMD_RESOURCE *res = umd_resource(hResource);
    UMD_WDDM_BRIDGE *b;
    D3DKMT_HANDLE h_alloc = 0;
    uint32_t sz = 65536, w = 0, h = 0, pitch = 0, fmt = 0;
    BOOL tex2d = FALSE;
    uint64_t gpu_va = 0;
    void *cpu_ptr = NULL;
    HRESULT hr;

    (void)hRTResource;
    (void)protected_session;
    umd_trace("OpenHeapAndResource(nAlloc=%u hKM=0x%x privSz=%u)",
              args ? args->NumAllocations : 0u,
              args ? (unsigned)args->hKMResource.handle : 0u,
              args ? args->PrivateDriverDataSize : 0u);

    if (!dev || !dev->gpu_ready || !args || args->NumAllocations == 0 ||
        !args->pOpenAllocationInfo)
        return E_INVALIDARG;

    h_alloc = args->pOpenAllocationInfo[0].hAllocation;
    if (args->pOpenAllocationInfo[0].pPrivateDriverData &&
        args->pOpenAllocationInfo[0].PrivateDriverDataSize >= sizeof(adreno_wddm_alloc_private)) {
        const adreno_wddm_alloc_private *priv =
            (const adreno_wddm_alloc_private *)args->pOpenAllocationInfo[0].pPrivateDriverData;
        if (priv->magic == ADRENO_WDDM_ALLOC_PRIVATE_MAGIC && priv->size) {
            sz = priv->size;
            w = priv->width;
            h = priv->height;
            pitch = priv->row_pitch;
            fmt = priv->format;
            tex2d = priv->texture2d ? TRUE : FALSE;
        }
    } else if (args->pPrivateDriverData &&
               args->PrivateDriverDataSize >= sizeof(adreno_wddm_alloc_private)) {
        const adreno_wddm_alloc_private *priv =
            (const adreno_wddm_alloc_private *)args->pPrivateDriverData;
        if (priv->magic == ADRENO_WDDM_ALLOC_PRIVATE_MAGIC && priv->size) {
            sz = priv->size;
            w = priv->width;
            h = priv->height;
            pitch = priv->row_pitch;
            fmt = priv->format;
            tex2d = priv->texture2d ? TRUE : FALSE;
        }
    }
    sz = (sz + 4095u) & ~4095u;

    b = &dev->wddm;
    if (b->h_paging_queue && b->kt && b->kt->pfnMakeResidentCb &&
        b->kt->pfnMapGpuVirtualAddressCb && b->kt->pfnLock2Cb && h_alloc) {
        D3DDDI_MAKERESIDENT mr;
        D3DDDI_MAPGPUVIRTUALADDRESS mva;
        D3DDDICB_LOCK2 lock2;
        UINT64 target_fence = 0;

        memset(&mr, 0, sizeof(mr));
        mr.hPagingQueue = b->h_paging_queue;
        mr.NumAllocations = 1;
        mr.AllocationList = &h_alloc;
        hr = b->kt->pfnMakeResidentCb(b->h_device, &mr);
        if (FAILED(hr) && hr != E_PENDING)
            return hr;
        if (mr.PagingFenceValue > target_fence)
            target_fence = mr.PagingFenceValue;

        memset(&mva, 0, sizeof(mva));
        mva.hPagingQueue = b->h_paging_queue;
        mva.MinimumAddress = 0x20000000ull;
        mva.MaximumAddress = 0x50000000ull;
        mva.hAllocation = h_alloc;
        mva.OffsetInPages = 0;
        mva.SizeInPages = sz >> 12;
        mva.Protection.Write = 1;
        hr = b->kt->pfnMapGpuVirtualAddressCb(b->h_device, &mva);
        if ((FAILED(hr) && hr != E_PENDING) || mva.VirtualAddress == 0)
            return FAILED(hr) ? hr : E_FAIL;
        if (mva.PagingFenceValue > target_fence)
            target_fence = mva.PagingFenceValue;

        if (b->paging_fence_cpu && target_fence > 0) {
            uint64_t t0 = GetTickCount64();
            while (*b->paging_fence_cpu < target_fence) {
                if (GetTickCount64() - t0 > 2000)
                    break;
                Sleep(0);
            }
        }

        memset(&lock2, 0, sizeof(lock2));
        lock2.hAllocation = h_alloc;
        hr = b->kt->pfnLock2Cb(b->h_device, &lock2);
        if (SUCCEEDED(hr) && lock2.pData)
            cpu_ptr = lock2.pData;
        gpu_va = mva.VirtualAddress;
    }

    if (heap) {
        memset(heap, 0, sizeof(*heap));
        heap->device = dev;
        heap->bo.device = &dev->gpu;
        heap->bo.handle = (uint32_t)h_alloc;
        heap->bo.size = sz;
        heap->bo.iova = gpu_va;
        heap->bo.host = cpu_ptr;
        heap->owns_bo = FALSE;
    }
    if (res) {
        memset(res, 0, sizeof(*res));
        res->device = dev;
        res->bo.device = &dev->gpu;
        res->bo.handle = (uint32_t)h_alloc;
        res->bo.size = sz;
        res->bo.iova = gpu_va;
        res->bo.host = cpu_ptr;
        res->owns_bo = FALSE;
        res->bo_offset = 0;
        res->size = sz;
        res->gpu_va = gpu_va;
        res->texture2d = tex2d;
        res->width = w;
        res->height = h;
        res->row_pitch = pitch;
        res->format = (DXGI_FORMAT)fmt;
        (void)umd_device_add_resource(dev, res);
    }
    umd_trace("  OpenHeapAndResource OK: h=0x%x va=0x%llx sz=%u %ux%u",
              (unsigned)h_alloc, (unsigned long long)gpu_va, sz, w, h);
    return S_OK;
}

/* ---- Descriptor heaps and UAV views ------------------------------------- */

static UINT APIENTRY UmdGetDescriptorSizeInBytes(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_DESCRIPTOR_HEAP_TYPE type)
{
    (void)hDevice;
    (void)type;
    return UMD_DESCRIPTOR_SIZE;
}

static SIZE_T APIENTRY UmdCalcPrivateDescriptorHeapSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args)
{
    (void)hDevice;
    if (!args || args->NumDescriptors == 0)
        return 0;
    return sizeof(UMD_DESCRIPTOR_HEAP);
}

static HRESULT APIENTRY UmdCreateDescriptorHeap(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args,
    D3D12DDI_HDESCRIPTORHEAP hDescriptorHeap)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("CreateDescriptorHeap(dev=%p)", (void*)dev);
    UMD_DESCRIPTOR_HEAP *heap =
        (UMD_DESCRIPTOR_HEAP *)hDescriptorHeap.pDrvPrivate;
    uint64_t bytes;

    if (!dev || !heap || !args || args->NumDescriptors == 0)
        return E_INVALIDARG;
    memset(heap, 0, sizeof(*heap));
    heap->device = dev;
    heap->type = args->Type;
    heap->num_descriptors = args->NumDescriptors;
    heap->descriptor_size = UMD_DESCRIPTOR_SIZE;
    bytes = (uint64_t)args->NumDescriptors * UMD_DESCRIPTOR_SIZE;
    if (bytes == 0 || bytes > 256u * 1024u * 1024u)
        return E_INVALIDARG;
    if (adreno_d3d12_resource_create(&dev->gpu, (uint32_t)bytes,
                                     &heap->bo) != ADRENO_OK)
        return E_OUTOFMEMORY;
    heap->initialized = TRUE;
    return S_OK;
}

static VOID APIENTRY UmdDestroyDescriptorHeap(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HDESCRIPTORHEAP hDescriptorHeap)
{
    UMD_DESCRIPTOR_HEAP *heap =
        (UMD_DESCRIPTOR_HEAP *)hDescriptorHeap.pDrvPrivate;

    (void)hDevice;
    if (!heap)
        return;
    if (heap->initialized)
        adreno_d3d12_resource_destroy(&heap->bo);
    memset(heap, 0, sizeof(*heap));
}

static D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY
UmdGetCPUDescriptorHandleForHeapStart(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HDESCRIPTORHEAP hDescriptorHeap)
{
    UMD_DESCRIPTOR_HEAP *heap =
        (UMD_DESCRIPTOR_HEAP *)hDescriptorHeap.pDrvPrivate;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE handle;

    (void)hDevice;
    memset(&handle, 0, sizeof(handle));
    if (heap && heap->initialized)
        handle.ptr = (SIZE_T)(uintptr_t)heap->bo.host;
    return handle;
}

static D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY
UmdGetGPUDescriptorHandleForHeapStart(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HDESCRIPTORHEAP hDescriptorHeap)
{
    UMD_DESCRIPTOR_HEAP *heap =
        (UMD_DESCRIPTOR_HEAP *)hDescriptorHeap.pDrvPrivate;
    D3D12DDI_GPU_DESCRIPTOR_HANDLE handle;

    (void)hDevice;
    memset(&handle, 0, sizeof(handle));
    if (heap && heap->initialized)
        handle.ptr = heap->bo.iova;
    return handle;
}

static VOID APIENTRY UmdCreateUnorderedAccessView(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 *args,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    UMD_RESOURCE *res;
    UMD_UAV_DESC *desc;
    uint64_t stride = 4;
    uint64_t offset;
    uint64_t size;

    (void)hDevice;
    if (!args || dest.ptr == 0)
        return;
    res = umd_resource(args->hDrvResource);
    if (!res)
        return;
    if (args->Buffer.StructureByteStride != 0)
        stride = args->Buffer.StructureByteStride;
    offset = args->Buffer.FirstElement * stride;
    if (args->Buffer.NumElements != 0)
        size = (uint64_t)args->Buffer.NumElements * stride;
    else
        size = res->size > offset ? res->size - offset : 0;
    if (offset + size > res->size)
        return;
    desc = (UMD_UAV_DESC *)(uintptr_t)dest.ptr;
    desc->magic = UMD_UAV_DESC_MAGIC;
    desc->format = args->Format;
    desc->gpu_va = res->gpu_va + offset;
    desc->size = size;
    desc->reserved = 0;
}

static VOID APIENTRY UmdCreateRenderTargetView(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 *args,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    UMD_RESOURCE *res;
    UMD_RTV_DESC *desc;
    uint64_t offset;
    uint64_t size;

    (void)hDevice;
    if (!args || dest.ptr == 0)
        return;
    res = umd_resource(args->hDrvResource);
    if (!res)
        return;
    if (args->ResourceDimension == D3D12DDI_RD_BUFFER) {
        offset = args->Buffer.FirstElement * 4u;
        if (args->Buffer.NumElements != 0)
            size = (uint64_t)args->Buffer.NumElements * 4u;
        else
            size = res->size > offset ? res->size - offset : 0;
    } else if (args->ResourceDimension == D3D12DDI_RD_TEXTURE2D) {
        if (!res->texture2d || args->Tex2D.MipSlice != 0 ||
            args->Tex2D.FirstArraySlice != 0 ||
            args->Tex2D.ArraySize != 1 || args->Tex2D.PlaneSlice != 0)
            return;
        offset = 0;
        size = res->size;
    } else {
        return;
    }
    if (size == 0 || offset + size > res->size)
        return;
    desc = (UMD_RTV_DESC *)(uintptr_t)dest.ptr;
    desc->magic = UMD_RTV_DESC_MAGIC;
    desc->format = args->Format;
    desc->gpu_va = res->gpu_va + offset;
    desc->size = size;
    desc->resource = (uint64_t)(uintptr_t)res;
}

static VOID APIENTRY UmdCreateDepthStencilView(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW *args,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    UMD_RESOURCE *res;
    UMD_RTV_DESC *desc;

    (void)hDevice;
    if (!args || dest.ptr == 0 ||
        args->ResourceDimension != D3D12DDI_RD_TEXTURE2D)
        return;
    res = umd_resource(args->hDrvResource);
    if (!res || !res->texture2d || args->Format != DXGI_FORMAT_D32_FLOAT)
        return;
    if (args->Tex2D.MipSlice != 0 || args->Tex2D.FirstArraySlice != 0 ||
        args->Tex2D.ArraySize != 1)
        return;
    desc = (UMD_RTV_DESC *)(uintptr_t)dest.ptr;
    desc->magic = UMD_DSV_DESC_MAGIC;
    desc->format = args->Format;
    desc->gpu_va = res->gpu_va;
    desc->size = res->size;
    desc->resource = (uint64_t)(uintptr_t)res;
}

static uint32_t umd_d3d_addr_to_a730(D3D12DDI_TEXTURE_ADDRESS_MODE m)
{
    switch (m) {
    case D3D12DDI_TEXTURE_ADDRESS_MODE_WRAP:   return ADRENO_TEX_WRAP_REPEAT;
    case D3D12DDI_TEXTURE_ADDRESS_MODE_MIRROR: return ADRENO_TEX_WRAP_MIRROR_REPEAT;
    case D3D12DDI_TEXTURE_ADDRESS_MODE_BORDER: return ADRENO_TEX_WRAP_CLAMP_BORDER;
    case D3D12DDI_TEXTURE_ADDRESS_MODE_MIRRORONCE: return ADRENO_TEX_WRAP_MIRROR_CLAMP;
    case D3D12DDI_TEXTURE_ADDRESS_MODE_CLAMP:
    default:                                   return ADRENO_TEX_WRAP_CLAMP_TO_EDGE;
    }
}

static void umd_encode_d3d_sampler(D3D12DDI_FILTER filter,
                                   D3D12DDI_TEXTURE_ADDRESS_MODE addr_u,
                                   D3D12DDI_TEXTURE_ADDRESS_MODE addr_v,
                                   D3D12DDI_TEXTURE_ADDRESS_MODE addr_w,
                                   uint32_t out_dw[4])
{
    adreno_a730_samp_desc s;
    memset(&s, 0, sizeof(s));
    s.mip_linear = (((uint32_t)filter & 0x03u) != 0u);
    s.mag_linear = ((((uint32_t)filter >> 2) & 0x03u) != 0u);
    s.min_linear = ((((uint32_t)filter >> 4) & 0x03u) != 0u);
    s.wrap_u = umd_d3d_addr_to_a730(addr_u);
    s.wrap_v = umd_d3d_addr_to_a730(addr_v);
    s.wrap_w = umd_d3d_addr_to_a730(addr_w);
    adreno_a730_encode_tex_samp(&s, out_dw);
}

static VOID APIENTRY UmdCreateShaderResourceView(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *args,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    UMD_RESOURCE *res;
    UMD_SRV_DESC *desc;
    (void)hDevice;
    if (!args || dest.ptr == 0)
        return;
    res = umd_resource(args->hDrvResource);
    if (!res)
        return;
    desc = (UMD_SRV_DESC *)(uintptr_t)dest.ptr;
    desc->magic    = UMD_SRV_DESC_MAGIC;
    desc->format   = args->Format ? (uint32_t)args->Format : (uint32_t)res->format;
    desc->gpu_va   = res->gpu_va;
    desc->size     = res->size;
    desc->resource = (uint64_t)(uintptr_t)res;
}

static VOID APIENTRY UmdCreateConstantBufferView(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC *args,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    UMD_CBV_DESC *desc;
    (void)hDevice;
    if (!args || dest.ptr == 0)
        return;
    desc = (UMD_CBV_DESC *)(uintptr_t)dest.ptr;
    desc->magic     = UMD_CBV_DESC_MAGIC;
    desc->reserved0 = 0;
    desc->gpu_va    = args->BufferLocation;
    desc->size      = args->SizeInBytes;
    desc->reserved1 = 0;
}

static VOID APIENTRY UmdCreateSampler(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_SAMPLER *args,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    UMD_SAMP_DESC *desc;
    (void)hDevice;
    if (!args || !args->pSamplerDesc || dest.ptr == 0)
        return;
    desc = (UMD_SAMP_DESC *)(uintptr_t)dest.ptr;
    memset(desc, 0, sizeof(*desc));
    desc->magic = UMD_SAMP_DESC_MAGIC;
    umd_encode_d3d_sampler(args->pSamplerDesc->Filter,
                           args->pSamplerDesc->AddressU,
                           args->pSamplerDesc->AddressV,
                           args->pSamplerDesc->AddressW,
                           desc->dw);
}

static VOID APIENTRY UmdCopyDescriptorsSimple(
    D3D12DDI_HDEVICE hDevice, UINT num_descriptors,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE dst_start,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE src_start,
    D3D12DDI_DESCRIPTOR_HEAP_TYPE heap_type)
{
    (void)hDevice;
    (void)heap_type;
    if (num_descriptors == 0 || !dst_start.ptr || !src_start.ptr)
        return;
    memcpy((void *)(uintptr_t)dst_start.ptr,
           (const void *)(uintptr_t)src_start.ptr,
           (size_t)num_descriptors * UMD_DESCRIPTOR_SIZE);
}

static VOID APIENTRY UmdCopyDescriptors(
    D3D12DDI_HDEVICE hDevice,
    UINT num_dst_ranges,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dst_starts,
    const UINT *dst_sizes,
    UINT num_src_ranges,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *src_starts,
    const UINT *src_sizes,
    D3D12DDI_DESCRIPTOR_HEAP_TYPE heap_type)
{
    UINT di = 0, doff = 0, si = 0, soff = 0;
    (void)hDevice;
    (void)heap_type;
    if (!dst_starts || !src_starts)
        return;
    while (di < num_dst_ranges && si < num_src_ranges) {
        UINT dlen = dst_sizes ? dst_sizes[di] : 1u;
        UINT slen = src_sizes ? src_sizes[si] : 1u;
        UINT drem = (dlen > doff) ? (dlen - doff) : 0u;
        UINT srem = (slen > soff) ? (slen - soff) : 0u;
        UINT cnt  = (drem < srem) ? drem : srem;
        if (cnt > 0 && dst_starts[di].ptr && src_starts[si].ptr) {
            memcpy((uint8_t *)(uintptr_t)dst_starts[di].ptr + (size_t)doff * UMD_DESCRIPTOR_SIZE,
                   (const uint8_t *)(uintptr_t)src_starts[si].ptr + (size_t)soff * UMD_DESCRIPTOR_SIZE,
                   (size_t)cnt * UMD_DESCRIPTOR_SIZE);
        }
        doff += cnt;
        soff += cnt;
        if (doff >= dlen) { di++; doff = 0; }
        if (soff >= slen) { si++; soff = 0; }
    }
}

/* ---- Format and pipeline/shader scaffold -------------------------------- */

static VOID APIENTRY UmdCheckFormatSupport(D3D12DDI_HDEVICE hDevice,
                                           DXGI_FORMAT format, UINT *support)
{
    (void)hDevice;
    if (!support)
        return;
    *support = 0;
    switch (format) {
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_FLOAT:
        *support = D3D12DDI_FORMAT_SUPPORT_BUFFER |
                   D3D12DDI_FORMAT_SUPPORT_UAV_READS |
                   D3D12DDI_FORMAT_SUPPORT_UAV_WRITES |
                   D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        *support = D3D12DDI_FORMAT_SUPPORT_BUFFER |
                   D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE |
                   D3D12DDI_FORMAT_SUPPORT_RENDERTARGET |
                   D3D12DDI_FORMAT_SUPPORT_BLENDABLE |
                   D3D12DDI_FORMAT_SUPPORT_MULTIPLANE_OVERLAY |
                   D3D12DDI_FORMAT_SUPPORT_DISPLAY;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        *support = D3D12DDI_FORMAT_SUPPORT_BUFFER |
                   D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE |
                   D3D12DDI_FORMAT_SUPPORT_RENDERTARGET |
                   D3D12DDI_FORMAT_SUPPORT_DISPLAY;
        break;
    case DXGI_FORMAT_D32_FLOAT:
        *support = D3D12DDI_FORMAT_SUPPORT_BUFFER |
                   D3D12DDI_FORMAT_SUPPORT_RENDERTARGET;
        break;
    default:
        break;
    }
}

static VOID APIENTRY UmdCheckMultisampleQualityLevels(
    D3D12DDI_HDEVICE hDevice, DXGI_FORMAT format, UINT sample_count,
    D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags, UINT *levels)
{
    (void)hDevice;
    (void)format;
    (void)flags;
    if (!levels)
        return;
    *levels = sample_count == 1 ? 1 : 0;
}

static bool umd_is_sm45_tokens(const uint32_t *toks, uint32_t *out_bytes)
{
    uint32_t ver, stg, maj, dwords;
    if (!toks) return false;
    ver = toks[0];
    stg = (ver >> 16) & 0xffffu;
    maj = (ver >> 4) & 0xfu;
    dwords = toks[1];
    if ((stg == 0u || stg == 1u || stg == 5u) &&
        (maj == 4u || maj == 5u) &&
        dwords >= 2u && dwords <= 4u * 1024u * 1024u) {
        if (out_bytes) *out_bytes = dwords * 4u;
        return true;
    }
    return false;
}

static uint32_t UmdShaderCodeBytes(
    const D3D12DDIARG_CREATE_SHADER_0026 *args)
{
    const uint8_t *p;
    uint32_t magic;
    uint32_t size;

    if (!args || !args->pShaderCode)
        return 0;
    p = (const uint8_t *)args->pShaderCode;
    memcpy(&magic, p, 4);
    if (magic == ADRENO_IR3_BLOB_MAGIC) {
        const adreno_ir3_blob *blob = (const adreno_ir3_blob *)p;
        uint64_t total;
        if (blob->version != ADRENO_IR3_BLOB_VERSION ||
            blob->shader_bytes == 0 ||
            blob->shader_bytes > 16u * 1024u * 1024u)
            return 0;
        total = sizeof(*blob) + blob->shader_bytes;
        if (total > 16u * 1024u * 1024u)
            return 0;
        return (uint32_t)total;
    }
    if (magic == UMD_REF_TRIANGLE_MAGIC)
        return 8;
    if (magic == 0x43425844u /* DXBC */ || magic == 0x4C495844u /* DXIL */) {
        memcpy(&size, p + 24, 4);
        if (size < 32 || size > 16u * 1024u * 1024u)
            return 0;
        return size;
    }
    /* D3D12 runtime passes stripped SM4/SM5 token streams (tok[0]=ver, tok[1]=dwords). */
    if (umd_is_sm45_tokens((const uint32_t *)args->pShaderCode, &size))
        return size;
    return 0;
}

static SIZE_T APIENTRY UmdCalcPrivateShaderSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_SHADER_0026 *args)
{
    (void)hDevice;
    return sizeof(UMD_SHADER) + UmdShaderCodeBytes(args);
}

static VOID APIENTRY UmdCreateShader(
    D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_SHADER_0026 *args,
    D3D12DDI_HSHADER hShader)
{
    UMD_SHADER *shader = (UMD_SHADER *)hShader.pDrvPrivate;
    uint32_t bytes = UmdShaderCodeBytes(args);
    uint32_t magic = 0;
    uint8_t *dst;

    (void)hDevice;
    if (!shader)
        return;
    memset(shader, 0, sizeof(*shader));
    if (!bytes)
        return;
    memcpy(&magic, args->pShaderCode, 4);
    shader->magic = 0x53484455u; /* "UDHS" */
    shader->bytes = bytes;
    shader->code_bytes = bytes;
    dst = (uint8_t *)shader + sizeof(*shader);
    if (magic == UMD_REF_TRIANGLE_MAGIC) {
        shader->reference_triangle = TRUE;
        shader->code_bytes = 8;
        memcpy(dst, args->pShaderCode, 8);
    } else if (magic == ADRENO_IR3_BLOB_MAGIC) {
        const adreno_ir3_blob *blob =
            (const adreno_ir3_blob *)args->pShaderCode;
        shader->has_ir3 = TRUE;
        shader->code_bytes = blob->shader_bytes;
        shader->meta.instrlen_units = blob->instrlen_units;
        shader->meta.constlen = blob->constlen;
        shader->meta.fullregfootprint = blob->fullregfootprint;
        shader->meta.halfregfootprint = blob->halfregfootprint;
        shader->meta.branchstack = blob->branchstack;
        shader->meta.threadsize = blob->threadsize;
        shader->meta.shared_size = blob->shared_size;
        shader->meta.constantrammode = blob->constantrammode;
        shader->meta.mergedregs = blob->mergedregs;
        shader->meta.localid_regid = blob->localid_regid;
        shader->meta.wgid_constid = blob->wgid_constid;
        shader->meta.local_size[0] = blob->local_size[0];
        shader->meta.local_size[1] = blob->local_size[1];
        shader->meta.local_size[2] = blob->local_size[2];
        memcpy(dst, (const uint8_t *)args->pShaderCode + sizeof(*blob),
               blob->shader_bytes);
    } else {
        memcpy(dst, args->pShaderCode, bytes);
        if (magic == 0x43425844u /* DXBC */) {
            adreno_dxbc_compiled *comp =
                (adreno_dxbc_compiled *)calloc(1, sizeof(*comp));
            if (comp) {
                enum adreno_status cst =
                    adreno_dxbc_compile_to_ir3(dst, bytes, comp);
                if (cst == ADRENO_OK) {
                    shader->compiled = comp;
                    shader->has_dxbc_ir3 = TRUE;
                    umd_trace("CreateShader DXBC->IR3 OK: stage=%u insns=%u units=%u regs=%u",
                              (unsigned)comp->stage, comp->insn_count,
                              comp->instrlen_units, comp->fullregfootprint);
                } else {
                    umd_trace("CreateShader DXBC->IR3 err=%d", (int)cst);
                    free(comp);
                }
            }
        } else if (umd_is_sm45_tokens((const uint32_t *)dst, NULL)) {
            adreno_dxbc_compiled *comp =
                (adreno_dxbc_compiled *)calloc(1, sizeof(*comp));
            if (comp) {
                adreno_dxbc_io_elem in_sig[ADRENO_DXBC_MAX_IO];
                adreno_dxbc_io_elem out_sig[ADRENO_DXBC_MAX_IO];
                uint32_t num_in = 0, num_out = 0, failed_op = 0, k;
                const D3D12DDIARG_STAGE_IO_SIGNATURES *io = args->IOSignatures.Standard;
                if (io) {
                    if (io->pInputSignature && io->NumInputSignatureEntries) {
                        num_in = io->NumInputSignatureEntries;
                        if (num_in > ADRENO_DXBC_MAX_IO) num_in = ADRENO_DXBC_MAX_IO;
                        for (k = 0; k < num_in; k++) {
                            memset(&in_sig[k], 0, sizeof(in_sig[k]));
                            in_sig[k].sysval_type = (uint32_t)io->pInputSignature[k].SystemValue;
                            in_sig[k].reg         = io->pInputSignature[k].Register;
                            in_sig[k].mask        = io->pInputSignature[k].Mask & 0xfu;
                        }
                    }
                    if (io->pOutputSignature && io->NumOutputSignatureEntries) {
                        num_out = io->NumOutputSignatureEntries;
                        if (num_out > ADRENO_DXBC_MAX_IO) num_out = ADRENO_DXBC_MAX_IO;
                        for (k = 0; k < num_out; k++) {
                            memset(&out_sig[k], 0, sizeof(out_sig[k]));
                            out_sig[k].sysval_type = (uint32_t)io->pOutputSignature[k].SystemValue;
                            out_sig[k].reg         = io->pOutputSignature[k].Register;
                            out_sig[k].mask        = io->pOutputSignature[k].Mask & 0xfu;
                        }
                    }
                }
                {
                    enum adreno_status cst = adreno_dxbc_compile_tokens_to_ir3(
                        (const uint32_t *)dst, bytes / 4u,
                        in_sig, num_in, out_sig, num_out, comp, &failed_op);
                    if (cst == ADRENO_OK) {
                        shader->compiled = comp;
                        shader->has_dxbc_ir3 = TRUE;
                        umd_trace("CreateShader SM5->IR3 OK: stage=%u insns=%u units=%u regs=%u in=%u out=%u var=%u",
                                  (unsigned)comp->stage, comp->insn_count,
                                  comp->instrlen_units, comp->fullregfootprint,
                                  num_in, num_out, comp->num_varying_scalars);
                    } else {
                        umd_trace("CreateShader SM5->IR3 FAIL: st=%d failed_op=0x%x ver=0x%x dwords=%u",
                                  (int)cst, failed_op, ((const uint32_t *)dst)[0], bytes / 4u);
                        free(comp);
                    }
                }
            }
        } else {
            umd_trace("CreateShader unrecognized magic=0x%08x bytes=%u", magic, bytes);
        }
    }
}

static VOID APIENTRY UmdDestroyShader(D3D12DDI_HDEVICE hDevice,
                                      D3D12DDI_HSHADER hShader)
{
    UMD_SHADER *shader = (UMD_SHADER *)hShader.pDrvPrivate;
    (void)hDevice;
    if (shader) {
        if (shader->compiled) {
            free(shader->compiled);
            shader->compiled = NULL;
        }
        memset(shader, 0, sizeof(*shader));
    }
}

static SIZE_T APIENTRY UmdCalcPrivateElementLayoutSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATEELEMENTLAYOUT_0010 *args)
{
    (void)hDevice;
    (void)args;
    return sizeof(UMD_ELEMENT_LAYOUT);
}

static VOID APIENTRY UmdCreateElementLayout(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATEELEMENTLAYOUT_0010 *args,
    D3D12DDI_HELEMENTLAYOUT hLayout)
{
    UMD_ELEMENT_LAYOUT *el = (UMD_ELEMENT_LAYOUT *)hLayout.pDrvPrivate;
    UINT n, i;
    (void)hDevice;
    if (!el)
        return;
    memset(el, 0, sizeof(*el));
    if (!args || !args->pVertexElements)
        return;
    n = args->NumElements;
    if (n > ADRENO_3D_MAX_VATTRS)
        n = ADRENO_3D_MAX_VATTRS;
    el->num_elements = n;
    for (i = 0; i < n; i++)
        el->elements[i] = args->pVertexElements[i];
}

static VOID APIENTRY UmdDestroyElementLayout(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HELEMENTLAYOUT hLayout)
{
    (void)hDevice;
    if (hLayout.pDrvPrivate)
        memset(hLayout.pDrvPrivate, 0, sizeof(UMD_ELEMENT_LAYOUT));
}

static SIZE_T APIENTRY UmdCalcPrivateRootSignatureSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 *args)
{
    (void)hDevice;
    (void)args;
    return sizeof(UMD_ROOTSIG);
}

static HRESULT APIENTRY UmdCreateRootSignature(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 *args,
    D3D12DDI_HROOTSIGNATURE hRootSignature)
{
    UMD_ROOTSIG *root = (UMD_ROOTSIG *)hRootSignature.pDrvPrivate;
    const D3D12DDI_ROOT_SIGNATURE_0013 *desc = args ?
        args->pRootSignature_1_1 : NULL;
    UINT i, n;

    (void)hDevice;
    if (!root || !args)
        return E_INVALIDARG;
    memset(root, 0, sizeof(*root));
    root->version = (uint32_t)args->Version;
    root->node_mask = args->NodeMask;
    if (!desc || !desc->pRootParameters)
        return S_OK;
    n = desc->NumParameters;
    if (n > UMD_MAX_ROOT_PARAMS)
        n = UMD_MAX_ROOT_PARAMS;
    root->num_params = n;
    for (i = 0; i < n; i++) {
        const D3D12DDI_ROOT_PARAMETER_0013 *p = &desc->pRootParameters[i];
        root->params[i].type = (uint32_t)p->ParameterType;
        switch (p->ParameterType) {
        case D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            root->params[i].shader_register = p->Constants.ShaderRegister;
            root->params[i].register_space = p->Constants.RegisterSpace;
            root->params[i].num_32bit_values = p->Constants.Num32BitValues;
            break;
        case D3D12DDI_ROOT_PARAMETER_TYPE_CBV:
        case D3D12DDI_ROOT_PARAMETER_TYPE_SRV:
        case D3D12DDI_ROOT_PARAMETER_TYPE_UAV:
            root->params[i].shader_register = p->Descriptor.ShaderRegister;
            root->params[i].register_space = p->Descriptor.RegisterSpace;
            break;
        case D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            if (p->DescriptorTable.NumDescriptorRanges > 0 &&
                p->DescriptorTable.pDescriptorRanges) {
                const D3D12DDI_DESCRIPTOR_RANGE_0013 *r0 =
                    &p->DescriptorTable.pDescriptorRanges[0];
                root->params[i].table_range_type = (uint32_t)r0->RangeType;
                root->params[i].table_base_reg   = r0->BaseShaderRegister;
                root->params[i].table_num_descs  =
                    (r0->NumDescriptors == 0xffffffffu || r0->NumDescriptors > ADRENO_3D_MAX_TEXTURES)
                        ? ADRENO_3D_MAX_TEXTURES : r0->NumDescriptors;
            }
            break;
        default:
            break;
        }
    }
    if (desc->NumStaticSamplers > 0 && desc->pStaticSamplers) {
        UINT ns = desc->NumStaticSamplers;
        if (ns > ADRENO_3D_MAX_SAMPLERS) ns = ADRENO_3D_MAX_SAMPLERS;
        root->num_static_samplers = ns;
        for (i = 0; i < ns; i++) {
            const D3D12DDI_STATIC_SAMPLER *ss = &desc->pStaticSamplers[i];
            root->static_samplers[i].shader_register = ss->ShaderRegister;
            umd_encode_d3d_sampler(ss->Filter, ss->AddressU, ss->AddressV,
                                   ss->AddressW, root->static_samplers[i].dw);
        }
    }
    return S_OK;
}

static VOID APIENTRY UmdDestroyRootSignature(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HROOTSIGNATURE hRootSignature)
{
    UMD_ROOTSIG *root = (UMD_ROOTSIG *)hRootSignature.pDrvPrivate;
    (void)hDevice;
    if (root)
        memset(root, 0, sizeof(*root));
}

static SIZE_T APIENTRY UmdCalcPrivatePipelineStateSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 *args)
{
    (void)hDevice;
    (void)args;
    return sizeof(UMD_PSO);
}

static HRESULT APIENTRY UmdCreatePipelineState(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 *args,
    D3D12DDI_HPIPELINESTATE hPipelineState,
    D3D12DDI_HRTPIPELINESTATE hRTPipelineState)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("CreatePipelineState(dev=%p)", (void*)dev);
    UMD_PSO *pso = (UMD_PSO *)hPipelineState.pDrvPrivate;

    (void)hRTPipelineState;
    if (!dev || !pso || !args)
        return E_INVALIDARG;
    memset(pso, 0, sizeof(*pso));
    pso->id = ++dev->next_pso_id;
    pso->compute = args->hComputeShader.pDrvPrivate != NULL;
    pso->shader = (UMD_SHADER *)args->hComputeShader.pDrvPrivate;
    pso->root_signature =
        (UMD_ROOTSIG *)args->hRootSignature.pDrvPrivate;
    if (!pso->compute && args->hVertexShader.pDrvPrivate) {
        UMD_SHADER *vs = (UMD_SHADER *)args->hVertexShader.pDrvPrivate;
        UMD_SHADER *ps = (UMD_SHADER *)args->hPixelShader.pDrvPrivate;
        pso->vs = vs;
        pso->ps = ps;
        pso->layout = (UMD_ELEMENT_LAYOUT *)args->hElementLayout.pDrvPrivate;
        if (args->NumRenderTargets >= 1)
            pso->rtv_format = args->RTVFormats[0];
        if (vs->reference_triangle && args->NumRenderTargets == 1 &&
            args->RTVFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM &&
            args->SampleDesc.Count == 1) {
            pso->reference_triangle = TRUE;
        } else if (vs->has_dxbc_ir3 && vs->compiled &&
                   ps && ps->has_dxbc_ir3 && ps->compiled) {
            if (adreno_d3d12_resource_create(&dev->gpu,
                                             vs->compiled->code_bytes,
                                             &pso->vs_bo) == ADRENO_OK &&
                pso->vs_bo.host) {
                if (adreno_d3d12_resource_create(&dev->gpu,
                                                 ps->compiled->code_bytes,
                                                 &pso->ps_bo) == ADRENO_OK &&
                    pso->ps_bo.host) {
                    memcpy(pso->vs_bo.host, vs->compiled->insns,
                           vs->compiled->code_bytes);
                    memcpy(pso->ps_bo.host, ps->compiled->insns,
                           ps->compiled->code_bytes);
                    AdrenoArm64FlushDcache(pso->vs_bo.host, vs->compiled->code_bytes);
                    AdrenoArm64FlushDcache(pso->ps_bo.host, ps->compiled->code_bytes);
                    pso->has_hw_gfx = TRUE;
                    umd_trace("CreatePipelineState HW-GFX OK: vs_iova=0x%llx ps_iova=0x%llx",
                              (unsigned long long)pso->vs_bo.iova,
                              (unsigned long long)pso->ps_bo.iova);
                } else {
                    adreno_d3d12_resource_destroy(&pso->vs_bo);
                }
            }
        }
    }
    if (pso->compute && pso->shader && pso->shader->has_ir3) {
        if (pso->shader->code_bytes == 0)
            return E_INVALIDARG;
        if (adreno_d3d12_resource_create(&dev->gpu,
                                         pso->shader->code_bytes,
                                         &pso->shader_bo) != ADRENO_OK)
            return E_OUTOFMEMORY;
        if (!pso->shader_bo.host) {
            adreno_d3d12_resource_destroy(&pso->shader_bo);
            return E_FAIL;
        }
        memcpy(pso->shader_bo.host,
               (const uint8_t *)pso->shader + sizeof(*pso->shader),
               pso->shader->code_bytes);
        pso->has_ir3 = TRUE;
        pso->meta = pso->shader->meta;
    } else if (pso->compute && pso->shader && pso->shader->has_dxbc_ir3 &&
               pso->shader->compiled) {
        adreno_dxbc_compiled *c = pso->shader->compiled;
        if (adreno_d3d12_resource_create(&dev->gpu, c->code_bytes,
                                         &pso->shader_bo) != ADRENO_OK)
            return E_OUTOFMEMORY;
        if (!pso->shader_bo.host) {
            adreno_d3d12_resource_destroy(&pso->shader_bo);
            return E_FAIL;
        }
        memcpy(pso->shader_bo.host, c->insns, c->code_bytes);
        AdrenoArm64FlushDcache(pso->shader_bo.host, c->code_bytes);
        pso->has_ir3 = TRUE;
        pso->meta = c->cs_meta;
        umd_trace("CreatePipelineState HW-CS (DXBC->IR3) OK: iova=0x%llx units=%u",
                  (unsigned long long)pso->shader_bo.iova, c->instrlen_units);
    }
    return S_OK;
}

static VOID APIENTRY UmdDestroyPipelineState(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HPIPELINESTATE hPipelineState)
{
    UMD_PSO *pso = (UMD_PSO *)hPipelineState.pDrvPrivate;
    (void)hDevice;
    if (!pso)
        return;
    if (pso->has_ir3)
        adreno_d3d12_resource_destroy(&pso->shader_bo);
    if (pso->has_hw_gfx) {
        adreno_d3d12_resource_destroy(&pso->vs_bo);
        adreno_d3d12_resource_destroy(&pso->ps_bo);
    }
    memset(pso, 0, sizeof(*pso));
}

static VOID APIENTRY UmdSetPipelineState(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_HPIPELINESTATE hPipelineState)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    if (list)
        list->pso = hPipelineState.pDrvPrivate;
}

static VOID APIENTRY UmdOMSetRenderTargets(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT num_render_targets,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *render_targets,
    BOOL single_handle, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *depth_stencil)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;

    (void)single_handle;
    (void)depth_stencil;
    if (!list)
        return;
    if (num_render_targets > 0 && render_targets && render_targets[0].ptr)
        list->rtv = (void *)(uintptr_t)render_targets[0].ptr;
    else
        list->rtv = NULL;
}

static VOID APIENTRY UmdRSSetViewports(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT count,
    const D3D12DDI_VIEWPORT *viewports)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;

    if (!list)
        return;
    if (count == 0 || !viewports) {
        list->has_viewport = FALSE;
        memset(&list->viewport, 0, sizeof(list->viewport));
        return;
    }
    list->viewport = viewports[0];
    list->has_viewport = TRUE;
}

static VOID APIENTRY UmdRSSetScissorRects(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT count,
    const D3D12DDI_RECT *rects)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    if (!list)
        return;
    if (count == 0 || !rects) {
        list->has_scissor = FALSE;
        memset(&list->scissor, 0, sizeof(list->scissor));
        return;
    }
    list->scissor = rects[0];
    list->has_scissor = TRUE;
}

static VOID APIENTRY UmdIASetPrimitiveTopology(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_PRIMITIVE_TOPOLOGY topology)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    if (!list)
        return;
    switch (topology) {
    case D3D12DDI_PRIMITIVE_TOPOLOGY_POINTLIST:
        list->prim_type = ADRENO_PRIM_POINTLIST; break;
    case D3D12DDI_PRIMITIVE_TOPOLOGY_LINELIST:
        list->prim_type = ADRENO_PRIM_LINELIST; break;
    case D3D12DDI_PRIMITIVE_TOPOLOGY_LINESTRIP:
        list->prim_type = ADRENO_PRIM_LINESTRIP; break;
    case D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        list->prim_type = ADRENO_PRIM_TRISTRIP; break;
    case D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    default:
        list->prim_type = ADRENO_PRIM_TRILIST; break;
    }
}

static VOID APIENTRY UmdIASetVertexBuffers(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT start_slot, UINT num_views,
    const D3D12DDI_VERTEX_BUFFER_VIEW *views)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UINT i;
    if (!list || !views)
        return;
    for (i = 0; i < num_views; i++) {
        UINT slot = start_slot + i;
        if (slot < ADRENO_3D_MAX_VBUFS) {
            list->vbufs[slot] = views[i];
            if (slot + 1u > list->num_vbufs)
                list->num_vbufs = slot + 1u;
        }
    }
}

static VOID APIENTRY UmdIASetIndexBuffer(
    D3D12DDI_HCOMMANDLIST hCommandList,
    const D3D12DDI_INDEX_BUFFER_VIEW *view)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    if (!list)
        return;
    if (!view || !view->BufferLocation) {
        list->has_ibuf = FALSE;
        memset(&list->ibuf, 0, sizeof(list->ibuf));
        return;
    }
    list->ibuf = *view;
    list->has_ibuf = TRUE;
}

static VOID APIENTRY UmdSetGraphicsRootSignature(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_HROOTSIGNATURE hRootSignature)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    if (list)
        list->root_signature = hRootSignature.pDrvPrivate;
}

static VOID APIENTRY UmdSetGraphicsRoot32BitConstants(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT root_param,
    UINT num_values, const void *src_data, UINT dst_offset)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UINT count;
    (void)root_param;
    if (!list || !src_data || dst_offset >= 16u)
        return;
    count = num_values;
    if (count > 16u - dst_offset)
        count = 16u - dst_offset;
    memcpy(&list->gfx_constants[dst_offset], src_data, count * sizeof(uint32_t));
    if ((dst_offset + count + 3u) / 4u > list->gfx_const_vec4s)
        list->gfx_const_vec4s = (dst_offset + count + 3u) / 4u;
}

static VOID APIENTRY UmdSetGraphicsRoot32BitConstant(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT root_param,
    UINT src_data, UINT dst_offset)
{
    UmdSetGraphicsRoot32BitConstants(hCommandList, root_param, 1, &src_data, dst_offset);
}

static VOID APIENTRY UmdSetGraphicsRootConstantBufferView(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT root_param,
    D3D12DDI_GPU_VIRTUAL_ADDRESS buf_va)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_RESOURCE *res;
    (void)root_param;
    if (!list || !list->device || !buf_va)
        return;
    res = umd_find_va(list->device, buf_va, 16u);
    if (res && res->bo.host) {
        uint64_t off = res->bo_offset + (buf_va - res->gpu_va);
        uint32_t avail = (off < res->bo.size) ? (uint32_t)(res->bo.size - off) : 0u;
        uint32_t copy_bytes = avail < sizeof(list->gfx_constants) ? avail : (uint32_t)sizeof(list->gfx_constants);
        if (copy_bytes > 0) {
            memcpy(list->gfx_constants, (const uint8_t *)res->bo.host + off, copy_bytes);
            list->gfx_const_vec4s = (copy_bytes + 15u) / 16u;
        }
    }
}

static VOID APIENTRY UmdSetGraphicsRootShaderResourceView(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT root_param,
    D3D12DDI_GPU_VIRTUAL_ADDRESS buf_va)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_ROOTSIG *root;
    UMD_RESOURCE *res;
    UINT slot = 0;

    if (!list || !list->device || !buf_va)
        return;
    root = (UMD_ROOTSIG *)list->root_signature;
    if (root && root_param < root->num_params)
        slot = root->params[root_param].shader_register;
    if (slot >= ADRENO_3D_MAX_TEXTURES)
        return;
    res = umd_find_va(list->device, buf_va, 4u);
    if (!res)
        return;
    list->gfx_srvs[slot].magic    = UMD_SRV_DESC_MAGIC;
    list->gfx_srvs[slot].format   = (uint32_t)res->format;
    list->gfx_srvs[slot].gpu_va   = buf_va;
    list->gfx_srvs[slot].size     = res->size;
    list->gfx_srvs[slot].resource = (uint64_t)(uintptr_t)res;
    list->gfx_srv_mask |= (1u << slot);
}

static VOID APIENTRY UmdSetGraphicsRootDescriptorTable(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT root_param,
    D3D12DDI_GPU_DESCRIPTOR_HANDLE base_desc)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_ROOTSIG *root;
    UMD_DESCRIPTOR_HEAP *heap = NULL;
    UINT base_reg = 0, max_cnt = ADRENO_3D_MAX_TEXTURES, k;
    uint64_t offset;

    if (!list || !base_desc.ptr)
        return;
    root = (UMD_ROOTSIG *)list->root_signature;
    if (root && root_param < root->num_params &&
        root->params[root_param].type == D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        base_reg = root->params[root_param].table_base_reg;
        if (root->params[root_param].table_num_descs > 0)
            max_cnt = root->params[root_param].table_num_descs;
    }

    /* Match GPU descriptor handle against bound CBV/SRV/UAV or SAMPLER heap */
    if (list->descriptor_heap) {
        UMD_DESCRIPTOR_HEAP *h = (UMD_DESCRIPTOR_HEAP *)list->descriptor_heap;
        if (h->initialized && h->bo.host &&
            base_desc.ptr >= h->bo.iova &&
            base_desc.ptr < h->bo.iova + h->bo.size)
            heap = h;
    }
    if (!heap && list->sampler_heap) {
        UMD_DESCRIPTOR_HEAP *h = (UMD_DESCRIPTOR_HEAP *)list->sampler_heap;
        if (h->initialized && h->bo.host &&
            base_desc.ptr >= h->bo.iova &&
            base_desc.ptr < h->bo.iova + h->bo.size)
            heap = h;
    }
    if (!heap)
        return;

    offset = base_desc.ptr - heap->bo.iova;
    for (k = 0; k < max_cnt; k++) {
        uint64_t d_off = offset + (uint64_t)k * UMD_DESCRIPTOR_SIZE;
        uint32_t magic;
        const uint8_t *p;
        if (d_off + UMD_DESCRIPTOR_SIZE > heap->bo.size)
            break;
        p = (const uint8_t *)heap->bo.host + (size_t)d_off;
        memcpy(&magic, p, 4);
        if (magic == UMD_SRV_DESC_MAGIC) {
            UINT slot = base_reg + k;
            if (slot < ADRENO_3D_MAX_TEXTURES) {
                memcpy(&list->gfx_srvs[slot], p, sizeof(UMD_SRV_DESC));
                list->gfx_srv_mask |= (1u << slot);
            }
        } else if (magic == UMD_SAMP_DESC_MAGIC) {
            UINT slot = base_reg + k;
            if (slot < ADRENO_3D_MAX_SAMPLERS) {
                memcpy(&list->gfx_samplers[slot], p, sizeof(UMD_SAMP_DESC));
                list->gfx_samp_mask |= (1u << slot);
            }
        } else if (magic == UMD_CBV_DESC_MAGIC) {
            const UMD_CBV_DESC *cbv = (const UMD_CBV_DESC *)p;
            if (cbv->gpu_va)
                UmdSetGraphicsRootConstantBufferView(hCommandList, root_param, cbv->gpu_va);
        }
    }
}

static uint32_t umd_dxgi_to_a730_vfmt(DXGI_FORMAT fmt, uint32_t *out_mask, bool *out_float)
{
    *out_float = true;
    switch (fmt) {
    case DXGI_FORMAT_R32_FLOAT:
        *out_mask = 0x1u; return ADRENO_FMT_R32_FLOAT;
    case DXGI_FORMAT_R32G32_FLOAT:
        *out_mask = 0x3u; return ADRENO_FMT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G32B32_FLOAT:
        *out_mask = 0x7u; return ADRENO_FMT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        *out_mask = 0xfu; return ADRENO_FMT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        *out_mask = 0xfu; return ADRENO_FMT_R8G8B8A8_UNORM;
    default:
        *out_mask = 0xfu; return ADRENO_FMT_R32G32B32A32_FLOAT;
    }
}

static void umd_dxgi_to_a730_rtv(DXGI_FORMAT fmt, uint32_t *out_fmt, uint32_t *out_swap)
{
    *out_swap = ADRENO_SWAP_WZYX;
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        *out_fmt = ADRENO_FMT_R8G8B8A8_UNORM;
        *out_swap = ADRENO_SWAP_WXYZ;
        break;
    case DXGI_FORMAT_R32_FLOAT:
        *out_fmt = ADRENO_FMT_R32_FLOAT;
        break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        *out_fmt = ADRENO_FMT_R32G32B32A32_FLOAT;
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    default:
        *out_fmt = ADRENO_FMT_R8G8B8A8_UNORM;
        break;
    }
}

static void umd_emit_hw_draw(UMD_CMDLIST *list, UINT vertex_count,
                             UINT instance_count, UINT start_vertex,
                             UINT start_instance, BOOL indexed)
{
    UMD_PSO *pso = (UMD_PSO *)list->pso;
    UMD_RTV_DESC *rtv = (UMD_RTV_DESC *)list->rtv;
    UMD_RESOURCE *res;
    adreno_a730_draw_desc draw;
    adreno_dxbc_compiled *vs_c, *ps_c;
    UINT i;

    if (!pso || !pso->has_hw_gfx || !pso->vs || !pso->ps ||
        !pso->vs->compiled || !pso->ps->compiled)
        return;
    if (!rtv || rtv->magic != UMD_RTV_DESC_MAGIC || rtv->size == 0)
        return;
    res = (UMD_RESOURCE *)(uintptr_t)rtv->resource;
    if (!res || !res->texture2d || res->width == 0 || res->height == 0 ||
        res->row_pitch == 0)
        return;

    vs_c = pso->vs->compiled;
    ps_c = pso->ps->compiled;
    memset(&draw, 0, sizeof(draw));

    draw.rtv.iova   = rtv->gpu_va;
    draw.rtv.width  = res->width;
    draw.rtv.height = res->height;
    draw.rtv.pitch  = res->row_pitch;
    umd_dxgi_to_a730_rtv((DXGI_FORMAT)rtv->format, &draw.rtv.fmt, &draw.rtv.swap);

    if (list->has_viewport && list->viewport.Width > 0.0f && list->viewport.Height > 0.0f) {
        draw.vp_x     = list->viewport.TopLeftX;
        draw.vp_y     = list->viewport.TopLeftY;
        draw.vp_w     = list->viewport.Width;
        draw.vp_h     = list->viewport.Height;
        draw.vp_min_z = list->viewport.MinDepth;
        draw.vp_max_z = list->viewport.MaxDepth;
    } else {
        draw.vp_x = 0.0f; draw.vp_y = 0.0f;
        draw.vp_w = (float)res->width; draw.vp_h = (float)res->height;
        draw.vp_min_z = 0.0f; draw.vp_max_z = 1.0f;
    }
    if (list->has_scissor) {
        draw.scissor_x0 = (list->scissor.left > 0) ? (uint32_t)list->scissor.left : 0u;
        draw.scissor_y0 = (list->scissor.top > 0)  ? (uint32_t)list->scissor.top  : 0u;
        draw.scissor_x1 = (list->scissor.right > 0) ? (uint32_t)list->scissor.right : res->width;
        draw.scissor_y1 = (list->scissor.bottom > 0) ? (uint32_t)list->scissor.bottom : res->height;
    } else {
        draw.scissor_x0 = 0; draw.scissor_y0 = 0;
        draw.scissor_x1 = res->width; draw.scissor_y1 = res->height;
    }

    draw.prim_type      = list->prim_type ? list->prim_type : ADRENO_PRIM_TRILIST;
    draw.vertex_count   = vertex_count;
    draw.instance_count = instance_count;
    draw.start_vertex   = start_vertex;
    draw.start_instance = start_instance;

    if (indexed && list->has_ibuf) {
        draw.indexed          = true;
        draw.index_iova       = list->ibuf.BufferLocation;
        draw.index_size_bytes = (list->ibuf.Format == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
        draw.index_buf_bytes  = list->ibuf.SizeInBytes;
    }

    draw.num_vbufs = list->num_vbufs;
    for (i = 0; i < list->num_vbufs && i < ADRENO_3D_MAX_VBUFS; i++) {
        draw.vbufs[i].iova   = list->vbufs[i].BufferLocation;
        draw.vbufs[i].size   = list->vbufs[i].SizeInBytes;
        draw.vbufs[i].stride = list->vbufs[i].StrideInBytes;
    }
    if (pso->layout) {
        draw.num_vattrs = pso->layout->num_elements;
        for (i = 0; i < pso->layout->num_elements && i < ADRENO_3D_MAX_VATTRS; i++) {
            const D3D12DDIARG_INPUT_ELEMENT_DESC *ie = &pso->layout->elements[i];
            uint32_t mask = 0xfu;
            bool is_f = true;
            draw.vattrs[i].buf_idx   = ie->InputSlot;
            draw.vattrs[i].offset    = ie->AlignedByteOffset;
            draw.vattrs[i].fmt       = umd_dxgi_to_a730_vfmt(ie->Format, &mask, &is_f);
            draw.vattrs[i].dst_regid = IR3_REG(ie->InputRegister, 0);
            draw.vattrs[i].writemask = mask;
            draw.vattrs[i].is_float  = is_f;
        }
    }

    draw.vs.iova             = pso->vs_bo.iova;
    draw.vs.instrlen_units   = vs_c->instrlen_units;
    draw.vs.fullregfootprint = vs_c->fullregfootprint;
    draw.vs.halfregfootprint = vs_c->halfregfootprint;
    draw.vs.constlen         = vs_c->constlen;
    draw.vs.pos_regid        = vs_c->pos_regid;
    draw.vs.vertexid_regid   = vs_c->vertexid_regid;
    draw.vs.instanceid_regid = vs_c->instanceid_regid;
    draw.vs.num_varyings     = vs_c->num_varyings;
    for (i = 0; i < vs_c->num_varyings && i < ADRENO_3D_MAX_VARYINGS; i++) {
        draw.vs.varying_regid[i] = vs_c->varying_regid[i];
        draw.vs.varying_mask[i]  = vs_c->varying_mask[i];
    }
    if (list->gfx_const_vec4s > 0) {
        draw.vs.constants   = list->gfx_constants;
        draw.vs.const_vec4s = list->gfx_const_vec4s;
    }

    draw.ps.iova                = pso->ps_bo.iova;
    draw.ps.instrlen_units      = ps_c->instrlen_units;
    draw.ps.fullregfootprint    = ps_c->fullregfootprint;
    draw.ps.halfregfootprint    = ps_c->halfregfootprint;
    draw.ps.constlen            = ps_c->constlen;
    draw.ps.color0_regid        = ps_c->color0_regid;
    draw.ps.ij_regid            = ps_c->ij_regid;
    draw.ps.num_varying_scalars = ps_c->num_varying_scalars;
    if (list->gfx_const_vec4s > 0) {
        draw.ps.constants   = list->gfx_constants;
        draw.ps.const_vec4s = list->gfx_const_vec4s;
    }

    /* Pack bound PS textures (A6XX_TEX_MEMOBJ) & samplers (A6XX_TEX_SAMP) into tex_desc_bo */
    if (ps_c->num_textures > 0u && list->tex_desc_bo.host) {
        uint32_t nt = ps_c->num_textures;
        uint32_t ns = ps_c->num_samplers ? ps_c->num_samplers : 1u;
        uint32_t slot = list->tex_desc_slot & 15u; /* 16 slots x 1024B = 16KB */
        uint32_t slot_off = slot * 1024u;
        uint8_t *base_host = (uint8_t *)list->tex_desc_bo.host + slot_off;
        adreno_iova_t base_iova = list->tex_desc_bo.iova + slot_off;
        uint32_t *tex_mem  = (uint32_t *)base_host;           /* 0..511: 8 x 64B */
        uint32_t *samp_mem = (uint32_t *)(base_host + 512u);  /* 512..639: 8 x 16B */
        UMD_ROOTSIG *rs = (UMD_ROOTSIG *)list->root_signature;

        if (nt > ADRENO_3D_MAX_TEXTURES) nt = ADRENO_3D_MAX_TEXTURES;
        if (ns > ADRENO_3D_MAX_SAMPLERS) ns = ADRENO_3D_MAX_SAMPLERS;
        memset(base_host, 0, 1024u);

        for (i = 0; i < nt; i++) {
            if ((list->gfx_srv_mask & (1u << i)) &&
                list->gfx_srvs[i].magic == UMD_SRV_DESC_MAGIC) {
                UMD_RESOURCE *tres = (UMD_RESOURCE *)(uintptr_t)list->gfx_srvs[i].resource;
                if (tres && tres->texture2d) {
                    adreno_a730_tex_desc td;
                    memset(&td, 0, sizeof(td));
                    td.iova   = list->gfx_srvs[i].gpu_va;
                    td.width  = tres->width;
                    td.height = tres->height;
                    td.pitch  = tres->row_pitch;
                    umd_dxgi_to_a730_rtv((DXGI_FORMAT)list->gfx_srvs[i].format,
                                         &td.fmt, &td.swap);
                    td.srgb = ((DXGI_FORMAT)list->gfx_srvs[i].format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                               (DXGI_FORMAT)list->gfx_srvs[i].format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
                    adreno_a730_encode_tex_memobj(&td, &tex_mem[i * 16u]);
                }
            }
        }
        for (i = 0; i < ns; i++) {
            if ((list->gfx_samp_mask & (1u << i)) &&
                list->gfx_samplers[i].magic == UMD_SAMP_DESC_MAGIC) {
                memcpy(&samp_mem[i * 4u], list->gfx_samplers[i].dw, 16u);
            } else {
                bool found_static = false;
                if (rs) {
                    UINT k;
                    for (k = 0; k < rs->num_static_samplers; k++) {
                        if (rs->static_samplers[k].shader_register == i) {
                            memcpy(&samp_mem[i * 4u], rs->static_samplers[k].dw, 16u);
                            found_static = true;
                            break;
                        }
                    }
                }
                if (!found_static) {
                    adreno_a730_samp_desc def_s;
                    memset(&def_s, 0, sizeof(def_s));
                    def_s.mag_linear = true;
                    def_s.min_linear = true;
                    def_s.wrap_u = ADRENO_TEX_WRAP_CLAMP_TO_EDGE;
                    def_s.wrap_v = ADRENO_TEX_WRAP_CLAMP_TO_EDGE;
                    def_s.wrap_w = ADRENO_TEX_WRAP_CLAMP_TO_EDGE;
                    adreno_a730_encode_tex_samp(&def_s, &samp_mem[i * 4u]);
                }
            }
        }

        draw.ps.num_textures    = nt;
        draw.ps.tex_memobj_iova = base_iova;
        draw.ps.num_samplers    = ns;
        draw.ps.tex_samp_iova   = base_iova + 512ull;
        AdrenoArm64FlushDcache(base_host, 1024u);
        list->tex_desc_slot++;
    }

    if (adreno_d3d12_cmdlist_reserve(&list->cmd, 1024u) != ADRENO_OK)
        return;
    if (!adreno_a730_emit_draw_3d(&list->cmd.pm4, &draw))
        umd_log("adreno730_d3d12: HW 3D draw encode failed\n");
    else
        umd_trace("Draw3D HW OK: vtx=%u inst=%u idx=%d rtv=0x%llx (%ux%u) tex=%u samp=%u",
                  vertex_count, instance_count, (int)indexed,
                  (unsigned long long)draw.rtv.iova, res->width, res->height,
                  draw.ps.num_textures, draw.ps.num_samplers);
}

static VOID APIENTRY UmdDrawInstanced(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT vertex_count,
    UINT instance_count, UINT start_vertex, UINT start_instance)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_PSO *pso;
    UMD_RTV_DESC *rtv;
    UMD_RESOURCE *res;
    uint32_t width, height, x, y;
    uint64_t base;

    if (!list || !list->initialized || list->closed || !list->pso ||
        !list->rtv || instance_count == 0)
        return;
    pso = (UMD_PSO *)list->pso;
    if (pso->has_hw_gfx) {
        umd_emit_hw_draw(list, vertex_count, instance_count,
                         start_vertex, start_instance, FALSE);
        return;
    }
    if (!pso->reference_triangle)
        return;
    rtv = (UMD_RTV_DESC *)list->rtv;
    if (rtv->magic != UMD_RTV_DESC_MAGIC || rtv->size == 0)
        return;
    res = (UMD_RESOURCE *)(uintptr_t)rtv->resource;
    if (!res || !res->texture2d || res->row_pitch == 0)
        return;
    width = res->width;
    height = res->height;
    if (list->has_viewport) {
        if (list->viewport.Width > 0 &&
            list->viewport.Width < (FLOAT)width)
            width = (uint32_t)list->viewport.Width;
        if (list->viewport.Height > 0 &&
            list->viewport.Height < (FLOAT)height)
            height = (uint32_t)list->viewport.Height;
    }
    if (width == 0 || height == 0)
        return;
    if (adreno_d3d12_cmdlist_reserve(&list->cmd,
                                     (uint32_t)width * height * 4u + 8u) !=
        ADRENO_OK)
        return;
    base = rtv->gpu_va;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            float nx = ((float)x + 0.5f) / (float)width * 2.0f - 1.0f;
            float ny = 1.0f - ((float)y + 0.5f) / (float)height * 2.0f;
            if (nx + ny <= 0.0f) {
                if (!adreno_pm4_mem_write(&list->cmd.pm4,
                                          base + (uint64_t)y * res->row_pitch +
                                              (uint64_t)x * 4u,
                                          0xff0000ffu)) {
                    list->cmd.pm4.overflow = true;
                    return;
                }
            }
        }
    }
}

static VOID APIENTRY UmdDrawIndexedInstanced(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT index_count,
    UINT instance_count, UINT start_index, INT base_vertex,
    UINT start_instance)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    (void)start_index;
    if (!list || !list->initialized || list->closed || !list->pso ||
        !list->rtv || instance_count == 0)
        return;
    umd_emit_hw_draw(list, index_count, instance_count,
                     (UINT)(base_vertex > 0 ? base_vertex : 0),
                     start_instance, TRUE);
}

static VOID APIENTRY UmdSetComputeRootSignature(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_HROOTSIGNATURE hRootSignature)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    if (list)
        list->root_signature = hRootSignature.pDrvPrivate;
}

/*
 * Diagnostic root binding convention for the precompiled IR3 kernel:
 *   root parameter 0: UAV buffer GPU VA (becomes c0.xy)
 *   root parameter 1: four 32-bit constants (c1.xyzw; only c1.x is used)
 * A real root-signature parser will replace this once DXIL->IR3 exists.
 */
static VOID APIENTRY UmdSetDescriptorHeaps(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT NumDescriptorHeaps,
    D3D12DDI_HDESCRIPTORHEAP *pDescriptorHeaps)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UINT i;

    if (!list)
        return;
    list->descriptor_heap = NULL;
    list->sampler_heap = NULL;
    if (NumDescriptorHeaps == 0 || !pDescriptorHeaps)
        return;
    for (i = 0; i < NumDescriptorHeaps; i++) {
        UMD_DESCRIPTOR_HEAP *h = (UMD_DESCRIPTOR_HEAP *)pDescriptorHeaps[i].pDrvPrivate;
        if (!h) continue;
        if (h->type == D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            list->sampler_heap = h;
        else
            list->descriptor_heap = h;
    }
}

static VOID APIENTRY UmdSetComputeRootDescriptorTable(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT RootParameterIndex,
    D3D12DDI_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_ROOTSIG *root;
    UMD_DESCRIPTOR_HEAP *heap;
    UMD_UAV_DESC *desc;
    uint64_t offset;

    if (!list)
        return;
    root = (UMD_ROOTSIG *)list->root_signature;
    if (root && RootParameterIndex < root->num_params &&
        root->params[RootParameterIndex].type !=
            D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        umd_log("adreno730_d3d12: root parameter is not a descriptor table\n");
        return;
    }
    heap = (UMD_DESCRIPTOR_HEAP *)list->descriptor_heap;
    if (!heap || !heap->initialized || !heap->bo.host ||
        BaseDescriptor.ptr < heap->bo.iova)
        return;
    offset = BaseDescriptor.ptr - heap->bo.iova;
    if (offset + sizeof(UMD_UAV_DESC) > heap->bo.size)
        return;
    desc = (UMD_UAV_DESC *)((uint8_t *)heap->bo.host + (size_t)offset);
    if (desc->magic != UMD_UAV_DESC_MAGIC) {
        umd_log("adreno730_d3d12: descriptor table has no UAV\n");
        return;
    }
    list->compute_uav_va = desc->gpu_va;
    list->has_compute_uav = TRUE;
}

static VOID APIENTRY UmdSetComputeRoot32BitConstants(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT RootParameterIndex,
    UINT Num32BitValuesToSet, const void *pSrcData,
    UINT DestOffsetIn32BitValues)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_ROOTSIG *root;
    UINT count;

    if (!list || !pSrcData)
        return;
    root = (UMD_ROOTSIG *)list->root_signature;
    if (root && RootParameterIndex < root->num_params &&
        root->params[RootParameterIndex].type !=
            D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        umd_log("adreno730_d3d12: root parameter is not 32-bit constants\n");
        return;
    }
    if (DestOffsetIn32BitValues >= 4)
        return;
    count = Num32BitValuesToSet;
    if (count > 4u - DestOffsetIn32BitValues)
        count = 4u - DestOffsetIn32BitValues;
    memcpy(&list->compute_constants[DestOffsetIn32BitValues], pSrcData,
           count * sizeof(uint32_t));
}

static VOID APIENTRY UmdSetComputeRoot32BitConstant(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT RootParameterIndex,
    UINT SrcData, UINT DestOffsetIn32BitValues)
{
    UmdSetComputeRoot32BitConstants(hCommandList, RootParameterIndex, 1,
                                    &SrcData, DestOffsetIn32BitValues);
}

static VOID APIENTRY UmdSetComputeRootUnorderedAccessView(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT RootParameterIndex,
    D3D12DDI_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_ROOTSIG *root;

    if (!list)
        return;
    root = (UMD_ROOTSIG *)list->root_signature;
    if (root && RootParameterIndex < root->num_params &&
        root->params[RootParameterIndex].type !=
            D3D12DDI_ROOT_PARAMETER_TYPE_UAV) {
        umd_log("adreno730_d3d12: root parameter is not a UAV\n");
        return;
    }
    list->compute_uav_va = BufferLocation;
    list->has_compute_uav = BufferLocation != 0;
}

static VOID APIENTRY UmdDispatch(D3D12DDI_HCOMMANDLIST hCommandList,
                                 UINT x, UINT y, UINT z)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_PSO *pso;
    uint32_t constants[8] = {0};
    uint32_t grid[3];
    enum adreno_status st;

    if (!list || !list->initialized || list->closed)
        return;
    pso = (UMD_PSO *)list->pso;
    if (!pso || !pso->compute || !list->root_signature)
        return;
    if (!pso->has_ir3 || !list->has_compute_uav) {
        umd_log("adreno730_d3d12: dispatch skipped: no precompiled IR3 or "
                "root UAV\n");
        (void)adreno_pm4_set_marker(&list->cmd.pm4,
                                    0x7000u | (pso->id & 0xffu));
        return;
    }
    constants[0] = (uint32_t)list->compute_uav_va;
    constants[1] = (uint32_t)(list->compute_uav_va >> 32);
    memcpy(&constants[4], list->compute_constants,
           sizeof(list->compute_constants));
    grid[0] = x;
    grid[1] = y;
    grid[2] = z;
    umd_trace("Dispatch: uav=0x%llx c1=0x%08x,0x%08x sh=0x%llx(%u) regs=%u clen=%u grid=%u,%u,%u",
              (unsigned long long)list->compute_uav_va,
              constants[4], constants[5],
              (unsigned long long)pso->shader_bo.iova, pso->meta.instrlen_units,
              pso->meta.fullregfootprint, pso->meta.constlen, x, y, z);
    st = adreno_d3d12_cmd_dispatch_ir3(&list->cmd, &pso->shader_bo,
                                       &pso->meta, constants, 2, grid);
    if (st != ADRENO_OK)
        umd_log("adreno730_d3d12: IR3 dispatch encode failed\n");
}

/* ---- Command pools (D3D12 command allocators) --------------------------- */

static SIZE_T APIENTRY UmdCalcPrivateCommandPoolSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *args)
{
    (void)hDevice;
    (void)args;
    return sizeof(UMD_POOL);
}

static HRESULT APIENTRY UmdCreateCommandPool(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *args,
    D3D12DDI_HCOMMANDPOOL_0040 hPool)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("CreateCommandPool(dev=%p)", (void*)dev);
    UMD_POOL *pool = (UMD_POOL *)hPool.pDrvPrivate;

    (void)args;
    if (!dev || !pool)
        return E_INVALIDARG;
    memset(pool, 0, sizeof(*pool));
    pool->device = dev;
    return S_OK;
}

static VOID APIENTRY UmdDestroyCommandPool(D3D12DDI_HDEVICE hDevice,
                                           D3D12DDI_HCOMMANDPOOL_0040 hPool)
{
    UMD_POOL *pool = (UMD_POOL *)hPool.pDrvPrivate;

    (void)hDevice;
    if (pool)
        memset(pool, 0, sizeof(*pool));
}

static VOID APIENTRY UmdResetCommandPool(D3D12DDI_HDEVICE hDevice,
                                         D3D12DDI_HCOMMANDPOOL_0040 hPool)
{
    (void)hDevice;
    (void)hPool;
    /* Each command list owns its PM4 BO; nothing to recycle here yet. */
}

/* ---- Command lists ------------------------------------------------------ */

static SIZE_T APIENTRY UmdCalcPrivateCommandListSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0040 *args)
{
    (void)hDevice;
    (void)args;
    return sizeof(UMD_CMDLIST);
}

static HRESULT APIENTRY UmdCreateCommandList(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0040 *args,
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_HRTCOMMANDLIST hRTCommandList)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("CreateCommandList(dev=%p)", (void*)dev);
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;

    (void)hRTCommandList;
    if (!dev || !list || !args)
        return E_INVALIDARG;
    memset(list, 0, sizeof(*list));
    list->device = dev;
    list->h_rt_cmdlist = hRTCommandList;
    if (!dev->gpu_ready)
        return E_FAIL;
    if (adreno_d3d12_cmdlist_create(&dev->gpu, 1024, &list->cmd) !=
        ADRENO_OK)
        return E_OUTOFMEMORY;
    (void)adreno_d3d12_resource_create(&dev->gpu, 16384u, &list->tex_desc_bo);
    list->initialized = TRUE;
    if (dev->wddm_bridge && dev->wddm.core &&
        dev->wddm.core->pfnSetCommandListDDITableCb &&
        dev->adapter && dev->adapter->cmdlist_rt_table.handle) {
        dev->wddm.core->pfnSetCommandListDDITableCb(
            hRTCommandList, dev->adapter->cmdlist_rt_table);
        umd_trace("  SetCommandListDDITableCb OK (rt_table=%p)",
                  dev->adapter->cmdlist_rt_table.handle);
    }
    return S_OK;
}

static VOID APIENTRY UmdDestroyCommandList(D3D12DDI_HDEVICE hDevice,
                                           D3D12DDI_HCOMMANDLIST hCommandList)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;

    (void)hDevice;
    if (!list)
        return;
    if (list->tex_desc_bo.handle)
        adreno_d3d12_resource_destroy(&list->tex_desc_bo);
    if (list->initialized)
        adreno_d3d12_cmdlist_destroy(&list->cmd);
    memset(list, 0, sizeof(*list));
}

static VOID APIENTRY UmdCloseCommandList(D3D12DDI_HCOMMANDLIST hCommandList)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;

    if (list && list->initialized)
        list->closed = TRUE;
}

static VOID APIENTRY UmdResetCommandList(
    D3D12DDI_HCOMMANDLIST hCommandList,
    const D3D12DDIARG_RESETCOMMANDLIST_0040 *args)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;

    (void)args;
    if (!list || !list->initialized)
        return;
    adreno_d3d12_cmdlist_reset(&list->cmd);
    list->tex_desc_slot = 0;
    list->closed = FALSE;
    list->pso = NULL;
    list->root_signature = NULL;
    list->descriptor_heap = NULL;
    list->sampler_heap = NULL;
    list->compute_uav_va = 0;
    memset(list->compute_constants, 0, sizeof(list->compute_constants));
    list->has_compute_uav = FALSE;
    list->rtv = NULL;
    memset(&list->viewport, 0, sizeof(list->viewport));
    list->has_viewport = FALSE;
    list->has_scissor = FALSE;
    list->num_vbufs = 0;
    list->has_ibuf = FALSE;
    list->gfx_const_vec4s = 0;
    list->gfx_srv_mask = 0;
    list->gfx_samp_mask = 0;
}

static uint32_t umd_pack_rtv_color(DXGI_FORMAT format, const FLOAT color[4])
{
    uint32_t r, g, b, a;

    if (!color)
        return 0;
    if (format == DXGI_FORMAT_R32_FLOAT) {
        union {
            float f;
            uint32_t u;
        } v;
        v.f = color[0];
        return v.u;
    }
    r = (uint32_t)(color[0] * 255.0f + 0.5f);
    g = (uint32_t)(color[1] * 255.0f + 0.5f);
    b = (uint32_t)(color[2] * 255.0f + 0.5f);
    a = (uint32_t)(color[3] * 255.0f + 0.5f);
    if (r > 255u)
        r = 255u;
    if (g > 255u)
        g = 255u;
    if (b > 255u)
        b = 255u;
    if (a > 255u)
        a = 255u;
    return r | (g << 8) | (b << 16) | (a << 24);
}

static VOID APIENTRY UmdClearRenderTargetView(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE view_cpu, const FLOAT color[4],
    UINT num_rects, const D3D12DDI_RECT *rects)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_RTV_DESC *desc;
    UMD_RESOURCE *res;
    uint64_t offset;
    uint32_t dwords, packed;

    (void)num_rects;
    (void)rects;
    if (!list || !list->initialized || list->closed || !view_cpu.ptr ||
        !color)
        return;
    desc = (UMD_RTV_DESC *)(uintptr_t)view_cpu.ptr;
    if (desc->magic != UMD_RTV_DESC_MAGIC || desc->size == 0)
        return;
    res = (UMD_RESOURCE *)(uintptr_t)desc->resource;
    if (!res || desc->gpu_va < res->gpu_va)
        return;
    offset = desc->gpu_va - res->gpu_va;
    if (offset + desc->size > res->size || res->bo_offset > 0xffffffffull)
        return;
    if (res->texture2d && res->width > 0 && res->height > 0 &&
        res->row_pitch > 0 && (res->row_pitch & 63u) == 0) {
        adreno_a730_rtv a_rtv;
        memset(&a_rtv, 0, sizeof(a_rtv));
        a_rtv.iova   = desc->gpu_va;
        a_rtv.width  = res->width;
        a_rtv.height = res->height;
        a_rtv.pitch  = res->row_pitch;
        umd_dxgi_to_a730_rtv((DXGI_FORMAT)desc->format, &a_rtv.fmt, &a_rtv.swap);
        if (adreno_d3d12_cmdlist_reserve(&list->cmd, 256u) == ADRENO_OK &&
            adreno_a730_emit_clear_2d(&list->cmd.pm4, &a_rtv, color)) {
            umd_trace("ClearRTV 2D-HW OK: iova=0x%llx (%ux%u)",
                      (unsigned long long)a_rtv.iova, a_rtv.width, a_rtv.height);
            return;
        }
    }
    dwords = (uint32_t)(desc->size / 4u);
    if (dwords == 0)
        return;
    packed = umd_pack_rtv_color((DXGI_FORMAT)desc->format, color);
    if (adreno_d3d12_cmd_clear_u32(&list->cmd, &res->bo,
                                   (uint32_t)(res->bo_offset + offset),
                                   dwords, packed) != ADRENO_OK)
        umd_log("adreno730_d3d12: RTV clear encode failed\n");
}

static VOID APIENTRY UmdClearDepthStencilView(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE view_cpu, UINT clear_flags, FLOAT depth,
    UINT8 stencil, UINT num_rects, const D3D12DDI_RECT *rects)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_RTV_DESC *desc;
    UMD_RESOURCE *res;
    union {
        float f;
        uint32_t u;
    } value;
    uint64_t offset;
    uint32_t dwords;

    (void)stencil;
    (void)num_rects;
    (void)rects;
    if (!list || !list->initialized || list->closed || !view_cpu.ptr ||
        !(clear_flags & 1u))
        return;
    desc = (UMD_RTV_DESC *)(uintptr_t)view_cpu.ptr;
    if (desc->magic != UMD_DSV_DESC_MAGIC || desc->size == 0)
        return;
    res = (UMD_RESOURCE *)(uintptr_t)desc->resource;
    if (!res || desc->gpu_va < res->gpu_va)
        return;
    offset = desc->gpu_va - res->gpu_va;
    if (offset + desc->size > res->size || res->bo_offset > 0xffffffffull)
        return;
    dwords = (uint32_t)(desc->size / 4u);
    if (dwords == 0)
        return;
    value.f = depth;
    if (adreno_d3d12_cmd_clear_u32(&list->cmd, &res->bo,
                                   (uint32_t)(res->bo_offset + offset),
                                   dwords, value.u) != ADRENO_OK)
        umd_log("adreno730_d3d12: DSV clear encode failed\n");
}

static VOID APIENTRY UmdClearUnorderedAccessViewUint(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_GPU_DESCRIPTOR_HANDLE view_gpu,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE view_cpu,
    D3D12DDI_HRESOURCE hResource, const UINT values[4], UINT num_rects,
    const D3D12DDI_RECT *rects)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_RESOURCE *res = umd_resource(hResource);
    uint32_t dwords;

    (void)view_gpu;
    (void)view_cpu;
    (void)num_rects;
    (void)rects;
    if (!list || !list->initialized || list->closed || !res || !values)
        return;
    if (res->bo_offset > 0xffffffffull)
        return;
    dwords = res->size / 4;
    if (dwords == 0)
        return;
    /*
     * First-step semantics: a buffer UAV clear fills the whole resource with
     * component 0. Rect/texture clears and multi-component formats are not
     * implemented yet.
     */
    if (adreno_d3d12_cmd_clear_u32(&list->cmd, &res->bo,
                                   (uint32_t)res->bo_offset, dwords,
                                   values[0]) != ADRENO_OK)
        umd_log("adreno730_d3d12: UAV clear encode failed\n");
}

static VOID APIENTRY UmdCopyBufferRegion(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDIARG_BUFFER_PLACEMENT dst, D3D12DDIARG_BUFFER_PLACEMENT src,
    UINT64 src_bytes)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_RESOURCE *dst_res = umd_resource(dst.BaseAddress.UMD.hResource);
    UMD_RESOURCE *src_res = umd_resource(src.BaseAddress.UMD.hResource);
    uint64_t dst_off, src_off;
    uint64_t dst_bo_off, src_bo_off;
    uint64_t dwords;

    if (!list || !list->initialized || list->closed || !dst_res || !src_res)
        return;
    if (src_bytes == 0 || (src_bytes & 3) != 0)
        return;
    dst_off = dst.BaseAddress.UMD.Offset;
    src_off = src.BaseAddress.UMD.Offset;
    if (dst_off + src_bytes > dst_res->size ||
        src_off + src_bytes > src_res->size)
        return;
    dwords = src_bytes / 4;
    if (dwords > 0xffffffffull)
        return;
    dst_bo_off = dst_res->bo_offset + dst_off;
    src_bo_off = src_res->bo_offset + src_off;
    if (dst_bo_off > 0xffffffffull || src_bo_off > 0xffffffffull)
        return;
    if (adreno_d3d12_cmd_copy_u32(&list->cmd, &dst_res->bo,
                                  (uint32_t)dst_bo_off, &src_res->bo,
                                  (uint32_t)src_bo_off,
                                  (uint32_t)dwords) != ADRENO_OK)
        umd_log("adreno730_d3d12: CopyBufferRegion encode failed\n");
    else
        umd_trace("CopyBufferRegion OK: dst=0x%llx src=0x%llx dwords=%llu",
                  (unsigned long long)(dst_res->bo.iova + dst_bo_off),
                  (unsigned long long)(src_res->bo.iova + src_bo_off),
                  (unsigned long long)dwords);
}

static VOID APIENTRY UmdCopyTextureRegion(
    D3D12DDI_HCOMMANDLIST hCommandList,
    const D3D12DDIARG_BUFFER_PLACEMENT *dst_place,
    D3D12DDIARG_PLACED_RESOURCE dst_layout,
    UINT dst_x, UINT dst_y, UINT dst_z,
    const D3D12DDIARG_BUFFER_PLACEMENT *src_place,
    D3D12DDIARG_PLACED_RESOURCE src_layout,
    const D3D12DDI_BOX *src_box)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_RESOURCE *dst_res, *src_res;
    uint32_t dst_pitch, src_pitch, w, h, row_bytes, y;
    uint64_t dst_base, src_base;
    UINT sx = 0, sy = 0;

    (void)dst_z;
    if (!list || !list->initialized || list->closed || !dst_place || !src_place)
        return;
    dst_res = umd_resource(dst_place->BaseAddress.UMD.hResource);
    src_res = umd_resource(src_place->BaseAddress.UMD.hResource);
    if (!dst_res || !src_res)
        return;

    dst_pitch = dst_res->texture2d ? dst_res->row_pitch : 0u;
    src_pitch = src_res->texture2d ? src_res->row_pitch : 0u;
    w = src_res->texture2d ? src_res->width  : (dst_res->texture2d ? dst_res->width  : 0u);
    h = src_res->texture2d ? src_res->height : (dst_res->texture2d ? dst_res->height : 0u);

    if (dst_layout.Layout == D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED && dst_layout.pLayout) {
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *pl =
            (const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *)dst_layout.pLayout;
        dst_pitch = pl->Pitch;
        if (w == 0) w = pl->PhysicalWidth;
        if (h == 0) h = pl->PhysicalHeight;
    }
    if (src_layout.Layout == D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED && src_layout.pLayout) {
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *pl =
            (const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *)src_layout.pLayout;
        src_pitch = pl->Pitch;
        if (w == 0) w = pl->PhysicalWidth;
        if (h == 0) h = pl->PhysicalHeight;
    }
    if (src_box) {
        sx = (UINT)src_box->Left;
        sy = (UINT)src_box->Top;
        w  = (UINT)(src_box->Right - src_box->Left);
        h  = (UINT)(src_box->Bottom - src_box->Top);
    }
    if (w == 0 || h == 0)
        return;
    row_bytes = w * 4u;
    if (dst_pitch == 0) dst_pitch = row_bytes;
    if (src_pitch == 0) src_pitch = row_bytes;

    dst_base = dst_res->gpu_va + dst_place->BaseAddress.UMD.Offset +
               (uint64_t)dst_y * dst_pitch + (uint64_t)dst_x * 4u;
    src_base = src_res->gpu_va + src_place->BaseAddress.UMD.Offset +
               (uint64_t)sy * src_pitch + (uint64_t)sx * 4u;

    if (dst_pitch == src_pitch && dst_pitch == row_bytes) {
        if (adreno_d3d12_cmdlist_reserve(&list->cmd, 16u) == ADRENO_OK)
            adreno_pm4_memcpy(&list->cmd.pm4, dst_base, src_base, (row_bytes * h) / 4u);
    } else {
        if (adreno_d3d12_cmdlist_reserve(&list->cmd, h * 8u + 16u) == ADRENO_OK) {
            for (y = 0; y < h; y++) {
                adreno_pm4_memcpy(&list->cmd.pm4,
                                  dst_base + (uint64_t)y * dst_pitch,
                                  src_base + (uint64_t)y * src_pitch,
                                  row_bytes / 4u);
            }
        }
    }
    umd_trace("CopyTextureRegion OK: dst=0x%llx(p=%u) src=0x%llx(p=%u) %ux%u",
              (unsigned long long)dst_base, dst_pitch,
              (unsigned long long)src_base, src_pitch, w, h);
}

static VOID APIENTRY UmdAtomicCopyBufferRegion(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDIARG_BUFFER_PLACEMENT dst, D3D12DDIARG_BUFFER_PLACEMENT src,
    UINT64 src_bytes)
{
    UmdCopyBufferRegion(hCommandList, dst, src, src_bytes);
}

static VOID APIENTRY UmdResourceBarrier(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT count,
    const D3D12DDIARG_RESOURCE_BARRIER_0022 *barriers)
{
    (void)hCommandList;
    (void)count;
    (void)barriers;
    /* The bring-up CP stream has no cache/state hazards yet. */
}

static VOID APIENTRY UmdDiscardResource(
    D3D12DDI_HCOMMANDLIST hCommandList, D3D12DDI_HRESOURCE hResource,
    const D3D12DDIARG_DISCARD_RESOURCE_0003 *args)
{
    (void)hCommandList;
    (void)hResource;
    (void)args;
}

static VOID APIENTRY UmdWriteBufferImmediate(
    D3D12DDI_HCOMMANDLIST hCommandList, UINT count,
    const D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032 *params,
    const D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032 *modes)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UINT i;

    (void)modes;
    if (!list || !list->initialized || list->closed || !params)
        return;
    for (i = 0; i < count; i++) {
        UMD_RESOURCE *res =
            umd_find_va(list->device, params[i].Dst, sizeof(uint32_t));
        uint64_t bo_off;
        if (!res)
            continue;
        bo_off = res->bo_offset + (params[i].Dst - res->gpu_va);
        if (bo_off > 0xffffffffull)
            continue;
        if (adreno_d3d12_cmd_clear_u32(&list->cmd, &res->bo,
                                       (uint32_t)bo_off, 1,
                                       params[i].Value) != ADRENO_OK)
            umd_log("adreno730_d3d12: WriteBufferImmediate failed\n");
    }
}

/* ---- Command queues and fences ----------------------------------------- */

static SIZE_T APIENTRY UmdCalcPrivateCommandQueueSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *args)
{
    (void)hDevice;
    (void)args;
    return sizeof(UMD_QUEUE);
}

static HRESULT APIENTRY UmdCreateCommandQueue(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *args,
    D3D12DDI_HCOMMANDQUEUE hCommandQueue,
    D3D12DDI_HRTCOMMANDQUEUE hRTCommandQueue)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    umd_trace("CreateCommandQueue(dev=%p)", (void*)dev);
    UMD_QUEUE *queue = (UMD_QUEUE *)hCommandQueue.pDrvPrivate;

    (void)args;
    if (!dev || !queue)
        return E_INVALIDARG;
    memset(queue, 0, sizeof(*queue));
    queue->device = dev;
    if (dev->wddm_bridge) {
        HRESULT hr = E_FAIL;
        if (dev->wddm.core && dev->wddm.core->pfnCreateContextVirtualCb) {
            D3DDDICB_CREATECONTEXTVIRTUAL cv;
            memset(&cv, 0, sizeof(cv));
            cv.NodeOrdinal = 0;
            cv.EngineAffinity = 1;
            hr = dev->wddm.core->pfnCreateContextVirtualCb(hRTCommandQueue, &cv);
            umd_trace("CreateContextVirtualCb: hr=0x%08lx hCtx=%p", (unsigned long)hr, cv.hContext);
            if (SUCCEEDED(hr) && cv.hContext) {
                dev->wddm.h_context = cv.hContext;
                dev->wddm.h_rt_queue = hRTCommandQueue;
            }
        }
        if (!dev->wddm.h_context && dev->wddm.core && dev->wddm.core->pfnCreateContextCb) {
            D3DDDICB_CREATECONTEXT create;
            memset(&create, 0, sizeof(create));
            create.NodeOrdinal = 0;
            create.EngineAffinity = 1;
            hr = dev->wddm.core->pfnCreateContextCb(hRTCommandQueue, &create);
            umd_trace("CreateContextCb: hr=0x%08lx hCtx=%p", (unsigned long)hr, create.hContext);
            if (SUCCEEDED(hr) && create.hContext) {
                dev->wddm.h_context = create.hContext;
                dev->wddm.h_rt_queue = hRTCommandQueue;
            }
        }
        if (!dev->wddm.h_context) {
            umd_log("adreno730_d3d12: WDDM CreateContext failed\n");
            return E_FAIL;
        }
    }
    return S_OK;
}

static VOID APIENTRY UmdDestroyCommandQueue(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDQUEUE hCommandQueue)
{
    UMD_QUEUE *queue = (UMD_QUEUE *)hCommandQueue.pDrvPrivate;
    UMD_DEVICE *dev = queue ? queue->device : NULL;

    (void)hDevice;
    if (dev && dev->wddm_bridge && dev->wddm.h_hw_queue) {
        D3DDDICB_DESTROYHWQUEUE destroy_q;
        memset(&destroy_q, 0, sizeof(destroy_q));
        destroy_q.hHwQueue = dev->wddm.h_hw_queue;
        if (dev->wddm.kt && dev->wddm.kt->pfnDestroyHwQueueCb)
            (void)dev->wddm.kt->pfnDestroyHwQueueCb(dev->wddm.h_device,
                                                    &destroy_q);
        dev->wddm.h_hw_queue = NULL;
        dev->wddm.use_hw_queue = FALSE;
        dev->wddm.hw_fence_cpu = NULL;
    }
    if (dev && dev->wddm_bridge && dev->wddm.h_context) {
        D3DDDICB_DESTROYCONTEXT destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.hContext = dev->wddm.h_context;
        if (dev->wddm.core && dev->wddm.core->pfnDestroyContextCb)
            (void)dev->wddm.core->pfnDestroyContextCb(dev->wddm.h_rt_queue,
                                                      &destroy);
        dev->wddm.h_context = NULL;
    }
    if (hCommandQueue.pDrvPrivate)
        memset(hCommandQueue.pDrvPrivate, 0, sizeof(UMD_QUEUE));
}

static VOID APIENTRY UmdExecuteCommandLists(
    D3D12DDI_HCOMMANDQUEUE hCommandQueue, UINT count,
    const D3D12DDI_HCOMMANDLIST *command_lists)
{
    UMD_QUEUE *queue = (UMD_QUEUE *)hCommandQueue.pDrvPrivate;
    adreno_d3d12_cmdlist *cmd_ptrs[32];
    uint32_t seqno;
    UINT i, n = 0;
    enum adreno_status st;

    umd_trace("ExecuteCommandLists(count=%u)", count);
    if (!queue || !queue->device || !queue->device->gpu_ready ||
        !command_lists || count == 0 || count > 32) {
        umd_log("adreno730_d3d12: ExecuteCommandLists rejected\n");
        return;
    }
    for (i = 0; i < count; i++) {
        UMD_CMDLIST *list =
            (UMD_CMDLIST *)command_lists[i].pDrvPrivate;
        if (!list || !list->initialized || !list->closed) {
            umd_log("adreno730_d3d12: command list not closed\n");
            return;
        }
        if (list->cmd.cmd_bo.host && list->cmd.pm4.count)
            AdrenoArm64FlushDcache(list->cmd.cmd_bo.host,
                                   (size_t)list->cmd.pm4.count * 4u);
        cmd_ptrs[n++] = &list->cmd;
    }
    umd_flush_all_resources(queue->device);
    seqno = queue->device->gpu.next_seqno + 1;
    st = adreno_d3d12_submit_lists(&queue->device->gpu, cmd_ptrs, n, seqno);
    if (st != ADRENO_OK) {
        umd_log("adreno730_d3d12: submit failed\n");
        return;
    }
    queue->last_seqno = seqno;
}

static SIZE_T APIENTRY UmdCalcPrivateFenceSize(
    D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_FENCE *args)
{
    (void)hDevice;
    (void)args;
    return sizeof(UMD_FENCE);
}

static HRESULT APIENTRY UmdCreateFence(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HFENCE hFence,
    const D3D12DDIARG_CREATE_FENCE *args)
{
    UMD_DEVICE *dev = umd_device(hDevice);
    UMD_FENCE *fence = (UMD_FENCE *)hFence.pDrvPrivate;

    if (!dev || !fence)
        return E_INVALIDARG;
    memset(fence, 0, sizeof(*fence));
    fence->device = dev;
    if (args && args->Fences && args->FenceCount) {
        fence->value_va = args->Fences[0].FenceValue.BaseAddress;
        fence->monitored_va = args->Fences[0].FenceMonitoredValue.BaseAddress;
    }
    return S_OK;
}

static VOID APIENTRY UmdDestroyFence(D3D12DDI_HDEVICE hDevice,
                                     D3D12DDI_HFENCE hFence)
{
    UMD_FENCE *fence = (UMD_FENCE *)hFence.pDrvPrivate;

    (void)hDevice;
    if (fence)
        memset(fence, 0, sizeof(*fence));
}

/*
 * Write the 64-bit fence value to the GPU-visible location supplied by the
 * runtime. This is a separate tiny submission on purpose: the first-step UMD
 * has no command queue ring of its own, but it must still make
 * FenceValue/FenceMonitoredValue visible to the runtime.
 */
static void umd_fence_write(UMD_DEVICE *dev, uint64_t va, UINT64 value)
{
    adreno_d3d12_cmdlist cmd;
    uint32_t seqno;
    enum adreno_status st;

    if (!dev || !dev->gpu_ready || va == 0)
        return;
    st = adreno_d3d12_cmdlist_create(&dev->gpu, 64, &cmd);
    if (st != ADRENO_OK)
        return;
    if (!adreno_pm4_mem_write(&cmd.pm4, va, (uint32_t)value) ||
        !adreno_pm4_mem_write(&cmd.pm4, va + 4, (uint32_t)(value >> 32))) {
        adreno_d3d12_cmdlist_destroy(&cmd);
        return;
    }
    seqno = dev->gpu.next_seqno + 1;
    if (adreno_d3d12_submit(&dev->gpu, &cmd, seqno) == ADRENO_OK)
        (void)adreno_d3d12_wait(&dev->gpu, seqno, 5000);
    adreno_d3d12_cmdlist_destroy(&cmd);
}

static VOID APIENTRY UmdSignalFence(D3D12DDI_HCOMMANDQUEUE hCommandQueue,
                                    D3D12DDIARG_FENCE_OPERATION *op)
{
    UMD_QUEUE *queue = (UMD_QUEUE *)hCommandQueue.pDrvPrivate;
    UMD_FENCE *fence = op ? (UMD_FENCE *)op->Fence.pDrvPrivate : NULL;

    umd_trace("SignalFence(val=%llu val_va=0x%llx mon_va=0x%llx last_seq=%u)",
              op ? (unsigned long long)op->Value : 0ull,
              fence ? (unsigned long long)fence->value_va : 0ull,
              fence ? (unsigned long long)fence->monitored_va : 0ull,
              queue ? queue->last_seqno : 0u);
    if (!queue || !fence || !queue->device)
        return;
    fence->pending_value = op->Value;
    fence->pending_seqno = queue->last_seqno;
    if (queue->last_seqno) {
        if (adreno_d3d12_wait(&queue->device->gpu, queue->last_seqno,
                              10000) != ADRENO_OK) {
            umd_log("adreno730_d3d12: fence signal wait failed\n");
            return;
        }
        umd_flush_all_resources(queue->device);
    }
    umd_fence_write(queue->device, fence->value_va, op->Value);
    if (fence->monitored_va && fence->monitored_va != fence->value_va)
        umd_fence_write(queue->device, fence->monitored_va, op->Value);
    if (op->Value > fence->value)
        fence->value = op->Value;
}

static VOID APIENTRY UmdWaitForFence(D3D12DDI_HCOMMANDQUEUE hCommandQueue,
                                     D3D12DDIARG_FENCE_OPERATION *op)
{
    UMD_QUEUE *queue = (UMD_QUEUE *)hCommandQueue.pDrvPrivate;
    UMD_FENCE *fence = op ? (UMD_FENCE *)op->Fence.pDrvPrivate : NULL;

    if (!queue || !fence || !queue->device)
        return;
    if (fence->value >= op->Value)
        return;
    if (fence->pending_seqno) {
        if (adreno_d3d12_wait(&queue->device->gpu, fence->pending_seqno,
                              10000) == ADRENO_OK &&
            fence->pending_value > fence->value)
            fence->value = fence->pending_value;
    }
}

/* ---- DDI tables --------------------------------------------------------- */

static HRESULT APIENTRY UmdStubOk(void)
{
    static int stub_logs = 0;
    if (stub_logs < 200) {
        stub_logs++;
        umd_trace("UmdStubOk invoked (#%d)", stub_logs);
    }
    return S_OK;
}

#define UMD_DEF_STUB(n) static HRESULT APIENTRY UmdStub_##n(void) { \
    static int c = 0; if (++c <= 4) umd_trace("UmdStub slot=%d (call #%d)", n, c); return S_OK; }
#define UMD_DEF_STUB10(b) UMD_DEF_STUB(b##0) UMD_DEF_STUB(b##1) UMD_DEF_STUB(b##2) UMD_DEF_STUB(b##3) UMD_DEF_STUB(b##4) \
                          UMD_DEF_STUB(b##5) UMD_DEF_STUB(b##6) UMD_DEF_STUB(b##7) UMD_DEF_STUB(b##8) UMD_DEF_STUB(b##9)
UMD_DEF_STUB(0) UMD_DEF_STUB(1) UMD_DEF_STUB(2) UMD_DEF_STUB(3) UMD_DEF_STUB(4)
UMD_DEF_STUB(5) UMD_DEF_STUB(6) UMD_DEF_STUB(7) UMD_DEF_STUB(8) UMD_DEF_STUB(9)
UMD_DEF_STUB10(1) UMD_DEF_STUB10(2) UMD_DEF_STUB10(3) UMD_DEF_STUB10(4) UMD_DEF_STUB10(5)
UMD_DEF_STUB10(6) UMD_DEF_STUB10(7) UMD_DEF_STUB10(8) UMD_DEF_STUB10(9) UMD_DEF_STUB10(10)
UMD_DEF_STUB10(11) UMD_DEF_STUB10(12) UMD_DEF_STUB10(13) UMD_DEF_STUB10(14) UMD_DEF_STUB10(15)

#define UMD_REF_STUB10(b) (void*)UmdStub_##b##0, (void*)UmdStub_##b##1, (void*)UmdStub_##b##2, (void*)UmdStub_##b##3, (void*)UmdStub_##b##4, \
                          (void*)UmdStub_##b##5, (void*)UmdStub_##b##6, (void*)UmdStub_##b##7, (void*)UmdStub_##b##8, (void*)UmdStub_##b##9
static void *g_umd_slot_stubs[160] = {
    (void*)UmdStub_0, (void*)UmdStub_1, (void*)UmdStub_2, (void*)UmdStub_3, (void*)UmdStub_4,
    (void*)UmdStub_5, (void*)UmdStub_6, (void*)UmdStub_7, (void*)UmdStub_8, (void*)UmdStub_9,
    UMD_REF_STUB10(1), UMD_REF_STUB10(2), UMD_REF_STUB10(3), UMD_REF_STUB10(4), UMD_REF_STUB10(5),
    UMD_REF_STUB10(6), UMD_REF_STUB10(7), UMD_REF_STUB10(8), UMD_REF_STUB10(9), UMD_REF_STUB10(10),
    UMD_REF_STUB10(11), UMD_REF_STUB10(12), UMD_REF_STUB10(13), UMD_REF_STUB10(14), UMD_REF_STUB10(15)
};

static UINT APIENTRY UmdGetImplicitPhysicalAdapterMask(D3D12DDI_HDEVICE hDevice)
{
    (void)hDevice;
    return 1u;
}

static VOID APIENTRY UmdQueryNodeMap(D3D12DDI_HDEVICE hDevice, UINT numAdapters, UINT *pMap)
{
    UINT i;
    (void)hDevice;
    if (pMap) {
        for (i = 0; i < numAdapters; i++)
            pMap[i] = 0;
    }
}

static VOID APIENTRY UmdGetMipPacking(D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hResource,
                                      UINT *pNumPackedMips, UINT *pNumTilesForPackedMips)
{
    (void)hDevice;
    (void)hResource;
    if (pNumPackedMips) *pNumPackedMips = 0;
    if (pNumTilesForPackedMips) *pNumTilesForPackedMips = 0;
}

static VOID APIENTRY UmdCheckSubresourceInfo(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hResource,
    UINT subresource, D3D12DDI_SUBRESOURCE_INFO *out)
{
    UMD_RESOURCE *res = umd_resource(hResource);
    (void)hDevice;
    (void)subresource;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (res) {
        out->Offset = res->bo_offset;
        out->RowStride = res->row_pitch ? res->row_pitch : res->size;
        out->DepthStride = res->size;
    }
}

/* ---- DXGI SwapChain Present & Blt --------------------------------------- */

static VOID APIENTRY UmdBlt(D3D12DDI_HCOMMANDLIST hCommandList,
                            const D3D12DDIARG_BLT *args)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_RESOURCE *dst, *src;
    uint32_t sw, sh, dw, dh, cw, ch;
    adreno_a730_rtv d_rtv, s_rtv;

    if (!list || !list->initialized || list->closed || !args)
        return;
    dst = umd_resource(args->hDstResource);
    src = umd_resource(args->hSrcResource);
    if (!dst || !src || !dst->texture2d || !src->texture2d)
        return;
    sw = (args->SrcRight > args->SrcLeft) ? (args->SrcRight - args->SrcLeft) : src->width;
    sh = (args->SrcBottom > args->SrcTop) ? (args->SrcBottom - args->SrcTop) : src->height;
    dw = (args->DstRight > args->DstLeft) ? (args->DstRight - args->DstLeft) : dst->width;
    dh = (args->DstBottom > args->DstTop) ? (args->DstBottom - args->DstTop) : dst->height;
    cw = (sw < dw) ? sw : dw;
    ch = (sh < dh) ? sh : dh;
    if (cw == 0 || ch == 0)
        return;

    memset(&d_rtv, 0, sizeof(d_rtv));
    memset(&s_rtv, 0, sizeof(s_rtv));
    d_rtv.iova = dst->gpu_va; d_rtv.width = dst->width; d_rtv.height = dst->height; d_rtv.pitch = dst->row_pitch;
    s_rtv.iova = src->gpu_va; s_rtv.width = src->width; s_rtv.height = src->height; s_rtv.pitch = src->row_pitch;
    umd_dxgi_to_a730_rtv(dst->format, &d_rtv.fmt, &d_rtv.swap);
    umd_dxgi_to_a730_rtv(src->format, &s_rtv.fmt, &s_rtv.swap);

    if (adreno_d3d12_cmdlist_reserve(&list->cmd, 128u) == ADRENO_OK) {
        (void)adreno_a730_emit_copy_2d(&list->cmd.pm4,
                                       &d_rtv, args->DstLeft, args->DstTop,
                                       &s_rtv, args->SrcLeft, args->SrcTop,
                                       cw, ch);
        umd_trace("UmdBlt 2D-HW: dst=0x%llx src=0x%llx %ux%u",
                  (unsigned long long)dst->gpu_va, (unsigned long long)src->gpu_va, cw, ch);
    }
}

static VOID APIENTRY UmdResourceCopy(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_HRESOURCE hDstResource,
    D3D12DDI_HRESOURCE hSrcResource)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_RESOURCE *dst = umd_resource(hDstResource);
    UMD_RESOURCE *src = umd_resource(hSrcResource);

    if (!list || !list->initialized || list->closed || !dst || !src)
        return;

    if (dst->texture2d && src->texture2d &&
        dst->width > 0 && dst->height > 0 && src->width > 0 && src->height > 0) {
        adreno_a730_rtv d_rtv, s_rtv;
        uint32_t cw = (src->width < dst->width) ? src->width : dst->width;
        uint32_t ch = (src->height < dst->height) ? src->height : dst->height;
        memset(&d_rtv, 0, sizeof(d_rtv));
        memset(&s_rtv, 0, sizeof(s_rtv));
        d_rtv.iova = dst->gpu_va; d_rtv.width = dst->width; d_rtv.height = dst->height; d_rtv.pitch = dst->row_pitch;
        s_rtv.iova = src->gpu_va; s_rtv.width = src->width; s_rtv.height = src->height; s_rtv.pitch = src->row_pitch;
        umd_dxgi_to_a730_rtv(dst->format, &d_rtv.fmt, &d_rtv.swap);
        umd_dxgi_to_a730_rtv(src->format, &s_rtv.fmt, &s_rtv.swap);
        if (cw > 0 && ch > 0 &&
            adreno_d3d12_cmdlist_reserve(&list->cmd, 128u) == ADRENO_OK) {
            (void)adreno_a730_emit_copy_2d(&list->cmd.pm4, &d_rtv, 0, 0, &s_rtv, 0, 0, cw, ch);
            umd_trace("ResourceCopy 2D-HW: dst=0x%llx src=0x%llx %ux%u",
                      (unsigned long long)dst->gpu_va, (unsigned long long)src->gpu_va, cw, ch);
        }
    } else {
        uint32_t bytes = (dst->size < src->size) ? dst->size : src->size;
        uint32_t dwords = bytes / 4u;
        if (dwords > 0 && dst->bo_offset <= 0xffffffffull && src->bo_offset <= 0xffffffffull) {
            (void)adreno_d3d12_cmd_copy_u32(&list->cmd, &dst->bo, (uint32_t)dst->bo_offset,
                                            &src->bo, (uint32_t)src->bo_offset, dwords);
            umd_trace("ResourceCopy BUF: dst=0x%llx src=0x%llx dwords=%u",
                      (unsigned long long)dst->gpu_va, (unsigned long long)src->gpu_va, dwords);
        }
    }
}

static VOID APIENTRY UmdPresent(
    D3D12DDI_HCOMMANDLIST hCommandList,
    D3D12DDI_HCOMMANDQUEUE hCommandQueue,
    const D3D12DDIARG_PRESENT_0001 *args,
    D3D12DDI_PRESENT_0051 *out_present,
    D3D12DDI_PRESENT_CONTEXTS_0051 *out_contexts,
    D3D12DDI_PRESENT_HWQUEUES_0051 *out_hwqueues)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    UMD_QUEUE *queue  = (UMD_QUEUE *)hCommandQueue.pDrvPrivate;
    UMD_DEVICE *dev   = queue ? queue->device : (list ? list->device : NULL);
    UMD_RESOURCE *src = NULL, *dst = NULL;

    if (out_present)
        memset(out_present, 0, sizeof(*out_present));
    if (out_contexts)
        memset(out_contexts, 0, sizeof(*out_contexts));
    if (out_hwqueues)
        memset(out_hwqueues, 0, sizeof(*out_hwqueues));

    if (args && args->SurfacesToPresent > 0 && args->phSurfacesToPresent)
        src = umd_resource(args->phSurfacesToPresent[0].hSurface);
    if (args && args->hDstResource.pDrvPrivate)
        dst = umd_resource(args->hDstResource);

    if (out_present) {
        if (src)
            out_present->BroadcastSrcAllocation[0] = (D3DKMT_HANDLE)src->bo.handle;
        if (dst)
            out_present->BroadcastDstAllocation[0] = (D3DKMT_HANDLE)dst->bo.handle;
        out_present->BackBufferMultiplicity = 1;
        out_present->AddedGpuWork = FALSE;

        /* If runtime requested a Blt present into a separate destination surface, emit HW 2D copy */
        if (list && list->initialized && !list->closed &&
            src && dst && src != dst && src->texture2d && dst->texture2d &&
            args && args->Flags.Blt) {
            adreno_a730_rtv d_rtv, s_rtv;
            uint32_t cw = (src->width < dst->width) ? src->width : dst->width;
            uint32_t ch = (src->height < dst->height) ? src->height : dst->height;
            memset(&d_rtv, 0, sizeof(d_rtv));
            memset(&s_rtv, 0, sizeof(s_rtv));
            d_rtv.iova = dst->gpu_va; d_rtv.width = dst->width; d_rtv.height = dst->height; d_rtv.pitch = dst->row_pitch;
            s_rtv.iova = src->gpu_va; s_rtv.width = src->width; s_rtv.height = src->height; s_rtv.pitch = src->row_pitch;
            umd_dxgi_to_a730_rtv(dst->format, &d_rtv.fmt, &d_rtv.swap);
            umd_dxgi_to_a730_rtv(src->format, &s_rtv.fmt, &s_rtv.swap);
            if (cw > 0 && ch > 0 &&
                adreno_d3d12_cmdlist_reserve(&list->cmd, 128u) == ADRENO_OK &&
                adreno_a730_emit_copy_2d(&list->cmd.pm4, &d_rtv, 0, 0, &s_rtv, 0, 0, cw, ch)) {
                out_present->AddedGpuWork = TRUE;
            }
        }
    }

    if (out_contexts && dev && dev->wddm.h_context) {
        out_contexts->hContext = dev->wddm.h_context;
        out_contexts->BroadcastContextCount = 0;
    }

    umd_trace("UmdPresent: src=0x%x(%ux%u) dst=0x%x flags=0x%x vidpn=%u ctx=%p gpuWork=%d",
              src ? (unsigned)src->bo.handle : 0u,
              src ? src->width : 0u, src ? src->height : 0u,
              dst ? (unsigned)dst->bo.handle : 0u,
              args ? (unsigned)args->Flags.Value : 0u,
              args ? (unsigned)args->VidPnSourceID : 0u,
              (dev && dev->wddm.h_context) ? dev->wddm.h_context : NULL,
              out_present ? (int)out_present->AddedGpuWork : 0);
}

static HRESULT APIENTRY UmdDxgiGetGammaCaps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *args)
{
    if (!args || !args->pGammaCapabilities)
        return E_INVALIDARG;
    memset(args->pGammaCapabilities, 0, sizeof(*args->pGammaCapabilities));
    args->pGammaCapabilities->ScaleAndOffsetSupported = FALSE;
    args->pGammaCapabilities->MaxConvertedValue = 1.0f;
    args->pGammaCapabilities->MinConvertedValue = 0.0f;
    args->pGammaCapabilities->NumGammaControlPoints = 0;
    return S_OK;
}

static HRESULT APIENTRY UmdDxgiQueryResourceResidency(
    DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *args)
{
    UINT i;
    if (!args)
        return E_INVALIDARG;
    for (i = 0; i < args->Resources; i++) {
        if (args->pStatus)
            args->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
    }
    return S_OK;
}

static HRESULT APIENTRY UmdDxgiCheckMpoColorSpace(
    DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYCOLORSPACESUPPORT *args)
{
    if (!args)
        return E_INVALIDARG;
    args->Supported = (args->ColorSpace == D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
                       args->ColorSpace == D3DDDI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) ? TRUE : FALSE;
    return S_OK;
}

static HRESULT APIENTRY UmdDxgiPresent(DXGI_DDI_ARG_PRESENT *args)
{
    umd_trace("UmdDxgiPresent(src=%p flags=0x%x)",
              args ? (void *)(uintptr_t)args->hSurfaceToPresent : NULL,
              args ? (unsigned)args->Flags.Value : 0u);
    return S_OK;
}

static HRESULT APIENTRY UmdDxgiPresent1(DXGI1_6_1_DDI_ARG_PRESENT *args)
{
    umd_trace("UmdDxgiPresent1(surfs=%u flags=0x%x)",
              args ? args->SurfacesToPresent : 0u,
              args ? (unsigned)args->Flags.Value : 0u);
    return S_OK;
}

static HRESULT APIENTRY UmdDxgiOk1(void *arg)
{
    (void)arg;
    return S_OK;
}

static VOID APIENTRY UmdCheckExistingResourceAllocationInfo(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hResource,
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out)
{
    UMD_RESOURCE *res = umd_resource(hResource);
    (void)hDevice;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ResourceDataSize = res ? ((res->size + 65535ull) & ~65535ull) : 65536;
    out->ResourceDataAlignment = 65536;
    out->Layout = D3D12DDI_TL_ROW_MAJOR;
}

static VOID APIENTRY UmdCheckResourceAllocationInfo(
    D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATERESOURCE_0088 *desc,
    D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT32 alignRestr,
    UINT visibleNodeMask, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out)
{
    UINT64 sz = 65536;
    (void)hDevice; (void)flags; (void)alignRestr; (void)visibleNodeMask;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (desc) {
        if (desc->ResourceType == D3D12DDI_RT_BUFFER) {
            sz = (desc->Width + 65535ull) & ~65535ull;
            if (sz == 0) sz = 65536;
        } else {
            UINT64 pitch = ((UINT64)desc->Width * 4ull + 255ull) & ~255ull;
            UINT64 h = desc->Height ? desc->Height : 1;
            sz = (pitch * h + 65535ull) & ~65535ull;
            if (sz == 0) sz = 65536;
        }
    }
    out->ResourceDataSize = sz;
    out->ResourceDataAlignment = 65536;
    out->AdditionalDataHeaderAlignment = 0;
    out->AdditionalDataSize = 0;
    out->Layout = D3D12DDI_TL_ROW_MAJOR;
}

static HRESULT APIENTRY UmdGetSupportedExtendedFeatures(
    D3D12DDI_HDEVICE hDevice, UINT32 *count, D3D12DDI_FEATURE_0020 *features)
{
    (void)hDevice; (void)features;
    if (!count) return E_INVALIDARG;
    *count = 0;
    return S_OK;
}

static HRESULT APIENTRY UmdGetSupportedExtendedFeatureVersions(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_FEATURE_0020 feature, UINT32 *count, UINT32 *versions)
{
    (void)hDevice; (void)feature; (void)versions;
    if (count) *count = 0;
    return E_INVALIDARG;
}

static HRESULT APIENTRY UmdEnableExtendedFeature(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_FEATURE_0020 feature, UINT32 version)
{
    (void)hDevice; (void)feature; (void)version;
    return E_INVALIDARG;
}

static HRESULT APIENTRY UmdSetExtendedFeatureCallbacks(
    D3D12DDI_HDEVICE hDevice, D3D12DDI_TABLE_TYPE table, const void *pTable, SIZE_T size)
{
    (void)hDevice; (void)table; (void)pTable; (void)size;
    return S_OK;
}

static void umd_fill_null_slots(void *table, SIZE_T size)
{
    void **slots = (void **)table;
    SIZE_T i, n = size / sizeof(void *);
    for (i = 0; i < n; i++) {
        if (!slots[i])
            slots[i] = (i < 160) ? g_umd_slot_stubs[i] : (void *)UmdStubOk;
    }
}

static HRESULT APIENTRY UmdFillDDITable(D3D12DDI_HADAPTER hAdapter,
                                        D3D12DDI_TABLE_TYPE type, VOID *table,
                                        SIZE_T size, UINT table_version,
                                        D3D12DDI_HRTTABLE rt_table)
{
    (void)hAdapter;
    (void)table_version;
    (void)rt_table;
    umd_trace("FillDDITable(type=%u size=%llu ver=0x%x)", (unsigned)type, (unsigned long long)size, table_version);
    if (!table)
        return E_INVALIDARG;

    switch (type) {
    case D3D12DDI_TABLE_TYPE_DEVICE_CORE: {
        D3D12DDI_DEVICE_FUNCS_CORE_0088 *funcs;
        if (size < sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088))
            return E_INVALIDARG;
        funcs = (D3D12DDI_DEVICE_FUNCS_CORE_0088 *)table;
        memset(funcs, 0, size);
        funcs->pfnCheckFormatSupport = UmdCheckFormatSupport;
        funcs->pfnCheckMultisampleQualityLevels =
            UmdCheckMultisampleQualityLevels;
        funcs->pfnGetMipPacking = UmdGetMipPacking;
        funcs->pfnGetImplicitPhysicalAdapterMask =
            UmdGetImplicitPhysicalAdapterMask;
        funcs->pfnQueryNodeMap = UmdQueryNodeMap;
        funcs->pfnCheckResourceAllocationInfo =
            UmdCheckResourceAllocationInfo;
        funcs->pfnCheckSubresourceInfo = UmdCheckSubresourceInfo;
        funcs->pfnCheckExistingResourceAllocationInfo =
            UmdCheckExistingResourceAllocationInfo;
        funcs->pfnCalcPrivateShaderSize = UmdCalcPrivateShaderSize;
        funcs->pfnCreateVertexShader = UmdCreateShader;
        funcs->pfnCreatePixelShader = UmdCreateShader;
        funcs->pfnCreateGeometryShader = UmdCreateShader;
        funcs->pfnCreateComputeShader = UmdCreateShader;
        funcs->pfnDestroyShader = UmdDestroyShader;
        funcs->pfnCalcPrivateElementLayoutSize =
            UmdCalcPrivateElementLayoutSize;
        funcs->pfnCreateElementLayout = UmdCreateElementLayout;
        funcs->pfnDestroyElementLayout = UmdDestroyElementLayout;
        funcs->pfnCalcPrivateRootSignatureSize =
            UmdCalcPrivateRootSignatureSize;
        funcs->pfnCreateRootSignature = UmdCreateRootSignature;
        funcs->pfnDestroyRootSignature = UmdDestroyRootSignature;
        funcs->pfnCalcPrivatePipelineStateSize =
            UmdCalcPrivatePipelineStateSize;
        funcs->pfnCreatePipelineState = UmdCreatePipelineState;
        funcs->pfnDestroyPipelineState = UmdDestroyPipelineState;
        funcs->pfnCalcPrivateCommandQueueSize =
            UmdCalcPrivateCommandQueueSize;
        funcs->pfnCreateCommandQueue = UmdCreateCommandQueue;
        funcs->pfnDestroyCommandQueue = UmdDestroyCommandQueue;
        funcs->pfnCalcPrivateCommandPoolSize = UmdCalcPrivateCommandPoolSize;
        funcs->pfnCreateCommandPool = UmdCreateCommandPool;
        funcs->pfnDestroyCommandPool = UmdDestroyCommandPool;
        funcs->pfnResetCommandPool = UmdResetCommandPool;
        funcs->pfnCalcPrivateCommandListSize = UmdCalcPrivateCommandListSize;
        funcs->pfnCreateCommandList = UmdCreateCommandList;
        funcs->pfnDestroyCommandList = UmdDestroyCommandList;
        funcs->pfnCalcPrivateFenceSize = UmdCalcPrivateFenceSize;
        funcs->pfnCreateFence = UmdCreateFence;
        funcs->pfnDestroyFence = UmdDestroyFence;
        funcs->pfnCalcPrivateHeapAndResourceSizes =
            UmdCalcPrivateHeapAndResourceSizes;
        funcs->pfnCreateHeapAndResource = UmdCreateHeapAndResource;
        funcs->pfnDestroyHeapAndResource = UmdDestroyHeapAndResource;
        funcs->pfnMakeResident = UmdMakeResident;
        funcs->pfnEvict = UmdEvict;
        funcs->pfnCalcPrivateOpenedHeapAndResourceSizes =
            UmdCalcPrivateOpenedHeapAndResourceSizes;
        funcs->pfnOpenHeapAndResource = UmdOpenHeapAndResource;
        funcs->pfnMapHeap = UmdMapHeap;
        funcs->pfnUnmapHeap = UmdUnmapHeap;
        funcs->pfnCheckResourceVirtualAddress =
            UmdCheckResourceVirtualAddress;
        funcs->pfnCheckResourceAllocationHandle =
            UmdCheckResourceAllocationHandle;
        funcs->pfnGetPresentPrivateDriverDataSize =
            UmdGetPresentPrivateDriverDataSize;
        funcs->pfnGetDescriptorSizeInBytes = UmdGetDescriptorSizeInBytes;
        funcs->pfnCalcPrivateDescriptorHeapSize =
            UmdCalcPrivateDescriptorHeapSize;
        funcs->pfnCreateDescriptorHeap = UmdCreateDescriptorHeap;
        funcs->pfnDestroyDescriptorHeap = UmdDestroyDescriptorHeap;
        funcs->pfnGetCPUDescriptorHandleForHeapStart =
            UmdGetCPUDescriptorHandleForHeapStart;
        funcs->pfnGetGPUDescriptorHandleForHeapStart =
            UmdGetGPUDescriptorHandleForHeapStart;
        funcs->pfnCreateUnorderedAccessView =
            UmdCreateUnorderedAccessView;
        funcs->pfnCreateShaderResourceView =
            UmdCreateShaderResourceView;
        funcs->pfnCreateConstantBufferView =
            UmdCreateConstantBufferView;
        funcs->pfnCreateSampler =
            UmdCreateSampler;
        funcs->pfnCopyDescriptors =
            UmdCopyDescriptors;
        funcs->pfnCopyDescriptorsSimple =
            UmdCopyDescriptorsSimple;
        funcs->pfnCreateRenderTargetView =
            UmdCreateRenderTargetView;
        funcs->pfnCreateDepthStencilView =
            UmdCreateDepthStencilView;
        umd_fill_null_slots(funcs, size);
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D: {
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 *funcs;
        UMD_ADAPTER *ad = (UMD_ADAPTER *)hAdapter.pDrvPrivate;
        if (size < sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092))
            return E_INVALIDARG;
        if (ad && table_version == 0 && rt_table.handle)
            ad->cmdlist_rt_table = rt_table;
        funcs = (D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 *)table;
        memset(funcs, 0, size);
        funcs->pfnCloseCommandList = UmdCloseCommandList;
        funcs->pfnResetCommandList = UmdResetCommandList;
        funcs->pfnDispatch = UmdDispatch;
        funcs->pfnDrawInstanced = UmdDrawInstanced;
        funcs->pfnDrawIndexedInstanced = UmdDrawIndexedInstanced;
        funcs->pfnSetPipelineState = UmdSetPipelineState;
        funcs->pfnOMSetRenderTargets = UmdOMSetRenderTargets;
        funcs->pfnRsSetViewports = UmdRSSetViewports;
        funcs->pfnRsSetScissorRects = UmdRSSetScissorRects;
        funcs->pfnIaSetTopology = UmdIASetPrimitiveTopology;
        funcs->pfnIASetVertexBuffers = UmdIASetVertexBuffers;
        funcs->pfnIASetIndexBuffer = UmdIASetIndexBuffer;
        funcs->pfnSetGraphicsRootSignature = UmdSetGraphicsRootSignature;
        funcs->pfnSetGraphicsRootDescriptorTable = UmdSetGraphicsRootDescriptorTable;
        funcs->pfnSetGraphicsRoot32BitConstant = UmdSetGraphicsRoot32BitConstant;
        funcs->pfnSetGraphicsRoot32BitConstants = UmdSetGraphicsRoot32BitConstants;
        funcs->pfnSetGraphicsRootConstantBufferView = UmdSetGraphicsRootConstantBufferView;
        funcs->pfnSetGraphicsRootShaderResourceView = UmdSetGraphicsRootShaderResourceView;
        funcs->pfnSetComputeRootSignature = UmdSetComputeRootSignature;
        funcs->pfnSetDescriptorHeaps = UmdSetDescriptorHeaps;
        funcs->pfnSetComputeRootDescriptorTable =
            UmdSetComputeRootDescriptorTable;
        funcs->pfnSetComputeRoot32BitConstant =
            UmdSetComputeRoot32BitConstant;
        funcs->pfnSetComputeRoot32BitConstants =
            UmdSetComputeRoot32BitConstants;
        funcs->pfnSetComputeRootUnorderedAccessView =
            UmdSetComputeRootUnorderedAccessView;
        funcs->pfnClearUnorderedAccessViewUint =
            UmdClearUnorderedAccessViewUint;
        funcs->pfnClearRenderTargetView = UmdClearRenderTargetView;
        funcs->pfnClearDepthStencilView = UmdClearDepthStencilView;
        funcs->pfnCopyTextureRegion = UmdCopyTextureRegion;
        funcs->pfnResourceCopy = UmdResourceCopy;
        funcs->pfnCopyBufferRegion = UmdCopyBufferRegion;
        funcs->pfnAtomicCopyBufferRegion = UmdAtomicCopyBufferRegion;
        funcs->pfnResourceBarrier = UmdResourceBarrier;
        funcs->pfnWriteBufferImmediate = UmdWriteBufferImmediate;
        funcs->pfnDiscardResource = UmdDiscardResource;
        funcs->pfnBlt = UmdBlt;
        funcs->pfnPresent = UmdPresent;
        umd_fill_null_slots(funcs, size);
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D: {
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *funcs;
        if (size < sizeof(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001))
            return E_INVALIDARG;
        funcs = (D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *)table;
        memset(funcs, 0, size);
        funcs->pfnExecuteCommandLists = UmdExecuteCommandLists;
        funcs->pfnSignalFence = UmdSignalFence;
        funcs->pfnWaitForFence = UmdWaitForFence;
        umd_fill_null_slots(funcs, size);
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_DXGI: {
        memset(table, 0, size);
        if (size >= sizeof(DXGI_DDI_BASE_FUNCTIONS)) {
            DXGI_DDI_BASE_FUNCTIONS *dx = (DXGI_DDI_BASE_FUNCTIONS *)table;
            dx->pfnPresent = UmdDxgiPresent;
            dx->pfnGetGammaCaps = UmdDxgiGetGammaCaps;
            dx->pfnSetDisplayMode = (HRESULT (APIENTRY *)(DXGI_DDI_ARG_SETDISPLAYMODE *))UmdDxgiOk1;
            dx->pfnSetResourcePriority = (HRESULT (APIENTRY *)(DXGI_DDI_ARG_SETRESOURCEPRIORITY *))UmdDxgiOk1;
            dx->pfnQueryResourceResidency = UmdDxgiQueryResourceResidency;
            dx->pfnRotateResourceIdentities = (HRESULT (APIENTRY *)(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *))UmdDxgiOk1;
            dx->pfnBlt = (HRESULT (APIENTRY *)(DXGI_DDI_ARG_BLT *))UmdDxgiOk1;
        }
        if (size >= sizeof(DXGI1_6_1_DDI_BASE_FUNCTIONS)) {
            DXGI1_6_1_DDI_BASE_FUNCTIONS *dx6 = (DXGI1_6_1_DDI_BASE_FUNCTIONS *)table;
            dx6->pfnPresent1 = UmdDxgiPresent1;
            dx6->pfnCheckMultiplaneOverlayColorSpaceSupport = UmdDxgiCheckMpoColorSpace;
        }
        umd_fill_null_slots(table, size);
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES: {
        D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *funcs;
        if (size < sizeof(*funcs))
            return E_INVALIDARG;
        funcs = (D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *)table;
        memset(funcs, 0, size);
        funcs->pfnGetSupportedExtendedFeatures = UmdGetSupportedExtendedFeatures;
        funcs->pfnGetSupportedExtendedFeatureVersions = UmdGetSupportedExtendedFeatureVersions;
        funcs->pfnEnableExtendedFeature = UmdEnableExtendedFeature;
        funcs->pfnSetExtendedFeatureCallbacks = UmdSetExtendedFeatureCallbacks;
        return S_OK;
    }
    default:
        umd_trace("  -> FillDDITable UNHANDLED type=%u size=%llu -> E_NOTIMPL", (unsigned)type, (unsigned long long)size);
        return E_NOTIMPL;
    }
}

/* ---- Entry point -------------------------------------------------------- */

__declspec(dllexport) HRESULT WINAPI
OpenAdapter12(D3D12DDIARG_OPENADAPTER *arg)
{
    UMD_ADAPTER *adapter;
    D3D12DDI_ADAPTERFUNCS *funcs;

    umd_trace("OpenAdapter12(rt=%p cb=%p funcs=%p)", arg ? arg->hRTAdapter.handle : NULL, arg ? (void*)arg->pAdapterCallbacks : NULL, arg ? (void*)arg->pAdapterFuncs : NULL);
    if (!arg || !arg->pAdapterFuncs)
        return E_INVALIDARG;
    adapter = (UMD_ADAPTER *)calloc(1, sizeof(*adapter));
    if (!adapter)
        return E_OUTOFMEMORY;
    adapter->rt_adapter = arg->hRTAdapter;
    adapter->callbacks = arg->pAdapterCallbacks;
    arg->hAdapter.pDrvPrivate = adapter;

    funcs = arg->pAdapterFuncs;
    memset(funcs, 0, sizeof(*funcs));
    funcs->pfnCalcPrivateDeviceSize = UmdCalcPrivateDeviceSize;
    funcs->pfnCreateDevice = UmdCreateDevice;
    funcs->pfnCloseAdapter = UmdCloseAdapter;
    funcs->pfnGetSupportedVersions = UmdGetSupportedVersions;
    funcs->pfnGetCaps = UmdGetCaps;
    funcs->pfnGetOptionalDDITables = UmdGetOptionalDDITables;
    funcs->pfnFillDDITable = UmdFillDDITable;
    funcs->pfnDestroyDevice = UmdDestroyDevice;
    return S_OK;
}

/* ---- Test/integration exports ------------------------------------------ */

/*
 * Inject the portable KMD binding. The D3D12 runtime path uses this via the
 * WDDM KMD callbacks; the standalone harness passes the simulated KMD, and
 * the bring-up tool can pass adreno_win_kmd_ops() over the real IOCTLs.
 */
__declspec(dllexport) int AdrenoUmdSetAdapterKmd(
    D3D12DDI_HADAPTER hAdapter, const adreno_kmd_ops *ops)
{
    UMD_ADAPTER *adapter;

    if (!hAdapter.pDrvPrivate || !ops || !ops->alloc || !ops->free ||
        !ops->submit || !ops->wait)
        return 1;
    adapter = (UMD_ADAPTER *)hAdapter.pDrvPrivate;
    adapter->ops = *ops;
    adapter->has_ops = TRUE;
    return 0;
}

__declspec(dllexport) UINT64 AdrenoUmdGetFenceValue(
    D3D12DDI_HFENCE hFence)
{
    UMD_FENCE *fence = (UMD_FENCE *)hFence.pDrvPrivate;
    return fence ? fence->value : 0;
}

__declspec(dllexport) int AdrenoUmdReadResource(
    D3D12DDI_HRESOURCE hResource, UINT64 offset, void *dst, UINT32 size)
{
    UMD_RESOURCE *res = umd_resource(hResource);

    if (!res || !dst || offset + size > res->size)
        return 1;
    if (res->bo_offset + offset > 0xffffffffull)
        return 1;
    if (adreno_d3d12_resource_read(&res->bo,
                                   (uint32_t)(res->bo_offset + offset), dst,
                                   size) != ADRENO_OK)
        return 1;
    return 0;
}

__declspec(dllexport) UINT32 AdrenoUmdCommandDwords(
    D3D12DDI_HCOMMANDLIST hCommandList)
{
    UMD_CMDLIST *list = (UMD_CMDLIST *)hCommandList.pDrvPrivate;
    return list && list->initialized ? list->cmd.pm4.count : 0;
}

__declspec(dllexport) UINT32 AdrenoUmdShaderBytes(D3D12DDI_HSHADER hShader)
{
    UMD_SHADER *shader = (UMD_SHADER *)hShader.pDrvPrivate;
    return shader ? shader->bytes : 0;
}

__declspec(dllexport) int AdrenoUmdCreateDeviceWithKmd(
    const adreno_kmd_ops *ops, void **out_device)
{
    adreno_d3d12_device *dev;
    if (!ops || !out_device)
        return 1;
    dev = (adreno_d3d12_device *)calloc(1, sizeof(*dev));
    if (!dev)
        return 2;
    if (adreno_d3d12_device_init(dev, ops) != ADRENO_OK) {
        free(dev);
        return 3;
    }
    *out_device = dev;
    return 0;
}

__declspec(dllexport) void AdrenoUmdDestroyDevice(void *device)
{
    if (!device)
        return;
    adreno_d3d12_device_finish((adreno_d3d12_device *)device);
    free(device);
}

__declspec(dllexport) int AdrenoUmdClearSelfTest(
    const adreno_kmd_ops *ops, uint32_t color, uint32_t dwords,
    uint32_t timeout_ms, uint32_t *mismatches, uint32_t *first_value)
{
    adreno_d3d12_device dev;
    adreno_d3d12_resource res;
    adreno_d3d12_cmdlist cmd;
    uint32_t i;
    int rc = 1;

    if (!ops || dwords == 0)
        return 1;
    if (mismatches)
        *mismatches = 0;
    if (first_value)
        *first_value = 0;
    if (adreno_d3d12_device_init(&dev, ops) != ADRENO_OK)
        return 2;
    if (adreno_d3d12_resource_create(&dev, dwords * 4, &res) != ADRENO_OK)
        return 3;
    if (adreno_d3d12_cmdlist_create(&dev, 1024, &cmd) != ADRENO_OK) {
        adreno_d3d12_resource_destroy(&res);
        return 4;
    }
    if (adreno_d3d12_cmd_clear_u32(&cmd, &res, 0, dwords, color) !=
            ADRENO_OK ||
        adreno_d3d12_submit(&dev, &cmd, 1) != ADRENO_OK ||
        adreno_d3d12_wait(&dev, 1, timeout_ms ? timeout_ms : 3000) !=
            ADRENO_OK) {
        rc = 5;
        goto out;
    }
    if (!res.host) {
        rc = 6;
        goto out;
    }
    for (i = 0; i < dwords; i++) {
        uint32_t v = ((const uint32_t *)res.host)[i];
        if (i == 0 && first_value)
            *first_value = v;
        if (v != color && mismatches)
            (*mismatches)++;
    }
    rc = (mismatches && *mismatches) ? 7 : 0;

out:
    adreno_d3d12_cmdlist_destroy(&cmd);
    adreno_d3d12_resource_destroy(&res);
    adreno_d3d12_device_finish(&dev);
    return rc;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)module;
    (void)reason;
    (void)reserved;
    return TRUE;
}

