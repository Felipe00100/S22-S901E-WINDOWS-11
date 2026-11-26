/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_WDDM_H
#define ADRENO_WDDM_H

#include "adreno_d3d12.h"
#include "adreno_ir3_ref.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADRENO_WDDM_MAX_ALLOCS 256
#define ADRENO_WDDM_IOVA_BASE 0x20000000ull

/*
 * WDDM-side memory and submission model.
 *
 * This is deliberately independent of the DDK so the exact allocation,
 * paging, patch and submit rules can be tested on the host. The WDDM miniport
 * binds the callbacks to MmAllocateContiguousMemory and, when the real GPU
 * backend is enabled, to the A7xx CP submit path.
 */
typedef struct adreno_wddm_alloc {
    uint32_t handle;
    uint32_t backend_handle;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t dxgi_format;
    uint64_t pa;
    uint64_t iova;
    void *host;
    bool in_use;
    bool mapped; /* KMD-specific: SMMU entry installed by WDDM */
    bool texture2d;
} adreno_wddm_alloc;

typedef struct adreno_wddm_ops {
    void *ctx;
    /* Contiguous or at least permanently resident system memory. */
    bool (*alloc)(void *ctx, uint32_t size, void **host, uint64_t *pa,
                  uint32_t *backend_handle);
    void (*free)(void *ctx, void *host);
    void (*log)(void *ctx, const char *message);
    /*
     * Optional hardware CP submit. When NULL the reference CPU command
     * processor executes the PM4 stream, which is what makes the paging and
     * fence path testable without the GPU booted. The WDDM miniport leaves it
     * NULL until the shared hardware boot path is attached.
     */
    enum adreno_status (*submit_ib)(void *ctx, const adreno_ib *ibs,
                                     uint32_t count, uint32_t seqno);
    enum adreno_status (*wait_fence)(void *ctx, uint32_t seqno,
                                     uint32_t timeout_ms);
} adreno_wddm_ops;

typedef struct adreno_wddm {
    adreno_wddm_ops ops;
    adreno_wddm_alloc allocs[ADRENO_WDDM_MAX_ALLOCS];
    uint32_t alloc_count;
    uint64_t next_iova;
    uint32_t next_handle;
    uint32_t last_fence;
    uint32_t completed_fence;
    bool hardware;
    adreno_ir3_ref_state compute;
} adreno_wddm;

typedef struct adreno_wddm_patch_entry {
    uint32_t handle;
    uint32_t alloc_offset;
    uint32_t patch_offset;
    uint32_t size; /* 4 or 8 bytes */
} adreno_wddm_patch_entry;

enum adreno_status adreno_wddm_init(adreno_wddm *w,
                                     const adreno_wddm_ops *ops);
void adreno_wddm_fini(adreno_wddm *w);

enum adreno_status adreno_wddm_alloc_buf(adreno_wddm *w, uint32_t size,
                                          uint32_t align,
                                          adreno_wddm_alloc **out);
void adreno_wddm_free_buf(adreno_wddm *w, adreno_wddm_alloc *alloc);
adreno_wddm_alloc *adreno_wddm_find_handle(adreno_wddm *w, uint32_t handle);
adreno_wddm_alloc *adreno_wddm_find_iova(adreno_wddm *w, uint64_t iova,
                                         uint32_t size);
adreno_wddm_alloc *adreno_wddm_find_pa(adreno_wddm *w, uint64_t pa,
                                       uint32_t size);

/* Paging operations for a fully resident system-memory aperture. */
enum adreno_status adreno_wddm_paging_fill(adreno_wddm *w,
                                            adreno_wddm_alloc *alloc,
                                            uint32_t offset, uint32_t size,
                                            uint32_t pattern);
enum adreno_status adreno_wddm_paging_transfer(adreno_wddm *w,
                                                adreno_wddm_alloc *alloc,
                                                uint32_t offset,
                                                const void *src,
                                                uint32_t size);
enum adreno_status adreno_wddm_paging_discard(adreno_wddm *w,
                                               adreno_wddm_alloc *alloc);

/* Patch GPU virtual addresses into a UMD DMA buffer. */
enum adreno_status adreno_wddm_patch(adreno_wddm *w, void *dma,
                                      uint32_t dma_size,
                                      const adreno_wddm_patch_entry *entries,
                                      uint32_t count);

/*
 * Submit one PM4 stream. dma_iova is the GPU address of the DMA buffer and
 * dma_host is its CPU mapping. With a hardware submit callback the IOVA is
 * handed to the CP; otherwise the reference executor walks the stream using
 * the allocation table.
 */
enum adreno_status adreno_wddm_submit_dma(adreno_wddm *w, uint64_t dma_iova,
                                           const void *dma_host, uint32_t bytes,
                                           uint32_t fence_id);
enum adreno_status adreno_wddm_submit_ibs(adreno_wddm *w,
                                           const adreno_ib *ibs,
                                           uint32_t count, uint32_t seqno);
enum adreno_status adreno_wddm_wait(adreno_wddm *w, uint32_t seqno,
                                    uint32_t timeout_ms);
uint32_t adreno_wddm_completed_fence(const adreno_wddm *w);

/* Bind the portable D3D12 backend to WDDM allocations and submission. */
adreno_kmd_ops adreno_wddm_kmd_ops(adreno_wddm *w);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_WDDM_H */
