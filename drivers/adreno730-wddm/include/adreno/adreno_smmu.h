/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_SMMU_H
#define ADRENO_SMMU_H

#include "adreno_types.h"
#include "adreno_a730.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ARMv8-A stage-1, 4K granule, 39-bit VA page tables, as used by the
 * Qualcomm SMMU-500 on SM8450. Tables are allocated from a caller-provided
 * physically contiguous pool (typically 256 KiB).
 */
typedef struct adreno_pt_pool {
    uint8_t *base;
    uint64_t base_pa;
    size_t size;
    size_t used;
} adreno_pt_pool;

typedef struct adreno_pt {
    adreno_pt_pool *pool;
    uint64_t *root;
    uint64_t root_pa;
    uint32_t mapped_pages;
    uint32_t va_bits;
} adreno_pt;

void adreno_pt_pool_init(adreno_pt_pool *pool, void *base, size_t size,
                         uint64_t base_pa);
enum adreno_status adreno_pt_init(adreno_pt *pt, adreno_pt_pool *pool);
enum adreno_status adreno_pt_map(adreno_pt *pt, uint64_t iova, uint64_t pa,
                                 uint64_t size, bool device);
enum adreno_status adreno_pt_unmap(adreno_pt *pt, uint64_t iova,
                                   uint64_t size);
/* Returns 0 and sets *mapped=false if not present. */
uint64_t adreno_pt_walk(const adreno_pt *pt, uint64_t iova, bool *mapped,
                        bool *device);

/* ---- SMMU-500 (SMMUv2) programming ------------------------------------ */

#define ADRENO_SMMU_GR0 0x0000u
#define ADRENO_SMMU_GR1 0x1000u
#define ADRENO_SMMU_GR0_sCR0 0x0000u
#define ADRENO_SMMU_GR0_ID0 0x0020u
#define ADRENO_SMMU_GR0_ID1 0x0024u
#define ADRENO_SMMU_GR0_TLBIALLNSNH 0x0068u
#define ADRENO_SMMU_GR0_sTLBGSYNC 0x0070u
#define ADRENO_SMMU_GR0_sTLBGSTATUS 0x0074u
#define ADRENO_SMMU_GR0_SMR(n) (0x0800u + ((n) << 2))
#define ADRENO_SMMU_GR0_S2CR(n) (0x0c00u + ((n) << 2))
#define ADRENO_SMMU_GR1_CBAR(n) (0x0000u + ((n) << 2))
#define ADRENO_SMMU_GR1_CBA2R(n) (0x0800u + ((n) << 2))
#define ADRENO_SMMU_CB_SCTLR 0x0000u
#define ADRENO_SMMU_CB_TTBR0 0x0020u
#define ADRENO_SMMU_CB_TCR 0x0030u
#define ADRENO_SMMU_CB_S1_MAIR0 0x0038u
#define ADRENO_SMMU_CB_S1_MAIR1 0x003cu
#define ADRENO_SMMU_CB_FSR 0x0058u
#define ADRENO_SMMU_CB_TLBSYNC 0x07f0u
#define ADRENO_SMMU_CB_TLBSTATUS 0x07f4u

#define ADRENO_SMMU_ID1_NUMCB_MASK 0xffu
#define ADRENO_SMMU_ID1_NUMPAGENDXB_SHIFT 28
#define ADRENO_SMMU_ID1_NUMPAGENDXB_MASK 0x7u

typedef struct adreno_smmu {
    adreno_a730 *gpu;
    uint32_t num_cbs;
    uint32_t numpage; /* GR0/GR1 group size, in 4K pages */
    uint32_t pgshift;
    uint32_t gpu_cb;
    uint32_t gmu_cb;
} adreno_smmu;

enum adreno_status adreno_smmu_probe(adreno_smmu *smmu, adreno_a730 *gpu);
enum adreno_status adreno_smmu_setup(adreno_smmu *smmu, adreno_pt *gpu_pt,
                                     adreno_pt *gmu_pt, uint16_t gpu_sid,
                                     uint16_t gmu_sid);
void adreno_smmu_tlb_invalidate(adreno_smmu *smmu);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_SMMU_H */
