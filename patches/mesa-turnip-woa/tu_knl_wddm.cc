/*
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 * SPDX-License-Identifier: MIT
 *
 * Turnip WDDM 2.0 kernel backend (adreno730_wddm.sys via D3DKMT + WddmEscape).
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef LONG NTSTATUS;
#include <d3dkmthk.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#ifdef interface
#undef interface
#endif

#include <atomic>
#include <errno.h>
#include <string.h>

#include "c11/threads.h"
#include "util/os_time.h"
#include "util/u_dynarray.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include "vk_util.h"

#include "adreno_wddm_escape.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_knl.h"
#include "tu_queue.h"
#include "tu_rmv.h"
#include "tu_tracepoints.h"

#define ADRENO_WDDM_ALLOC_PRIVATE_MAGIC 0x57444D41u /* "WDMA" */
struct adreno_wddm_alloc_private {
   uint32_t magic;
   uint32_t size;
};

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS)0x00000103L)
#endif

/* ---- Dynamic D3DKMT / DXGI loader -------------------------------------- */

struct wddm_thunks {
   bool initialized;
   bool ok;
   PFND3DKMT_OPENADAPTERFROMLUID open_adapter_from_luid;
   PFND3DKMT_CLOSEADAPTER close_adapter;
   PFND3DKMT_CREATEDEVICE create_device;
   PFND3DKMT_DESTROYDEVICE destroy_device;
   PFND3DKMT_CREATEPAGINGQUEUE create_paging_queue;
   PFND3DKMT_DESTROYPAGINGQUEUE destroy_paging_queue;
   PFND3DKMT_CREATEALLOCATION2 create_allocation2;
   PFND3DKMT_DESTROYALLOCATION2 destroy_allocation2;
   PFND3DKMT_MAKERESIDENT make_resident;
   PFND3DKMT_EVICT evict;
   PFND3DKMT_MAPGPUVIRTUALADDRESS map_gpu_va;
   PFND3DKMT_FREEGPUVIRTUALADDRESS free_gpu_va;
   PFND3DKMT_LOCK2 lock2;
   PFND3DKMT_UNLOCK2 unlock2;
   PFND3DKMT_ENUMADAPTERS2 enum_adapters2;
   PFND3DKMT_ESCAPE escape;
};

static struct wddm_thunks g_wddm;

static bool
wddm_load_thunks(void)
{
   if (g_wddm.initialized)
      return g_wddm.ok;
   g_wddm.initialized = true;

   HMODULE gdi = LoadLibraryA("gdi32.dll");
   if (!gdi)
      return false;

#define LOAD_GDI(field, sym, type)                                    \
   do {                                                               \
      g_wddm.field = (type)GetProcAddress(gdi, sym);                  \
      if (!g_wddm.field) {                                            \
         mesa_loge("tu_wddm: missing gdi32!%s", sym);                 \
         return false;                                                \
      }                                                               \
   } while (0)

   LOAD_GDI(enum_adapters2, "D3DKMTEnumAdapters2", PFND3DKMT_ENUMADAPTERS2);
   LOAD_GDI(open_adapter_from_luid, "D3DKMTOpenAdapterFromLuid", PFND3DKMT_OPENADAPTERFROMLUID);
   LOAD_GDI(close_adapter, "D3DKMTCloseAdapter", PFND3DKMT_CLOSEADAPTER);
   LOAD_GDI(create_device, "D3DKMTCreateDevice", PFND3DKMT_CREATEDEVICE);
   LOAD_GDI(destroy_device, "D3DKMTDestroyDevice", PFND3DKMT_DESTROYDEVICE);
   LOAD_GDI(create_paging_queue, "D3DKMTCreatePagingQueue", PFND3DKMT_CREATEPAGINGQUEUE);
   LOAD_GDI(destroy_paging_queue, "D3DKMTDestroyPagingQueue", PFND3DKMT_DESTROYPAGINGQUEUE);
   LOAD_GDI(create_allocation2, "D3DKMTCreateAllocation2", PFND3DKMT_CREATEALLOCATION2);
   LOAD_GDI(destroy_allocation2, "D3DKMTDestroyAllocation2", PFND3DKMT_DESTROYALLOCATION2);
   LOAD_GDI(make_resident, "D3DKMTMakeResident", PFND3DKMT_MAKERESIDENT);
   LOAD_GDI(evict, "D3DKMTEvict", PFND3DKMT_EVICT);
   LOAD_GDI(map_gpu_va, "D3DKMTMapGpuVirtualAddress", PFND3DKMT_MAPGPUVIRTUALADDRESS);
   LOAD_GDI(free_gpu_va, "D3DKMTFreeGpuVirtualAddress", PFND3DKMT_FREEGPUVIRTUALADDRESS);
   LOAD_GDI(lock2, "D3DKMTLock2", PFND3DKMT_LOCK2);
   LOAD_GDI(unlock2, "D3DKMTUnlock2", PFND3DKMT_UNLOCK2);
   LOAD_GDI(escape, "D3DKMTEscape", PFND3DKMT_ESCAPE);
#undef LOAD_GDI

   g_wddm.ok = true;
   return true;
}

static NTSTATUS
wddm_escape_call(D3DKMT_HANDLE hAdapter, D3DKMT_HANDLE hDevice, void *buf, UINT size)
{
   D3DKMT_ESCAPE esc = {};
   esc.hAdapter = hAdapter;
   esc.hDevice = hDevice;
   esc.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
   esc.pPrivateDriverData = buf;
   esc.PrivateDriverDataSize = size;
   return g_wddm.escape(&esc);
}

static void
wddm_wait_paging_fence(struct tu_device *dev, uint64_t target)
{
   if (!dev->wddm_paging_fence_cpu || target == 0)
      return;
   uint32_t spins = 0;
   while (*dev->wddm_paging_fence_cpu < target) {
      if (++spins > 100000u)
         Sleep(0);
      if (spins > 20000000u)
         break;
   }
}

/* ---- Thread-safe binary vk_sync implementation ------------------------- */

struct vk_wddm_sync {
   struct vk_sync base;
   mtx_t mtx;
   cnd_t cnd;
   bool signaled;
};

static VkResult
wddm_sync_init(struct vk_device *device, struct vk_sync *sync, uint64_t initial_value)
{
   struct vk_wddm_sync *s = container_of(sync, struct vk_wddm_sync, base);
   mtx_init(&s->mtx, mtx_plain);
   cnd_init(&s->cnd);
   s->signaled = initial_value != 0;
   return VK_SUCCESS;
}

static void
wddm_sync_finish(struct vk_device *device, struct vk_sync *sync)
{
   struct vk_wddm_sync *s = container_of(sync, struct vk_wddm_sync, base);
   cnd_destroy(&s->cnd);
   mtx_destroy(&s->mtx);
}

static VkResult
wddm_sync_signal(struct vk_device *device, struct vk_sync *sync, uint64_t value)
{
   struct vk_wddm_sync *s = container_of(sync, struct vk_wddm_sync, base);
   mtx_lock(&s->mtx);
   s->signaled = true;
   cnd_broadcast(&s->cnd);
   mtx_unlock(&s->mtx);
   return VK_SUCCESS;
}

static VkResult
wddm_sync_reset(struct vk_device *device, struct vk_sync *sync)
{
   struct vk_wddm_sync *s = container_of(sync, struct vk_wddm_sync, base);
   mtx_lock(&s->mtx);
   s->signaled = false;
   mtx_unlock(&s->mtx);
   return VK_SUCCESS;
}

static VkResult
wddm_sync_move(struct vk_device *device, struct vk_sync *dst, struct vk_sync *src)
{
   struct vk_wddm_sync *d = container_of(dst, struct vk_wddm_sync, base);
   struct vk_wddm_sync *s = container_of(src, struct vk_wddm_sync, base);
   bool val;
   mtx_lock(&s->mtx);
   val = s->signaled;
   s->signaled = false;
   mtx_unlock(&s->mtx);

   mtx_lock(&d->mtx);
   d->signaled = val;
   if (val)
      cnd_broadcast(&d->cnd);
   mtx_unlock(&d->mtx);
   return VK_SUCCESS;
}

static VkResult
wddm_sync_wait_one(struct vk_wddm_sync *s, uint64_t abs_timeout_ns)
{
   mtx_lock(&s->mtx);
   while (!s->signaled) {
      uint64_t now = os_time_get_nano();
      if (now >= abs_timeout_ns) {
         mtx_unlock(&s->mtx);
         return VK_TIMEOUT;
      }
      uint64_t rem_ns = abs_timeout_ns - now;
      if (rem_ns > 10000000ull)
         rem_ns = 10000000ull; /* 10ms slice */
      struct timespec ts;
      timespec_get(&ts, TIME_UTC);
      uint64_t nsec = (uint64_t)ts.tv_nsec + rem_ns;
      ts.tv_sec += (time_t)(nsec / 1000000000ull);
      ts.tv_nsec = (long)(nsec % 1000000000ull);
      cnd_timedwait(&s->cnd, &s->mtx, &ts);
   }
   mtx_unlock(&s->mtx);
   return VK_SUCCESS;
}

static VkResult
wddm_sync_wait_many(struct vk_device *device,
                    uint32_t wait_count,
                    const struct vk_sync_wait *waits,
                    enum vk_sync_wait_flags wait_flags,
                    uint64_t abs_timeout_ns)
{
   if (wait_count == 0)
      return VK_SUCCESS;

   if (wait_flags & VK_SYNC_WAIT_ANY) {
      for (;;) {
         for (uint32_t i = 0; i < wait_count; i++) {
            struct vk_wddm_sync *s = container_of(waits[i].sync, struct vk_wddm_sync, base);
            mtx_lock(&s->mtx);
            bool done = s->signaled;
            mtx_unlock(&s->mtx);
            if (done)
               return VK_SUCCESS;
         }
         if (os_time_get_nano() >= abs_timeout_ns)
            return VK_TIMEOUT;
         Sleep(1);
      }
   }

   for (uint32_t i = 0; i < wait_count; i++) {
      struct vk_wddm_sync *s = container_of(waits[i].sync, struct vk_wddm_sync, base);
      VkResult res = wddm_sync_wait_one(s, abs_timeout_ns);
      if (res != VK_SUCCESS)
         return res;
   }
   return VK_SUCCESS;
}

static const struct vk_sync_type vk_wddm_sync_type = {
   .size = sizeof(struct vk_wddm_sync),
   .features = (enum vk_sync_features)(
      VK_SYNC_FEATURE_BINARY |
      VK_SYNC_FEATURE_GPU_WAIT |
      VK_SYNC_FEATURE_GPU_MULTI_WAIT |
      VK_SYNC_FEATURE_CPU_WAIT |
      VK_SYNC_FEATURE_CPU_RESET |
      VK_SYNC_FEATURE_CPU_SIGNAL |
      VK_SYNC_FEATURE_WAIT_ANY |
      VK_SYNC_FEATURE_WAIT_PENDING),
   .init = wddm_sync_init,
   .finish = wddm_sync_finish,
   .signal = wddm_sync_signal,
   .reset = wddm_sync_reset,
   .move = wddm_sync_move,
   .wait_many = wddm_sync_wait_many,
};

/* ---- Device lifecycle -------------------------------------------------- */

static VkResult
wddm_device_init(struct tu_device *dev)
{
   D3DKMT_OPENADAPTERFROMLUID open_luid = {};
   open_luid.AdapterLuid.LowPart = (DWORD)(dev->physical_device->wddm_luid & 0xffffffffull);
   open_luid.AdapterLuid.HighPart = (LONG)(dev->physical_device->wddm_luid >> 32);

   NTSTATUS st = g_wddm.open_adapter_from_luid(&open_luid);
   if (st != STATUS_SUCCESS || !open_luid.hAdapter) {
      return vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                       "D3DKMTOpenAdapterFromLuid failed: 0x%08x", (unsigned)st);
   }
   dev->wddm_adapter = open_luid.hAdapter;

   D3DKMT_CREATEDEVICE cdev = {};
   cdev.hAdapter = dev->wddm_adapter;
   st = g_wddm.create_device(&cdev);
   if (st != STATUS_SUCCESS || !cdev.hDevice) {
      D3DKMT_CLOSEADAPTER ca = { dev->wddm_adapter };
      g_wddm.close_adapter(&ca);
      dev->wddm_adapter = 0;
      return vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                       "D3DKMTCreateDevice failed: 0x%08x", (unsigned)st);
   }
   dev->wddm_device = cdev.hDevice;

   D3DKMT_CREATEPAGINGQUEUE cpq = {};
   cpq.hDevice = dev->wddm_device;
   cpq.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
   st = g_wddm.create_paging_queue(&cpq);
   if (st != STATUS_SUCCESS || !cpq.hPagingQueue) {
      D3DKMT_DESTROYDEVICE dd = { dev->wddm_device };
      g_wddm.destroy_device(&dd);
      D3DKMT_CLOSEADAPTER ca = { dev->wddm_adapter };
      g_wddm.close_adapter(&ca);
      dev->wddm_device = 0;
      dev->wddm_adapter = 0;
      return vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                       "D3DKMTCreatePagingQueue failed: 0x%08x", (unsigned)st);
   }
   dev->wddm_paging_queue = cpq.hPagingQueue;
   dev->wddm_paging_sync = cpq.hSyncObject;
   dev->wddm_paging_fence_cpu = (volatile uint64_t *)cpq.FenceValueCPUVirtualAddress;
   dev->wddm_next_handle = 1;

   return VK_SUCCESS;
}

static void
wddm_device_finish(struct tu_device *dev)
{
   if (dev->wddm_paging_queue) {
      D3DDDI_DESTROYPAGINGQUEUE dpq = { dev->wddm_paging_queue };
      g_wddm.destroy_paging_queue(&dpq);
      dev->wddm_paging_queue = 0;
      dev->wddm_paging_sync = 0;
      dev->wddm_paging_fence_cpu = NULL;
   }
   if (dev->wddm_device) {
      D3DKMT_DESTROYDEVICE dd = { dev->wddm_device };
      g_wddm.destroy_device(&dd);
      dev->wddm_device = 0;
   }
   if (dev->wddm_adapter) {
      D3DKMT_CLOSEADAPTER ca = { dev->wddm_adapter };
      g_wddm.close_adapter(&ca);
      dev->wddm_adapter = 0;
   }
}

static int
wddm_device_get_gpu_timestamp(struct tu_device *dev, uint64_t *ts)
{
   adreno_wddm_esc_timestamp req = {};
   req.hdr.magic = ADRENO_WDDM_ESC_MAGIC;
   req.hdr.version = ADRENO_WDDM_ESC_VERSION;
   req.hdr.cmd = ADRENO_WDDM_ESC_GET_TIMESTAMP;

   NTSTATUS st = wddm_escape_call(dev->wddm_adapter, dev->wddm_device, &req, sizeof(req));
   if (st == STATUS_SUCCESS && req.hdr.status == 0 &&
       req.ticks != 0xdeadbeefdeadbeefull) {
      *ts = req.ticks;
      return 0;
   }
   /* Fallback: QPC scaled to 19.2 MHz XO clock */
   LARGE_INTEGER cnt, freq;
   QueryPerformanceCounter(&cnt);
   QueryPerformanceFrequency(&freq);
   *ts = (uint64_t)((double)cnt.QuadPart * (19200000.0 / (double)freq.QuadPart));
   return 0;
}

static int
wddm_device_get_suspend_count(struct tu_device *dev, uint64_t *suspend_count)
{
   *suspend_count = 0;
   return 0;
}

static VkResult
wddm_device_check_status(struct tu_device *dev)
{
   return VK_SUCCESS;
}

static int
wddm_submitqueue_new(struct tu_device *dev, struct tu_queue *queue)
{
   queue->msm_queue_id = 1;
   return 0;
}

static void
wddm_submitqueue_close(struct tu_device *dev, struct tu_queue *queue)
{
   (void)dev;
   (void)queue;
}

/* ---- BO management via WDDM 2.0 GpuMmu --------------------------------- */

static VkResult
wddm_bo_init(struct tu_device *dev,
             struct vk_object_base *base,
             struct tu_bo **out_bo,
             uint64_t size,
             uint64_t client_iova,
             VkMemoryPropertyFlags mem_property,
             enum tu_bo_alloc_flags flags,
             struct tu_sparse_vma *lazy_vma,
             const char *name)
{
   (void)mem_property;
   (void)flags;
   (void)lazy_vma;

   if (size == 0)
      size = 4096;
   size = align64(size, 4096);
   if (size > 0xffffffffull)
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY, "BO size > 4GiB");

   struct adreno_wddm_alloc_private priv = {};
   priv.magic = ADRENO_WDDM_ALLOC_PRIVATE_MAGIC;
   priv.size = (uint32_t)size;

   D3DDDI_ALLOCATIONINFO2 ainfo = {};
   ainfo.pPrivateDriverData = &priv;
   ainfo.PrivateDriverDataSize = sizeof(priv);

   D3DKMT_CREATEALLOCATION ca = {};
   ca.hDevice = dev->wddm_device;
   ca.NumAllocations = 1;
   ca.pAllocationInfo2 = &ainfo;

   NTSTATUS st = g_wddm.create_allocation2(&ca);
   if (st != STATUS_SUCCESS || !ainfo.hAllocation) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "D3DKMTCreateAllocation2(%llu) failed: 0x%08x",
                       (unsigned long long)size, (unsigned)st);
   }
   D3DKMT_HANDLE hAlloc = ainfo.hAllocation;

   D3DDDI_MAKERESIDENT mr = {};
   mr.hPagingQueue = dev->wddm_paging_queue;
   mr.NumAllocations = 1;
   mr.AllocationList = &hAlloc;
   st = g_wddm.make_resident(&mr);
   if (st != STATUS_SUCCESS && st != STATUS_PENDING) {
      D3DKMT_DESTROYALLOCATION2 da = {};
      da.hDevice = dev->wddm_device;
      da.AllocationCount = 1;
      da.phAllocationList = &hAlloc;
      g_wddm.destroy_allocation2(&da);
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "D3DKMTMakeResident failed: 0x%08x", (unsigned)st);
   }
   if (st == STATUS_PENDING)
      wddm_wait_paging_fence(dev, mr.PagingFenceValue);

   D3DDDI_MAPGPUVIRTUALADDRESS map = {};
   map.hPagingQueue = dev->wddm_paging_queue;
   map.hAllocation = hAlloc;
   map.BaseAddress = client_iova;
   map.MinimumAddress = dev->physical_device->va_start;
   map.MaximumAddress = dev->physical_device->va_start + dev->physical_device->va_size;
   map.SizeInPages = size >> 12;
   map.Protection.Write = 1;
   st = g_wddm.map_gpu_va(&map);
   if (st != STATUS_SUCCESS && st != STATUS_PENDING) {
      D3DKMT_EVICT ev = {};
      ev.hDevice = dev->wddm_device;
      ev.NumAllocations = 1;
      ev.AllocationList = &hAlloc;
      g_wddm.evict(&ev);
      D3DKMT_DESTROYALLOCATION2 da = {};
      da.hDevice = dev->wddm_device;
      da.AllocationCount = 1;
      da.phAllocationList = &hAlloc;
      g_wddm.destroy_allocation2(&da);
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "D3DKMTMapGpuVirtualAddress(%llu) failed: 0x%08x",
                       (unsigned long long)size, (unsigned)st);
   }
   if (st == STATUS_PENDING)
      wddm_wait_paging_fence(dev, map.PagingFenceValue);

   uint32_t handle = p_atomic_fetch_add(&dev->wddm_next_handle, 1);
   struct tu_bo *bo = tu_device_lookup_bo(dev, handle);
   *bo = {};
   bo->gem_handle = handle;
   bo->wddm_alloc_handle = hAlloc;
   bo->size = size;
   bo->iova = map.VirtualAddress;
   bo->name = tu_debug_bos_add(dev, size, name);
   bo->refcnt = 1;
   bo->never_unmap = true;
   bo->base = base;
   bo->unique_id = handle;

   *out_bo = bo;
   return VK_SUCCESS;
}

static VkResult
wddm_bo_init_dmabuf(struct tu_device *dev, struct tu_bo **out_bo,
                    uint64_t size, enum tu_bo_alloc_flags flags, int prime_fd)
{
   (void)dev; (void)out_bo; (void)size; (void)flags; (void)prime_fd;
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static int
wddm_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   (void)dev; (void)bo;
   return -1;
}

static VkResult
wddm_bo_map(struct tu_device *dev, struct tu_bo *bo, void *placed_addr)
{
   (void)placed_addr;
   if (bo->map)
      return VK_SUCCESS;

   D3DKMT_LOCK2 lk = {};
   lk.hDevice = dev->wddm_device;
   lk.hAllocation = bo->wddm_alloc_handle;
   NTSTATUS st = g_wddm.lock2(&lk);
   if (st != STATUS_SUCCESS || !lk.pData) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "D3DKMTLock2 failed: 0x%08x", (unsigned)st);
   }
   bo->map = lk.pData;
   return VK_SUCCESS;
}

static void
wddm_bo_allow_dump(struct tu_device *dev, struct tu_bo *bo)
{
   (void)dev;
   (void)bo;
}

static void
wddm_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);
   if (!p_atomic_dec_zero(&bo->refcnt))
      return;

   TU_RMV(bo_destroy, dev, bo);
   tu_debug_bos_del(dev, bo);

   D3DKMT_HANDLE hAlloc = bo->wddm_alloc_handle;
   uint64_t iova = bo->iova;
   uint64_t size = bo->size;

   if (bo->map) {
      D3DKMT_UNLOCK2 ulk = {};
      ulk.hDevice = dev->wddm_device;
      ulk.hAllocation = hAlloc;
      g_wddm.unlock2(&ulk);
      bo->map = NULL;
   }
   if (iova) {
      D3DKMT_FREEGPUVIRTUALADDRESS fva = {};
      fva.hAdapter = dev->wddm_adapter;
      fva.BaseAddress = iova;
      fva.Size = size;
      g_wddm.free_gpu_va(&fva);
   }
   if (hAlloc) {
      D3DKMT_EVICT ev = {};
      ev.hDevice = dev->wddm_device;
      ev.NumAllocations = 1;
      ev.AllocationList = &hAlloc;
      g_wddm.evict(&ev);

      D3DKMT_DESTROYALLOCATION2 da = {};
      da.hDevice = dev->wddm_device;
      da.AllocationCount = 1;
      da.phAllocationList = &hAlloc;
      g_wddm.destroy_allocation2(&da);
   }

   memset(bo, 0, sizeof(*bo));
}

/* ---- Command submission via WddmEscape --------------------------------- */

struct tu_wddm_submit {
   struct util_dynarray ibs;
};

static void *
wddm_submit_create(struct tu_device *device)
{
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)
      vk_zalloc(&device->vk.alloc, sizeof(*submit), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!submit)
      return NULL;
   util_dynarray_init(&submit->ibs, NULL);
   return submit;
}

static void
wddm_submit_finish(struct tu_device *device, void *_submit)
{
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (!submit)
      return;
   util_dynarray_fini(&submit->ibs);
   vk_free(&device->vk.alloc, submit);
}

static void
wddm_submit_add_entries(struct tu_device *device, void *_submit,
                        struct tu_cs_entry *entries, unsigned num_entries)
{
   (void)device;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   for (unsigned i = 0; i < num_entries; i++) {
      if (entries[i].size == 0)
         continue;
      adreno_wddm_esc_ib ib = {};
      ib.iova = entries[i].bo->iova + entries[i].offset;
      ib.dwords = entries[i].size / 4u;
      ib.flags = 0;
      util_dynarray_append(&submit->ibs, ib);
   }
}

static void
wddm_submit_add_bind(struct tu_device *device, void *_submit,
                     struct tu_sparse_vma *vma, uint64_t vma_offset,
                     struct tu_bo *bo, uint64_t bo_offset, uint64_t size)
{
   (void)device; (void)_submit; (void)vma; (void)vma_offset;
   (void)bo; (void)bo_offset; (void)size;
}

static VkResult
wddm_queue_submit(struct tu_queue *queue, void *_submit,
                  struct vk_sync_wait *waits, uint32_t wait_count,
                  struct vk_sync_signal *signals, uint32_t signal_count,
                  struct tu_u_trace_submission_data *u_trace_submission_data)
{
   struct tu_device *dev = queue->device;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;

   if (wait_count > 0) {
      VkResult wres = vk_sync_wait_many(&dev->vk, wait_count, waits,
                                        VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (wres != VK_SUCCESS)
         return wres;
   }

   unsigned total_ibs =
      util_dynarray_num_elements(&submit->ibs, adreno_wddm_esc_ib);
   bool trace = getenv("TU_WDDM_TRACE") != NULL;
   if (trace) {
      fprintf(stderr, "tu_wddm: queue_submit total_ibs=%u waits=%u signals=%u\n",
              total_ibs, wait_count, signal_count);
      fflush(stderr);
   }
   adreno_wddm_esc_ib *all_ibs = (adreno_wddm_esc_ib *)submit->ibs.data;

   if (total_ibs > 0) {
      /* Ensure Write-Combined CPU writes to IB/resource BOs are visible to GPU */
      MemoryBarrier();

      unsigned offset = 0;
      while (offset < total_ibs) {
         unsigned chunk = total_ibs - offset;
         if (chunk > ADRENO_WDDM_ESC_MAX_IBS)
            chunk = ADRENO_WDDM_ESC_MAX_IBS;

         adreno_wddm_esc_submit esc = {};
         esc.hdr.magic = ADRENO_WDDM_ESC_MAGIC;
         esc.hdr.version = ADRENO_WDDM_ESC_VERSION;
         esc.hdr.cmd = ADRENO_WDDM_ESC_SUBMIT_IBS;
         esc.num_ibs = chunk;
         esc.timeout_ms = 5000;
         memcpy(esc.ibs, &all_ibs[offset], chunk * sizeof(adreno_wddm_esc_ib));

         if (trace) {
            for (unsigned k = 0; k < chunk && k < 8; k++)
               fprintf(stderr, "  ib[%u]: iova=0x%llx dw=%u\n", offset + k,
                       (unsigned long long)esc.ibs[k].iova, esc.ibs[k].dwords);
            fflush(stderr);
         }
         NTSTATUS st = wddm_escape_call(dev->wddm_adapter, dev->wddm_device,
                                        &esc, sizeof(esc));
         if (trace) {
            fprintf(stderr, "  escape -> st=0x%08x hdr_st=%u fence=%u done=%u\n",
                    (unsigned)st, esc.hdr.status, esc.out_fence, esc.completed);
            fflush(stderr);
         }
         if (st != STATUS_SUCCESS || esc.hdr.status != 0) {
            mesa_loge("tu_wddm: SUBMIT_IBS failed st=0x%08x esc_st=%u chunk=%u",
                      (unsigned)st, esc.hdr.status, chunk);
            return vk_device_set_lost(&dev->vk, "WDDM SUBMIT_IBS failed");
         }
         if (!esc.completed) {
            mesa_loge("tu_wddm: SUBMIT_IBS timed out (fence=%u)", esc.out_fence);
            return vk_device_set_lost(&dev->vk, "WDDM GPU timeout");
         }
         queue->fence = (int)esc.out_fence;
         offset += chunk;
      }
      MemoryBarrier();
   }

   for (uint32_t i = 0; i < signal_count; i++) {
      VkResult sres = vk_sync_signal(&dev->vk, signals[i].sync,
                                     signals[i].signal_value);
      if (sres != VK_SUCCESS)
         return sres;
   }

   if (u_trace_submission_data)
      tu_u_trace_submission_data_finish(dev, u_trace_submission_data);

   return VK_SUCCESS;
}

static VkResult
wddm_queue_wait_fence(struct tu_queue *queue, uint32_t fence, uint64_t timeout_ns)
{
   struct tu_device *dev = queue->device;
   adreno_wddm_esc_wait esc = {};
   esc.hdr.magic = ADRENO_WDDM_ESC_MAGIC;
   esc.hdr.version = ADRENO_WDDM_ESC_VERSION;
   esc.hdr.cmd = ADRENO_WDDM_ESC_WAIT_FENCE;
   esc.fence = fence;
   uint64_t ms = timeout_ns / 1000000ull;
   if (ms == 0 && timeout_ns > 0)
      ms = 1;
   if (ms > 10000ull)
      ms = 10000ull;
   esc.timeout_ms = (uint32_t)ms;

   NTSTATUS st = wddm_escape_call(dev->wddm_adapter, dev->wddm_device,
                                  &esc, sizeof(esc));
   if (st != STATUS_SUCCESS || esc.hdr.status != 0)
      return VK_ERROR_DEVICE_LOST;
   return esc.completed ? VK_SUCCESS : VK_TIMEOUT;
}

static VkResult
wddm_sparse_vma_init(struct tu_device *dev, struct vk_object_base *base,
                     struct tu_sparse_vma *out_vma, uint64_t *out_iova,
                     enum tu_sparse_vma_flags flags, uint64_t size,
                     uint64_t client_iova)
{
   (void)dev; (void)base; (void)out_vma; (void)out_iova;
   (void)flags; (void)size; (void)client_iova;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static void
wddm_sparse_vma_finish(struct tu_device *device, struct tu_sparse_vma *vma)
{
   (void)device; (void)vma;
}

static const struct tu_knl wddm_knl_funcs = {
   .name = "wddm",
   .device_init = wddm_device_init,
   .device_finish = wddm_device_finish,
   .device_get_gpu_timestamp = wddm_device_get_gpu_timestamp,
   .device_get_suspend_count = wddm_device_get_suspend_count,
   .device_check_status = wddm_device_check_status,
   .submitqueue_new = wddm_submitqueue_new,
   .submitqueue_close = wddm_submitqueue_close,
   .bo_init = wddm_bo_init,
   .bo_init_dmabuf = wddm_bo_init_dmabuf,
   .bo_export_dmabuf = wddm_bo_export_dmabuf,
   .bo_map = wddm_bo_map,
   .bo_allow_dump = wddm_bo_allow_dump,
   .bo_finish = wddm_bo_finish,
   .submit_create = wddm_submit_create,
   .submit_finish = wddm_submit_finish,
   .submit_add_entries = wddm_submit_add_entries,
   .submit_add_bind = wddm_submit_add_bind,
   .queue_submit = wddm_queue_submit,
   .queue_wait_fence = wddm_queue_wait_fence,
   .sparse_vma_init = wddm_sparse_vma_init,
   .sparse_vma_finish = wddm_sparse_vma_finish,
};

/* ---- Physical device discovery ----------------------------------------- */

VkResult
tu_knl_wddm_load(struct tu_instance *instance)
{
   if (!wddm_load_thunks())
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   bool found = false;
   LUID found_luid = {};
   adreno_wddm_esc_info info = {};

   /* Use D3DKMTEnumAdapters2 directly from gdi32 (never load dxgi.dll to avoid
    * recursive deadlock when DXVK's dxgi.dll is in the application directory). */
   D3DKMT_ENUMADAPTERS2 ea = {};
   if (g_wddm.enum_adapters2(&ea) != STATUS_SUCCESS || ea.NumAdapters == 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   D3DKMT_ADAPTERINFO *list = (D3DKMT_ADAPTERINFO *)
      calloc(ea.NumAdapters, sizeof(D3DKMT_ADAPTERINFO));
   if (!list)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   ea.pAdapters = list;
   if (g_wddm.enum_adapters2(&ea) == STATUS_SUCCESS) {
      for (ULONG idx = 0; idx < ea.NumAdapters; idx++) {
         D3DKMT_HANDLE hAdapter = list[idx].hAdapter;
         if (!hAdapter)
            continue;
         if (!found) {
            adreno_wddm_esc_info probe = {};
            probe.hdr.magic = ADRENO_WDDM_ESC_MAGIC;
            probe.hdr.version = ADRENO_WDDM_ESC_VERSION;
            probe.hdr.cmd = ADRENO_WDDM_ESC_GET_INFO;

            NTSTATUS st = wddm_escape_call(hAdapter, 0, &probe, sizeof(probe));
            if (st == STATUS_SUCCESS &&
                probe.hdr.magic == ADRENO_WDDM_ESC_MAGIC &&
                probe.hdr.status == 0 &&
                probe.hw_ready) {
               found = true;
               found_luid = list[idx].AdapterLuid;
               info = probe;
            }
         }
         D3DKMT_CLOSEADAPTER ca = { hAdapter };
         g_wddm.close_adapter(&ca);
      }
   }
   free(list);

   if (!found)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   struct tu_physical_device *device = (struct tu_physical_device *)
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   device->instance = instance;
   device->master_fd = -1;
   device->local_fd = -1;
   device->kgsl_dma_fd = -1;
   device->wddm_luid = ((uint64_t)(uint32_t)found_luid.HighPart << 32) |
                       (uint64_t)(uint32_t)found_luid.LowPart;

   device->dev_id.chip_id = info.chip_id;
   device->dev_id.gpu_id =
      ((info.chip_id >> 24) & 0xff) * 100 +
      ((info.chip_id >> 16) & 0xff) * 10 +
      ((info.chip_id >>  8) & 0xff);
   device->gmem_size = debug_get_num_option("TU_GMEM", info.gmem_size);
   device->gmem_base = info.gmem_base;
   device->uche_trap_base = 0x1fffffffff000ull;

   device->va_start = info.va_start ? info.va_start : 0x20000000ull;
   device->va_size = info.va_size ? info.va_size : 0x30000000ull;

   device->ubwc_config.highest_bank_bit = info.highest_bank_bit ? info.highest_bank_bit : 16;
   device->ubwc_config.bank_swizzle_levels = info.ubwc_swizzle ? info.ubwc_swizzle : 0x6;
   device->ubwc_config.macrotile_mode =
      info.macrotile_mode ? FDL_MACROTILE_8_CHANNEL : FDL_MACROTILE_4_CHANNEL;

   device->submitqueue_priority_count = 1;
   device->has_set_iova = false;
   device->has_raytracing = false;
   device->has_vm_bind = false;
   device->has_sparse = false;
   device->has_sparse_prr = false;
   device->has_lazy_bos = false;
   device->has_cached_coherent_memory = false;
   device->has_cached_non_coherent_memory = false;
   device->has_preemption = false;
   device->is_perf_cntr_selectable = false;

   device->timeline_type = vk_sync_timeline_get_type(&vk_wddm_sync_type);
   device->sync_types[0] = &vk_wddm_sync_type;
   device->sync_types[1] = &device->timeline_type.sync;
   device->sync_types[2] = NULL;

   uint64_t sys_heap = tu_get_system_heap_size(device);
   if (sys_heap > 512ull * 1024ull * 1024ull)
      sys_heap = 512ull * 1024ull * 1024ull;
   device->heap.size = sys_heap;
   device->heap.used = 0;
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   instance->knl = &wddm_knl_funcs;

   VkResult result = tu_physical_device_init(device, instance);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   list_addtail(&device->vk.link, &instance->vk.physical_devices.list);
   return VK_SUCCESS;
}
