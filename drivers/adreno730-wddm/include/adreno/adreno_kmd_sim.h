/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_KMD_SIM_H
#define ADRENO_KMD_SIM_H

#include "adreno_d3d12.h"
#include "adreno_ir3_ref.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADRENO_SIM_MAX_BOS 64u

typedef struct adreno_sim_bo {
    bool used;
    uint32_t handle;
    uint64_t iova;
    uint8_t *host;
    uint32_t size;
} adreno_sim_bo;

typedef struct adreno_sim_kmd {
    adreno_sim_bo bos[ADRENO_SIM_MAX_BOS];
    uint32_t next_handle;
    uint64_t next_iova;
    uint32_t fence;
    uint32_t last_submit;
    uint32_t error_count;
    adreno_ir3_ref_state compute;
} adreno_sim_kmd;

void adreno_sim_init(adreno_sim_kmd *sim);
adreno_kmd_ops adreno_sim_kmd_ops(adreno_sim_kmd *sim);
void adreno_sim_finish(adreno_sim_kmd *sim);

/* Helpers for tests. */
adreno_sim_bo *adreno_sim_find(adreno_sim_kmd *sim, uint32_t handle);
adreno_sim_bo *adreno_sim_find_iova(adreno_sim_kmd *sim, uint64_t iova,
                                    uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_KMD_SIM_H */
