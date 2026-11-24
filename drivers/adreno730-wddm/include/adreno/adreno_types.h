/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_TYPES_H
#define ADRENO_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t adreno_reg_t;
typedef uint64_t adreno_iova_t;

#define ADRENO_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ADRENO_BIT(n) (1u << (n))

#define ADRENO_ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ADRENO_ALIGN_DOWN(v, a) ((v) & ~((uint64_t)(a) - 1))

enum adreno_status {
    ADRENO_OK = 0,
    ADRENO_ERR_INVALID = 1,
    ADRENO_ERR_NOMEM = 2,
    ADRENO_ERR_RANGE = 3,
    ADRENO_ERR_STATE = 4,
    ADRENO_ERR_OVERFLOW = 5,
    ADRENO_ERR_FIRMWARE = 6,
    ADRENO_ERR_IO = 7,
    ADRENO_ERR_TIMEOUT = 8,
    ADRENO_ERR_UNSUPPORTED = 9,
    ADRENO_ERR_CHECKSUM = 10,
};

static inline const char *adreno_status_str(enum adreno_status st)
{
    switch (st) {
    case ADRENO_OK: return "ok";
    case ADRENO_ERR_INVALID: return "invalid argument";
    case ADRENO_ERR_NOMEM: return "out of memory";
    case ADRENO_ERR_RANGE: return "out of range";
    case ADRENO_ERR_STATE: return "invalid state";
    case ADRENO_ERR_OVERFLOW: return "overflow";
    case ADRENO_ERR_FIRMWARE: return "bad firmware";
    case ADRENO_ERR_IO: return "i/o error";
    case ADRENO_ERR_TIMEOUT: return "timeout";
    case ADRENO_ERR_UNSUPPORTED: return "unsupported";
    case ADRENO_ERR_CHECKSUM: return "checksum mismatch";
    }
    return "unknown";
}

static inline bool adreno_is_pow2_u32(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_TYPES_H */
