/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_wddm.h"
#include "adreno/a730_regs.h"

#include <string.h>

static void wddm_log(adreno_wddm *w, const char *message)
{
    if (w && w->ops.log)
        w->ops.log(w->ops.ctx, message);
}

static bool ranges_overlap(uint64_t a, uint32_t asize, uint64_t b,
                           uint32_t bsize)
{
    return a < b + bsize && b < a + asize;
}

enum adreno_status adreno_wddm_init(adreno_wddm *w,
                                     const adreno_wddm_ops *ops)
{
    if (!w || !ops || !ops->alloc || !ops->free)
        return ADRENO_ERR_INVALID;
    memset(w, 0, sizeof(*w));
    w->ops = *ops;
    w->next_iova = ADRENO_WDDM_IOVA_BASE;
    w->hardware = ops->submit_ib != NULL;
    wddm_log(w, "wddm: core initialized");
    return ADRENO_OK;
}

void adreno_wddm_fini(adreno_wddm *w)
{
    uint32_t i;

    if (!w)
        return;
    for (i = 0; i < ADRENO_WDDM_MAX_ALLOCS; i++) {
        if (w->allocs[i].in_use) {
            w->ops.free(w->ops.ctx, w->allocs[i].host);
            memset(&w->allocs[i], 0, sizeof(w->allocs[i]));
        }
    }
    memset(w, 0, sizeof(*w));
}

enum adreno_status adreno_wddm_alloc_buf(adreno_wddm *w, uint32_t size,
                                          uint32_t align,
                                          adreno_wddm_alloc **out)
{
    adreno_wddm_alloc *alloc = NULL;
    void *host = NULL;
    uint64_t pa = 0;
    uint32_t backend_handle = 0;
    uint64_t iova;
    uint32_t i;

    if (!w || !out || size == 0 || size > 256u * 1024u * 1024u)
        return ADRENO_ERR_INVALID;
    if (align == 0)
        align = 4096;
    if (align < 4096 || align > 1024u * 1024u ||
        !adreno_is_pow2_u32(align))
        return ADRENO_ERR_INVALID;
    size = (uint32_t)ADRENO_ALIGN_UP(size, 4096);

    for (i = 0; i < ADRENO_WDDM_MAX_ALLOCS; i++) {
        if (!w->allocs[i].in_use) {
            alloc = &w->allocs[i];
            break;
        }
    }
    if (!alloc)
        return ADRENO_ERR_NOMEM;

    if (!w->ops.alloc(w->ops.ctx, size, &host, &pa, &backend_handle) ||
        !host)
        return ADRENO_ERR_NOMEM;

    if (pa != 0) {
        iova = ADRENO_ALIGN_UP(pa, align);
    } else {
        iova = ADRENO_ALIGN_UP(w->next_iova, align);
        w->next_iova = iova + size;
    }
    for (i = 0; i < ADRENO_WDDM_MAX_ALLOCS; i++) {
        if (w->allocs[i].in_use &&
            ranges_overlap(iova, size, w->allocs[i].iova,
                           w->allocs[i].size)) {
            w->ops.free(w->ops.ctx, host);
            return ADRENO_ERR_NOMEM;
        }
    }

    memset(alloc, 0, sizeof(*alloc));
    alloc->handle = ++w->next_handle;
    alloc->backend_handle = backend_handle;
    alloc->size = size;
    alloc->pa = pa;
    alloc->iova = iova;
    alloc->host = host;
    alloc->in_use = true;
    w->alloc_count++;
    *out = alloc;
    return ADRENO_OK;
}

void adreno_wddm_free_buf(adreno_wddm *w, adreno_wddm_alloc *alloc)
{
    if (!w || !alloc || !alloc->in_use)
        return;
    w->ops.free(w->ops.ctx, alloc->host);
    if (w->alloc_count)
        w->alloc_count--;
    memset(alloc, 0, sizeof(*alloc));
}

adreno_wddm_alloc *adreno_wddm_find_handle(adreno_wddm *w, uint32_t handle)
{
    uint32_t i;
    if (!w || handle == 0)
        return NULL;
    for (i = 0; i < ADRENO_WDDM_MAX_ALLOCS; i++) {
        if (w->allocs[i].in_use &&
            (w->allocs[i].handle == handle ||
             (w->allocs[i].backend_handle != 0 &&
              w->allocs[i].backend_handle == handle)))
            return &w->allocs[i];
    }
    return NULL;
}

adreno_wddm_alloc *adreno_wddm_find_iova(adreno_wddm *w, uint64_t iova,
                                         uint32_t size)
{
    uint32_t i;
    if (!w)
        return NULL;
    for (i = 0; i < ADRENO_WDDM_MAX_ALLOCS; i++) {
        adreno_wddm_alloc *a = &w->allocs[i];
        if (a->in_use && iova >= a->iova &&
            iova + size <= a->iova + a->size)
            return a;
    }
    return NULL;
}

adreno_wddm_alloc *adreno_wddm_find_pa(adreno_wddm *w, uint64_t pa,
                                       uint32_t size)
{
    uint32_t i;
    if (!w)
        return NULL;
    for (i = 0; i < ADRENO_WDDM_MAX_ALLOCS; i++) {
        adreno_wddm_alloc *a = &w->allocs[i];
        if (a->in_use && a->pa != 0 && pa >= a->pa &&
            pa + size <= a->pa + a->size)
            return a;
    }
    return NULL;
}

enum adreno_status adreno_wddm_paging_fill(adreno_wddm *w,
                                            adreno_wddm_alloc *alloc,
                                            uint32_t offset, uint32_t size,
                                            uint32_t pattern)
{
    uint32_t i;
    uint8_t *dst;

    if (!w || !alloc || !alloc->in_use ||
        (uint64_t)offset + size > alloc->size)
        return ADRENO_ERR_RANGE;
    dst = (uint8_t *)alloc->host + offset;
    for (i = 0; i < size / 4; i++)
        ((uint32_t *)dst)[i] = pattern;
    for (i = (size / 4) * 4; i < size; i++)
        dst[i] = (uint8_t)(pattern >> ((i & 3) * 8));
    return ADRENO_OK;
}

enum adreno_status adreno_wddm_paging_transfer(adreno_wddm *w,
                                                adreno_wddm_alloc *alloc,
                                                uint32_t offset,
                                                const void *src,
                                                uint32_t size)
{
    if (!w || !alloc || !alloc->in_use || !src ||
        (uint64_t)offset + size > alloc->size)
        return ADRENO_ERR_RANGE;
    memmove((uint8_t *)alloc->host + offset, src, size);
    return ADRENO_OK;
}

enum adreno_status adreno_wddm_paging_discard(adreno_wddm *w,
                                               adreno_wddm_alloc *alloc)
{
    if (!w || !alloc || !alloc->in_use)
        return ADRENO_ERR_INVALID;
    memset(alloc->host, 0, alloc->size);
    return ADRENO_OK;
}

enum adreno_status adreno_wddm_patch(adreno_wddm *w, void *dma,
                                      uint32_t dma_size,
                                      const adreno_wddm_patch_entry *entries,
                                      uint32_t count)
{
    uint32_t i;

    if (!w || !dma || (!entries && count))
        return ADRENO_ERR_INVALID;
    for (i = 0; i < count; i++) {
        adreno_wddm_alloc *alloc =
            adreno_wddm_find_handle(w, entries[i].handle);
        uint64_t value;
        if (!alloc || (entries[i].size != 4 && entries[i].size != 8))
            return ADRENO_ERR_INVALID;
        if ((uint64_t)entries[i].patch_offset + entries[i].size > dma_size)
            return ADRENO_ERR_RANGE;
        if ((uint64_t)entries[i].alloc_offset + entries[i].size >
            alloc->size)
            return ADRENO_ERR_RANGE;
        value = alloc->iova + entries[i].alloc_offset;
        if (entries[i].size == 4) {
            uint32_t v = (uint32_t)value;
            memcpy((uint8_t *)dma + entries[i].patch_offset, &v, 4);
        } else {
            memcpy((uint8_t *)dma + entries[i].patch_offset, &value, 8);
        }
    }
    return ADRENO_OK;
}

static enum adreno_status wddm_ref_read_iova(void *ctx, uint64_t iova,
                                              void *dst, uint32_t size)
{
    adreno_wddm *w = (adreno_wddm *)ctx;
    adreno_wddm_alloc *a = adreno_wddm_find_iova(w, iova, size);

    if (!a)
        return ADRENO_ERR_RANGE;
    memcpy(dst, (uint8_t *)a->host + (iova - a->iova), size);
    return ADRENO_OK;
}

static enum adreno_status wddm_ref_write_u32(void *ctx, uint64_t iova,
                                              uint32_t value)
{
    adreno_wddm *w = (adreno_wddm *)ctx;
    adreno_wddm_alloc *a = adreno_wddm_find_iova(w, iova, 4);

    if (!a)
        return ADRENO_ERR_RANGE;
    memcpy((uint8_t *)a->host + (iova - a->iova), &value, 4);
    return ADRENO_OK;
}

static enum adreno_status wddm_exec(adreno_wddm *w, const uint32_t *data,
                                    uint32_t dwords, uint32_t depth)
{
    uint32_t i = 0;

    if (depth > 4)
        return ADRENO_ERR_RANGE;
    while (i < dwords) {
        uint32_t hdr = data[i];
        uint32_t type = hdr >> 28;
        uint32_t count = (type == 4) ? (hdr & 0x7fu) : (hdr & 0x7fffu);
        uint32_t op = (hdr >> 16) & 0x7fu;

        if (count > dwords - i - 1)
            return ADRENO_ERR_RANGE;
        if (type == 4) {
            /* Register writes are not part of the WDDM reference path. */
        } else if (type == 7) {
            switch (op) {
            case CP_NOP:
            case CP_THREAD_CONTROL:
            case CP_SET_MARKER:
            case CP_ME_INIT:
            case CP_SET_AMBLE:
            case CP_CONTEXT_SWITCH_YIELD:
            case CP_WAIT_FOR_IDLE:
                break;
            case CP_LOAD_STATE6:
            case CP_LOAD_STATE6_FRAG: {
                uint32_t ctrl, state_type, state_src, state_block;
                uint32_t dst_off, units;
                uint64_t addr;
                enum adreno_status cst;

                if (count < 3)
                    return ADRENO_ERR_INVALID;
                ctrl = data[i + 1];
                state_type = (ctrl >> 14) & 3u;
                state_src = (ctrl >> 16) & 3u;
                state_block = (ctrl >> 18) & 0xfu;
                dst_off = ctrl & 0x3fffu;
                units = (ctrl >> 22) & 0x3ffu;
                addr = (uint64_t)data[i + 2] |
                       ((uint64_t)data[i + 3] << 32);
                if (state_block == ADRENO_SB6_CS_SHADER) {
                    if (state_type == ADRENO_ST6_SHADER &&
                        state_src == ADRENO_SS6_INDIRECT) {
                        if (count != 3 || units == 0)
                            return ADRENO_ERR_INVALID;
                        cst = adreno_ir3_ref_load_shader(
                            &w->compute, addr, units, wddm_ref_read_iova, w);
                        if (cst != ADRENO_OK)
                            return cst;
                    } else if (state_type == ADRENO_ST6_CONSTANTS &&
                               state_src == ADRENO_SS6_DIRECT) {
                        if (count != 3 + units * 4u || units == 0)
                            return ADRENO_ERR_INVALID;
                        cst = adreno_ir3_ref_load_const(
                            &w->compute, dst_off, units, &data[i + 4]);
                        if (cst != ADRENO_OK)
                            return cst;
                    }
                }
                break;
            }
            case CP_EXEC_CS: {
                uint32_t grid[3];
                enum adreno_status cst;
                if (count != 4)
                    return ADRENO_ERR_INVALID;
                grid[0] = data[i + 2];
                grid[1] = data[i + 3];
                grid[2] = data[i + 4];
                cst = adreno_ir3_ref_exec_state(&w->compute, grid,
                                                wddm_ref_write_u32, w);
                if (cst != ADRENO_OK)
                    return cst;
                break;
            }
            case CP_INDIRECT_BUFFER: {
                uint64_t iova;
                uint32_t size;
                adreno_wddm_alloc *a;
                if (count != 3)
                    return ADRENO_ERR_INVALID;
                iova = (uint64_t)data[i + 1] |
                       ((uint64_t)data[i + 2] << 32);
                size = data[i + 3];
                a = adreno_wddm_find_iova(w, iova, size * 4);
                if (!a)
                    return ADRENO_ERR_RANGE;
                if (wddm_exec(w, (const uint32_t *)((uint8_t *)a->host +
                                                   (iova - a->iova)),
                              size, depth + 1) != ADRENO_OK)
                    return ADRENO_ERR_STATE;
                break;
            }
            case CP_MEM_WRITE: {
                uint64_t iova;
                adreno_wddm_alloc *a;
                if (count != 3)
                    return ADRENO_ERR_INVALID;
                iova = (uint64_t)data[i + 1] |
                       ((uint64_t)data[i + 2] << 32);
                a = adreno_wddm_find_iova(w, iova, 4);
                if (!a)
                    return ADRENO_ERR_RANGE;
                memcpy((uint8_t *)a->host + (iova - a->iova), &data[i + 3],
                       4);
                break;
            }
            case CP_MEMCPY: {
                uint32_t copy_dwords = data[i + 1];
                uint64_t src = (uint64_t)data[i + 2] |
                               ((uint64_t)data[i + 3] << 32);
                uint64_t dst = (uint64_t)data[i + 4] |
                               ((uint64_t)data[i + 5] << 32);
                adreno_wddm_alloc *sa, *da;
                if (count != 5 || copy_dwords == 0)
                    return ADRENO_ERR_INVALID;
                sa = adreno_wddm_find_iova(w, src, copy_dwords * 4);
                da = adreno_wddm_find_iova(w, dst, copy_dwords * 4);
                if (!sa || !da)
                    return ADRENO_ERR_RANGE;
                memmove((uint8_t *)da->host + (dst - da->iova),
                        (uint8_t *)sa->host + (src - sa->iova),
                        copy_dwords * 4);
                break;
            }
            case CP_EVENT_WRITE: {
                uint32_t flags = data[i + 1];
                if (count == 4) {
                    uint64_t iova = (uint64_t)data[i + 2] |
                                    ((uint64_t)data[i + 3] << 32);
                    adreno_wddm_alloc *a =
                        adreno_wddm_find_iova(w, iova, 4);
                    if (a)
                        memcpy((uint8_t *)a->host + (iova - a->iova),
                               &data[i + 4], 4);
                    if (flags & 0x88000000u)
                        w->completed_fence = data[i + 4];
                }
                break;
            }
            case CP_WAIT_TIMESTAMP:
                if (count != 4)
                    return ADRENO_ERR_INVALID;
                if (w->completed_fence < data[i + 4])
                    return ADRENO_ERR_STATE;
                break;
            default:
                return ADRENO_ERR_UNSUPPORTED;
            }
        } else {
            return ADRENO_ERR_INVALID;
        }
        i += 1 + count;
    }
    return ADRENO_OK;
}

enum adreno_status adreno_wddm_submit_dma(adreno_wddm *w, uint64_t dma_iova,
                                           const void *dma_host, uint32_t bytes,
                                           uint32_t fence_id)
{
    enum adreno_status st;

    if (!w || !dma_host || bytes == 0 || (bytes & 3))
        return ADRENO_ERR_INVALID;
    if (fence_id == 0)
        fence_id = w->last_fence + 1;

    if (w->hardware) {
        adreno_ib ib;
        ib.iova = dma_iova;
        ib.size_dwords = bytes / 4;
        ib.handle = 0;
        st = w->ops.submit_ib(w->ops.ctx, &ib, 1, fence_id);
        if (st != ADRENO_OK)
            return st;
        w->last_fence = fence_id;
        return ADRENO_OK;
    }

    st = adreno_pm4_validate((const uint32_t *)dma_host, bytes / 4);
    if (st != ADRENO_OK)
        return st;
    adreno_ir3_ref_reset(&w->compute);
    st = wddm_exec(w, (const uint32_t *)dma_host, bytes / 4, 0);
    if (st != ADRENO_OK)
        return st;
    w->last_fence = fence_id;
    w->completed_fence = fence_id;
    return ADRENO_OK;
}

enum adreno_status adreno_wddm_submit_ibs(adreno_wddm *w,
                                           const adreno_ib *ibs,
                                           uint32_t count, uint32_t seqno)
{
    uint32_t i;
    enum adreno_status st;

    if (!w || !ibs || count == 0 || count > 32)
        return ADRENO_ERR_INVALID;
    if (seqno == 0)
        seqno = w->last_fence + 1;

    if (w->hardware) {
        st = w->ops.submit_ib(w->ops.ctx, ibs, count, seqno);
        if (st == ADRENO_OK)
            w->last_fence = seqno;
        return st;
    }

    adreno_ir3_ref_reset(&w->compute);
    for (i = 0; i < count; i++) {
        adreno_wddm_alloc *a;
        if (ibs[i].size_dwords == 0)
            continue;
        a = adreno_wddm_find_iova(w, ibs[i].iova,
                                  ibs[i].size_dwords * 4);
        if (!a)
            return ADRENO_ERR_RANGE;
        st = adreno_pm4_validate((const uint32_t *)((uint8_t *)a->host +
                                                   (ibs[i].iova - a->iova)),
                                 ibs[i].size_dwords);
        if (st != ADRENO_OK)
            return st;
        st = wddm_exec(w, (const uint32_t *)((uint8_t *)a->host +
                                            (ibs[i].iova - a->iova)),
                       ibs[i].size_dwords, 0);
        if (st != ADRENO_OK)
            return st;
    }
    w->last_fence = seqno;
    w->completed_fence = seqno;
    return ADRENO_OK;
}

enum adreno_status adreno_wddm_wait(adreno_wddm *w, uint32_t seqno,
                                    uint32_t timeout_ms)
{
    enum adreno_status st;

    if (!w || seqno == 0)
        return ADRENO_ERR_INVALID;
    if (w->completed_fence >= seqno)
        return ADRENO_OK;
    if (!w->hardware)
        return ADRENO_ERR_TIMEOUT;
    if (!w->ops.wait_fence)
        return ADRENO_ERR_UNSUPPORTED;
    st = w->ops.wait_fence(w->ops.ctx, seqno, timeout_ms);
    if (st == ADRENO_OK)
        w->completed_fence = seqno;
    return st;
}

uint32_t adreno_wddm_completed_fence(const adreno_wddm *w)
{
    return w ? w->completed_fence : 0;
}

/* ---- D3D12 backend binding --------------------------------------------- */

static enum adreno_status wddm_kmd_alloc(void *ctx, uint32_t size,
                                         uint32_t *handle, uint64_t *iova,
                                         void **host)
{
    adreno_wddm *w = (adreno_wddm *)ctx;
    adreno_wddm_alloc *alloc;
    enum adreno_status st = adreno_wddm_alloc_buf(w, size, 4096, &alloc);
    if (st != ADRENO_OK)
        return st;
    *handle = alloc->backend_handle ? alloc->backend_handle : alloc->handle;
    *iova = alloc->iova;
    *host = alloc->host;
    return ADRENO_OK;
}

static enum adreno_status wddm_kmd_free(void *ctx, uint32_t handle)
{
    adreno_wddm *w = (adreno_wddm *)ctx;
    adreno_wddm_alloc *alloc = adreno_wddm_find_handle(w, handle);
    if (!alloc)
        return ADRENO_ERR_INVALID;
    adreno_wddm_free_buf(w, alloc);
    return ADRENO_OK;
}

static enum adreno_status wddm_kmd_submit(void *ctx, const adreno_ib *ibs,
                                          uint32_t count, uint32_t seqno)
{
    return adreno_wddm_submit_ibs((adreno_wddm *)ctx, ibs, count, seqno);
}

static enum adreno_status wddm_kmd_wait(void *ctx, uint32_t seqno,
                                        uint32_t timeout_ms)
{
    return adreno_wddm_wait((adreno_wddm *)ctx, seqno, timeout_ms);
}

static enum adreno_status wddm_kmd_read(void *ctx, uint32_t handle,
                                        uint32_t offset, void *dst,
                                        uint32_t size)
{
    adreno_wddm *w = (adreno_wddm *)ctx;
    adreno_wddm_alloc *alloc = adreno_wddm_find_handle(w, handle);
    if (!alloc || (uint64_t)offset + size > alloc->size)
        return ADRENO_ERR_RANGE;
    memcpy(dst, (uint8_t *)alloc->host + offset, size);
    return ADRENO_OK;
}

adreno_kmd_ops adreno_wddm_kmd_ops(adreno_wddm *w)
{
    adreno_kmd_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.ctx = w;
    ops.alloc = wddm_kmd_alloc;
    ops.free = wddm_kmd_free;
    ops.submit = wddm_kmd_submit;
    ops.wait = wddm_kmd_wait;
    ops.read = wddm_kmd_read;
    return ops;
}
