/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_A730_H
#define ADRENO_A730_H

#include "adreno_types.h"
#include "adreno_fw.h"
#include "adreno_gpu.h"
#include "adreno_hfi.h"
#include "adreno_pm4.h"
#include "adreno_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adreno_space {
    ADRENO_SPACE_GPU = 0,
    ADRENO_SPACE_GMU = 1,
    ADRENO_SPACE_GMU_RSCC = 2,
    ADRENO_SPACE_CX_MISC = 3,
    ADRENO_SPACE_CX_DBGC = 4,
    ADRENO_SPACE_SMMU = 5,
    ADRENO_SPACE_MAX
} adreno_space;

typedef struct adreno_hw_ops {
    void *ctx;
    uint32_t (*read32)(void *ctx, adreno_space space, uint32_t reg);
    void (*write32)(void *ctx, adreno_space space, uint32_t reg,
                    uint32_t value);
    void (*write_bulk)(void *ctx, adreno_space space, uint32_t reg,
                       const void *data, size_t bytes);
    void (*udelay)(void *ctx, uint32_t usec);
    void (*log)(void *ctx, const char *message);
} adreno_hw_ops;

#define ADRENO_A730_MAX_WINDOWS 16

typedef struct adreno_mem_window {
    const char *name;
    adreno_iova_t iova;
    uint32_t size;
    uint8_t *host;
} adreno_mem_window;

typedef struct adreno_a730 {
    adreno_hw_ops hw;
    const adreno_gpu_info *info;
    uint32_t chip_id;
    uint32_t num_windows;
    adreno_mem_window windows[ADRENO_A730_MAX_WINDOWS];
    bool gmu_booted;
    bool cp_started;
    adreno_ring ring;
    bool ring_ready;
} adreno_a730;

/* Register helpers. */
static inline uint32_t adreno_read(adreno_a730 *g, adreno_space space,
                                   uint32_t reg)
{
    return g->hw.read32(g->hw.ctx, space, reg);
}

static inline void adreno_write(adreno_a730 *g, adreno_space space,
                                uint32_t reg, uint32_t value)
{
    g->hw.write32(g->hw.ctx, space, reg, value);
}

static inline void adreno_write64(adreno_a730 *g, adreno_space space,
                                  uint32_t reg, uint64_t value)
{
    adreno_write(g, space, reg, (uint32_t)value);
    adreno_write(g, space, reg + 1, (uint32_t)(value >> 32));
}

void adreno_a730_init(adreno_a730 *g, const adreno_hw_ops *hw,
                      uint32_t chip_id);
enum adreno_status adreno_a730_add_window(adreno_a730 *g, const char *name,
                                          adreno_iova_t iova, uint32_t size,
                                          void *host);
adreno_mem_window *adreno_a730_find_window(adreno_a730 *g,
                                           const char *name);
adreno_mem_window *adreno_a730_window_for_iova(adreno_a730 *g,
                                               adreno_iova_t iova,
                                               uint32_t size);

/*
 * Program the static (non-GMU) GPU registers needed before CP bring-up.
 * This is a conservative subset of a6xx_hw_init() for the A730 path; clock
 * gating is left off until the tuning tables are validated on hardware.
 */
enum adreno_status adreno_a730_hw_init(adreno_a730 *g,
                                        adreno_iova_t uche_trap_iova);

/*
 * Load gmu_gen70000.bin. ITCM/DTCM blocks go through GMU registers; other
 * blocks are copied into the matching cache/dummy window.
 */
enum adreno_status adreno_a730_gmu_load_fw(adreno_a730 *g,
                                           const adreno_fw_image *fw);

/*
 * Cold-boot the GMU (a6xx_gmu_fw_start + a6xx_gmu_start + HFI start).
 * Requires HFI 16K, log 16K, icache, dummy windows.
 */
enum adreno_status adreno_a730_gmu_boot(adreno_a730 *g,
                                        const adreno_fw_image *fw,
                                        uint32_t boot_state,
                                        uint32_t timeout_us);

/* Send the non-legacy HFI startup sequence and wait for each ACK. */
enum adreno_status adreno_a730_hfi_start(adreno_a730 *g, adreno_hfi *hfi,
                                         uint32_t timeout_us);

/* Configure CP ring registers and start the SQE. */
enum adreno_status adreno_a730_ring_init(adreno_a730 *g);
enum adreno_status adreno_a730_cp_start(adreno_a730 *g,
                                        adreno_iova_t pwrup_reglist_iova,
                                        uint32_t timeout_us);

/* Write WPTR to make the CP fetch everything committed so far. */
void adreno_a730_ring_kick(adreno_a730 *g, adreno_ring *ring);

/* Emit a submit into the ring and kick it. Returns ADRENO_OK if queued. */
enum adreno_status adreno_a730_submit(adreno_a730 *g, adreno_ring *ring,
                                      const adreno_ib *ibs, uint32_t num_ibs,
                                      uint32_t seqno);

/* Poll helper used by the boot sequence. */
enum adreno_status adreno_hw_poll(adreno_a730 *g, adreno_space space,
                                  uint32_t reg, uint32_t mask,
                                  uint32_t expected, uint32_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_A730_H */

