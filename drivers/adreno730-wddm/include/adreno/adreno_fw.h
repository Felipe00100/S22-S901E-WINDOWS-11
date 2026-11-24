/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_FW_H
#define ADRENO_FW_H

#include "adreno_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADRENO_GMU_ITCM_BASE 0x00000000u
#define ADRENO_GMU_ITCM_SIZE 0x00004000u
#define ADRENO_GMU_DTCM_BASE 0x10004000u
#define ADRENO_GMU_DTCM_SIZE 0x00004000u

#define ADRENO_FW_MAX_BLOCKS 512u

typedef enum adreno_fw_region {
    ADRENO_FW_REGION_TOC = 0, /* size == 0: metadata, never loaded */
    ADRENO_FW_REGION_ITCM = 1,
    ADRENO_FW_REGION_DTCM = 2,
    ADRENO_FW_REGION_HOST = 3, /* cache/dummy BO, matched by IOVA at load */
} adreno_fw_region;

typedef struct adreno_fw_block {
    uint32_t addr;
    uint32_t size;
    uint32_t type;
    uint32_t value;
    uint32_t data_offset;
    adreno_fw_region region;
} adreno_fw_block;

typedef struct adreno_fw_image {
    const uint8_t *data;
    size_t size;
    uint32_t count;
    uint32_t itcm_bytes;
    uint32_t dtcm_bytes;
    uint32_t host_bytes;
    uint32_t zero_blocks;
    uint32_t trailing_bytes;
    adreno_fw_block blocks[ADRENO_FW_MAX_BLOCKS];
} adreno_fw_image;

typedef struct adreno_fw_sqe {
    size_t size;
    uint32_t dwords;
    uint32_t crc32;
    uint32_t first_dword;
} adreno_fw_sqe;

/*
 * Parse the block container used by gmu_gen70000.bin (struct block_header in
 * a6xx_gmu.c). All fields are little endian, 16-byte header + payload.
 */
enum adreno_status adreno_fw_parse_gmu(const void *data, size_t size,
                                       adreno_fw_image *out);

/* Validate the raw SQE microcode blob (a730_sqe.fw). */
enum adreno_status adreno_fw_parse_sqe(const void *data, size_t size,
                                       adreno_fw_sqe *out);

/* RFC 6234 SHA-256, used for reproducible firmware manifests. */
typedef struct adreno_sha256 {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    uint32_t used;
} adreno_sha256;

void adreno_sha256_init(adreno_sha256 *ctx);
void adreno_sha256_update(adreno_sha256 *ctx, const void *data, size_t size);
void adreno_sha256_final(adreno_sha256 *ctx, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_FW_H */
