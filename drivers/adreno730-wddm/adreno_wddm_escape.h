/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_WDDM_ESCAPE_H
#define ADRENO_WDDM_ESCAPE_H

#include <stdint.h>

#define ADRENO_WDDM_ESC_MAGIC 0x41373330u /* 'A730' */
#define ADRENO_WDDM_ESC_VERSION 1u
#define ADRENO_WDDM_ESC_MAX_IBS 64u

enum adreno_wddm_esc_cmd {
    ADRENO_WDDM_ESC_GET_INFO      = 1,
    ADRENO_WDDM_ESC_SUBMIT_IBS    = 2,
    ADRENO_WDDM_ESC_WAIT_FENCE    = 3,
    ADRENO_WDDM_ESC_GET_TIMESTAMP = 4,
};

typedef struct adreno_wddm_esc_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t cmd;
    uint32_t status; /* 0 = OK, nonzero = error */
} adreno_wddm_esc_hdr;

typedef struct adreno_wddm_esc_info {
    adreno_wddm_esc_hdr hdr;
    uint32_t chip_id;          /* 0x07030001 (A730) */
    uint32_t gmem_size;        /* 2 MiB */
    uint64_t gmem_base;        /* 0x0c000000 */
    uint64_t va_start;         /* 0x20000000 */
    uint64_t va_size;          /* 0x30000000 */
    uint32_t highest_bank_bit; /* 16 */
    uint32_t ubwc_swizzle;     /* 0x6 */
    uint32_t macrotile_mode;   /* 1 */
    uint32_t hw_ready;
    uint32_t cp_started;
    uint32_t last_fence;
} adreno_wddm_esc_info;

typedef struct adreno_wddm_esc_ib {
    uint64_t iova;
    uint32_t dwords;
    uint32_t flags;
} adreno_wddm_esc_ib;

typedef struct adreno_wddm_esc_submit {
    adreno_wddm_esc_hdr hdr;
    uint32_t num_ibs;
    uint32_t timeout_ms; /* 0 = async, >0 = wait up to timeout_ms */
    uint32_t out_fence;
    uint32_t completed;
    adreno_wddm_esc_ib ibs[ADRENO_WDDM_ESC_MAX_IBS];
} adreno_wddm_esc_submit;

typedef struct adreno_wddm_esc_wait {
    adreno_wddm_esc_hdr hdr;
    uint32_t fence;
    uint32_t timeout_ms;
    uint32_t current_fence;
    uint32_t completed;
} adreno_wddm_esc_wait;

typedef struct adreno_wddm_esc_timestamp {
    adreno_wddm_esc_hdr hdr;
    uint64_t ticks;
    uint64_t frequency_hz;
} adreno_wddm_esc_timestamp;

#endif /* ADRENO_WDDM_ESCAPE_H */
