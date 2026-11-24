/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_RING_H
#define ADRENO_RING_H

#include "adreno_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A single Adreno CP ring. The ring is always a power-of-two number of
 * dwords, as required by CP_RB_CNTL.RB_BUFSZ. The host writes packets with
 * reserve/commit; the GPU consumes them and updates the read pointer.
 */
typedef struct adreno_ring {
    uint32_t *host;
    adreno_iova_t iova;
    uint32_t size_dwords;
    uint32_t wptr;
    uint32_t rptr;
    bool pending;
    uint32_t pending_dwords;
    bool overflow;
    uint64_t total_written;
} adreno_ring;

typedef void (*adreno_ring_kick_fn)(void *ctx, uint32_t wptr);

enum adreno_status adreno_ring_init(adreno_ring *ring, uint32_t *host,
                                    uint32_t size_dwords, adreno_iova_t iova);
void adreno_ring_reset(adreno_ring *ring);

/* Number of dwords that can be reserved without wrapping/overwriting rptr. */
uint32_t adreno_ring_free_dwords(const adreno_ring *ring);

/*
 * Reserve a contiguous range of dwords. If the range would cross the end of
 * the buffer, the remaining space is padded with CP_NOP packets and the
 * reservation restarts at index 0. The returned pointer is valid until the
 * next reserve.
 */
enum adreno_status adreno_ring_reserve(adreno_ring *ring, uint32_t dwords,
                                       uint32_t **out);
void adreno_ring_commit(adreno_ring *ring, uint32_t dwords);

/* Copy a complete packet stream into the ring (reserve + memcpy + commit). */
enum adreno_status adreno_ring_write(adreno_ring *ring, const uint32_t *data,
                                     uint32_t dwords);

/* Host-side read pointer update (normally read from CP_RB_RPTR). */
void adreno_ring_set_rptr(adreno_ring *ring, uint32_t rptr);

/* Number of dwords consumed since reset, for diagnostics. */
uint64_t adreno_ring_total_written(const adreno_ring *ring);

/* CP_RB_CNTL value for a ring of size_dwords. */
uint32_t adreno_ring_rb_cntl(uint32_t size_dwords);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_RING_H */
