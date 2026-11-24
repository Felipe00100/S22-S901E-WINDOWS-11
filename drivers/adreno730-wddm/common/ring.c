/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_ring.h"
#include "adreno/adreno_pm4.h"

#include <string.h>
#ifdef _M_ARM64
#include <intrin.h>
#define RING_MB() __dmb(_ARM64_BARRIER_SY)
#else
#define RING_MB() do {} while (0)
#endif

#define AXXX_CP_RB_CNTL_BUFSZ_MASK 0x0000003fu
#define AXXX_CP_RB_CNTL_BLKSZ_MASK 0x00003f00u
#define AXXX_CP_RB_CNTL_NO_UPDATE 0x08000000u

static uint32_t ilog2_u32(uint32_t v)
{
    uint32_t r = 0;
    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

static uint32_t ring_used(const adreno_ring *ring)
{
    return (ring->wptr - ring->rptr) & (ring->size_dwords - 1);
}

uint32_t adreno_ring_free_dwords(const adreno_ring *ring)
{
    uint32_t used;

    if (!ring || !ring->size_dwords)
        return 0;
    used = ring_used(ring);
    /* Keep one dword free so wptr == rptr always means empty. */
    return ring->size_dwords - used - 1;
}

enum adreno_status adreno_ring_init(adreno_ring *ring, uint32_t *host,
                                    uint32_t size_dwords, adreno_iova_t iova)
{
    if (!ring || !host || !adreno_is_pow2_u32(size_dwords) ||
        size_dwords < 16 || (iova & 7) != 0)
        return ADRENO_ERR_INVALID;

    memset(ring, 0, sizeof(*ring));
    ring->host = host;
    ring->iova = iova;
    ring->size_dwords = size_dwords;
    ring->wptr = 0;
    ring->rptr = 0;
    return ADRENO_OK;
}

void adreno_ring_reset(adreno_ring *ring)
{
    if (!ring)
        return;
    ring->wptr = 0;
    ring->rptr = 0;
    ring->pending = false;
    ring->pending_dwords = 0;
    ring->overflow = false;
    ring->total_written = 0;
}

void adreno_ring_set_rptr(adreno_ring *ring, uint32_t rptr)
{
    if (!ring)
        return;
    ring->rptr = rptr & (ring->size_dwords - 1);
}

uint64_t adreno_ring_total_written(const adreno_ring *ring)
{
    return ring ? ring->total_written : 0;
}

static void emit_nop_pad(adreno_ring *ring)
{
    while (ring->wptr < ring->size_dwords) {
        uint32_t avail = ring->size_dwords - ring->wptr;
        uint32_t payload = avail - 1;
        if (payload > 0xfff)
            payload = 0xfff;
        ring->host[ring->wptr++] = ADRENO_PM4_TYPE7_HEADER(CP_NOP, payload);
        while (payload--)
            ring->host[ring->wptr++] = 0;
    }
}

enum adreno_status adreno_ring_reserve(adreno_ring *ring, uint32_t dwords,
                                       uint32_t **out)
{
    uint32_t free_dwords;
    uint32_t alloc_dwords;
    uint32_t pad = 0;

    if (!ring || !out || !ring->host || dwords == 0)
        return ADRENO_ERR_INVALID;
    if (ring->pending)
        return ADRENO_ERR_STATE;
    if (dwords > ring->size_dwords - 2)
        return ADRENO_ERR_RANGE;

    /* The CP requires every WPTR update to be 8-byte aligned. */
    alloc_dwords = (dwords + 1u) & ~1u;
    free_dwords = adreno_ring_free_dwords(ring);
    if (ring->wptr + alloc_dwords > ring->size_dwords)
        pad = ring->size_dwords - ring->wptr;

    if (free_dwords < alloc_dwords + pad) {
        ring->overflow = true;
        return ADRENO_ERR_NOMEM;
    }

    if (pad) {
        emit_nop_pad(ring);
        if (ring->wptr != ring->size_dwords) {
            ring->overflow = true;
            return ADRENO_ERR_STATE;
        }
        ring->wptr = 0;
    }

    ring->pending = true;
    ring->pending_dwords = alloc_dwords;
    *out = &ring->host[ring->wptr];
    return ADRENO_OK;
}

void adreno_ring_commit(adreno_ring *ring, uint32_t dwords)
{
    uint32_t alloc_dwords;

    if (!ring || !ring->pending)
        return;
    if (dwords > ring->pending_dwords)
        return;
    /* Advance only by actual dwords (rounded up to 8-byte boundary with valid CP_NOP, NEVER 0x00000000!) */
    alloc_dwords = (dwords + 1u) & ~1u;
    if (dwords & 1u)
        ring->host[ring->wptr + dwords] = ADRENO_PM4_TYPE7_HEADER(CP_NOP, 0);
    RING_MB();
    ring->wptr = (ring->wptr + alloc_dwords) & (ring->size_dwords - 1);
    ring->pending = false;
    ring->pending_dwords = 0;
    ring->total_written += dwords;
}

enum adreno_status adreno_ring_write(adreno_ring *ring, const uint32_t *data,
                                     uint32_t dwords)
{
    uint32_t *dst;
    enum adreno_status st;

    if (!data && dwords)
        return ADRENO_ERR_INVALID;
    st = adreno_ring_reserve(ring, dwords, &dst);
    if (st != ADRENO_OK)
        return st;
    memcpy(dst, data, (size_t)dwords * sizeof(uint32_t));
    adreno_ring_commit(ring, dwords);
    return ADRENO_OK;
}

uint32_t adreno_ring_rb_cntl(uint32_t size_dwords)
{
    uint32_t bytes = size_dwords * 4u;
    uint32_t bufsz;

    if (!adreno_is_pow2_u32(size_dwords) || bytes < 64)
        return 0;
    bufsz = ilog2_u32(bytes / 8u) & AXXX_CP_RB_CNTL_BUFSZ_MASK;
    return bufsz | (2u << 8) /* 32-byte block */;
}
