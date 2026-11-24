/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_fw.h"
#include "adreno/adreno_pm4.h"

#include <string.h>

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

enum adreno_status adreno_fw_parse_gmu(const void *data, size_t size,
                                       adreno_fw_image *out)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;

    if (!data || !out)
        return ADRENO_ERR_INVALID;
    if (size < 16 || size > (16u * 1024u * 1024u))
        return ADRENO_ERR_FIRMWARE;

    memset(out, 0, sizeof(*out));
    out->data = p;
    out->size = size;

    while (off + 16 <= size) {
        uint32_t addr = rd32le(p + off);
        uint32_t bsize = rd32le(p + off + 4);
        uint32_t type = rd32le(p + off + 8);
        uint32_t value = rd32le(p + off + 12);
        adreno_fw_block *blk;

        if (bsize > 4u * 1024u * 1024u)
            return ADRENO_ERR_FIRMWARE;
        if (off + 16 + bsize > size)
            return ADRENO_ERR_FIRMWARE;
        if (out->count >= ADRENO_FW_MAX_BLOCKS)
            return ADRENO_ERR_OVERFLOW;

        blk = &out->blocks[out->count++];
        blk->addr = addr;
        blk->size = bsize;
        blk->type = type;
        blk->value = value;
        blk->data_offset = (uint32_t)(off + 16);

        if (bsize == 0) {
            blk->region = ADRENO_FW_REGION_TOC;
            out->zero_blocks++;
        } else if ((uint64_t)addr + bsize <=
                (uint64_t)ADRENO_GMU_ITCM_BASE + ADRENO_GMU_ITCM_SIZE &&
            addr >= ADRENO_GMU_ITCM_BASE) {
            blk->region = ADRENO_FW_REGION_ITCM;
            out->itcm_bytes += bsize;
        } else if ((uint64_t)addr + bsize <=
                       (uint64_t)ADRENO_GMU_DTCM_BASE + ADRENO_GMU_DTCM_SIZE &&
                   addr >= ADRENO_GMU_DTCM_BASE) {
            blk->region = ADRENO_FW_REGION_DTCM;
            out->dtcm_bytes += bsize;
        } else {
            blk->region = ADRENO_FW_REGION_HOST;
            out->host_bytes += bsize;
        }

        off += 16 + bsize;
    }

    out->trailing_bytes = (uint32_t)(size - off);
    if (out->trailing_bytes) {
        size_t i;
        for (i = 0; i < out->trailing_bytes; i++)
            if (p[off + i] != 0)
                return ADRENO_ERR_FIRMWARE;
        out->trailing_bytes = (uint32_t)(size - off);
    }
    return ADRENO_OK;
}

enum adreno_status adreno_fw_parse_sqe(const void *data, size_t size,
                                       adreno_fw_sqe *out)
{
    if (!data || !out || size < 1024 || size > (1024u * 1024u) ||
        (size & 3) != 0)
        return ADRENO_ERR_FIRMWARE;

    memset(out, 0, sizeof(*out));
    out->size = size;
    out->dwords = (uint32_t)(size / 4);
    out->crc32 = adreno_crc32(data, size, 0);
    out->first_dword = rd32le((const uint8_t *)data);
    return ADRENO_OK;
}

/* ---- SHA-256 ------------------------------------------------------------ */

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define EP1(x) (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define SIG0(x) (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static void sha256_transform(adreno_sha256 *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    for (i = 16; i < 64; i++)
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void adreno_sha256_init(adreno_sha256 *ctx)
{
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->used = 0;
}

void adreno_sha256_update(adreno_sha256 *ctx, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;

    while (size) {
        size_t n = 64 - ctx->used;
        if (n > size)
            n = size;
        memcpy(ctx->buffer + ctx->used, p, n);
        ctx->used += (uint32_t)n;
        ctx->bitlen += (uint64_t)n * 8;
        p += n;
        size -= n;
        if (ctx->used == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->used = 0;
        }
    }
}

void adreno_sha256_final(adreno_sha256 *ctx, uint8_t digest[32])
{
    uint64_t bitlen = ctx->bitlen;
    int i;

    ctx->buffer[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        while (ctx->used < 64)
            ctx->buffer[ctx->used++] = 0;
        sha256_transform(ctx, ctx->buffer);
        ctx->used = 0;
    }
    while (ctx->used < 56)
        ctx->buffer[ctx->used++] = 0;
    for (i = 7; i >= 0; i--)
        ctx->buffer[ctx->used++] = (uint8_t)(bitlen >> (i * 8));
    sha256_transform(ctx, ctx->buffer);

    for (i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}
