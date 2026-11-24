/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_PM4_H
#define ADRENO_PM4_H

#include "adreno_types.h"
#include "adreno/a730_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A grow-only packet writer. All units are 32-bit dwords. */
typedef struct adreno_pm4 {
    uint32_t *data;
    uint32_t capacity;
    uint32_t count;
    bool overflow;
} adreno_pm4;

typedef struct adreno_ib {
    adreno_iova_t iova;
    uint32_t size_dwords;
    uint32_t handle; /* optional KMD BO handle for SUBMIT_IB_LIST */
} adreno_ib;

typedef struct adreno_submit_desc {
    const adreno_ib *ibs;
    uint32_t num_ibs;
    adreno_iova_t bv_fence_iova;
    adreno_iova_t fence_iova;
    uint32_t seqno;
} adreno_submit_desc;

/*
 * A6xx/A7xx CP_LOAD_STATE6 state descriptors. These mirror the values in
 * adreno_pm4.xml; the state block for compute is SB6_CS_SHADER.
 */
#define ADRENO_ST6_SHADER 0
#define ADRENO_ST6_CONSTANTS 1
#define ADRENO_ST6_UBO 2
#define ADRENO_ST6_UAV 3
#define ADRENO_SS6_DIRECT 0
#define ADRENO_SS6_INDIRECT 2
#define ADRENO_SB6_CS_SHADER 13

/*
 * One shader preload unit is 16 instructions x 2 dwords = 128 bytes on
 * a4xx..a7xx. CP_LOAD_STATE6 shader num_unit and SP_CS_INSTR_SIZE both use
 * this unit, matching Mesa's v->instrlen.
 */
#define ADRENO_SHADER_UNIT_BYTES 128u

void adreno_pm4_init(adreno_pm4 *pm4, uint32_t *data, uint32_t capacity_dwords);
void adreno_pm4_reset(adreno_pm4 *pm4);
bool adreno_pm4_emit(adreno_pm4 *pm4, uint32_t value);
bool adreno_pm4_emit_pkt4(adreno_pm4 *pm4, uint32_t reg, uint32_t count);
bool adreno_pm4_emit_pkt7(adreno_pm4 *pm4, uint32_t opcode, uint32_t count);
bool adreno_pm4_reg_write(adreno_pm4 *pm4, uint32_t reg, uint32_t value);
bool adreno_pm4_reg_write64(adreno_pm4 *pm4, uint32_t reg, uint64_t value);
bool adreno_pm4_indirect_buffer(adreno_pm4 *pm4, adreno_iova_t iova,
                                uint32_t size_dwords);
bool adreno_pm4_event_write(adreno_pm4 *pm4, uint32_t event, uint32_t flags,
                            adreno_iova_t iova, uint32_t value);
bool adreno_pm4_mem_write(adreno_pm4 *pm4, adreno_iova_t iova, uint32_t value);
bool adreno_pm4_memcpy(adreno_pm4 *pm4, adreno_iova_t dst, adreno_iova_t src,
                       uint32_t dwords);
bool adreno_pm4_load_state6_shader(adreno_pm4 *pm4, uint32_t state_block,
                                   adreno_iova_t iova, uint32_t units);
bool adreno_pm4_load_state6_const(adreno_pm4 *pm4, uint32_t state_block,
                                  uint32_t dst_vec4, const uint32_t *data,
                                  uint32_t vec4s);
bool adreno_pm4_exec_cs(adreno_pm4 *pm4, uint32_t groups_x,
                        uint32_t groups_y, uint32_t groups_z);
bool adreno_pm4_wait_for_idle(adreno_pm4 *pm4);
bool adreno_pm4_cache_flush_ts(adreno_pm4 *pm4);
bool adreno_pm4_event_write_1(adreno_pm4 *pm4, uint32_t value);
bool adreno_pm4_thread_control(adreno_pm4 *pm4, uint32_t thread_value);
bool adreno_pm4_set_marker(adreno_pm4 *pm4, uint32_t marker);
bool adreno_pm4_wait_timestamp(adreno_pm4 *pm4, adreno_iova_t iova,
                               uint32_t seqno);
bool adreno_pm4_me_init(adreno_pm4 *pm4, adreno_iova_t pwrup_reglist_iova);

/*
 * Encode the A7xx submission preamble/epilogue used by a6xx_gpu.c
 * (a7xx_submit) for one indrect-buffer list.
 */
enum adreno_status adreno_pm4_emit_submit(adreno_pm4 *pm4,
                                          const adreno_submit_desc *desc);

/* Walk a fully built stream and validate packet lengths. */
enum adreno_status adreno_pm4_validate(const uint32_t *data, uint32_t dwords);

/* Deterministic CRC-32 (IEEE 802.3, reflected) for stream fingerprints. */
uint32_t adreno_crc32(const void *data, size_t size, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_PM4_H */
