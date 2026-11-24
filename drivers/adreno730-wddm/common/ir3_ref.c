/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_ir3_ref.h"

#include <string.h>

const uint8_t adreno_ir3_test_shader[ADRENO_IR3_TEST_SHADER_BYTES] = {
    0x00, 0x00, 0x00, 0x00, 0x04, 0xc0, 0x2c, 0x20, 0x04, 0x00, 0x00, 0x00,
    0x08, 0xc0, 0x2c, 0x20, 0x10, 0x00, 0x80, 0x01, 0x00, 0x09, 0xc6, 0xc0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

void adreno_ir3_ref_reset(adreno_ir3_ref_state *state)
{
    if (state)
        memset(state, 0, sizeof(*state));
}

enum adreno_status adreno_ir3_ref_load_shader(adreno_ir3_ref_state *state,
                                              uint64_t iova, uint32_t units,
                                              adreno_ir3_ref_read read,
                                              void *ctx)
{
    uint32_t bytes;

    if (!state || !read || units == 0)
        return ADRENO_ERR_INVALID;
    bytes = units * ADRENO_IR3_TEST_SHADER_BYTES;
    if (bytes > ADRENO_IR3_REF_MAX_SHADER_BYTES)
        return ADRENO_ERR_RANGE;
    if (read(ctx, iova, state->shader, bytes) != ADRENO_OK)
        return ADRENO_ERR_RANGE;
    state->shader_bytes = bytes;
    return ADRENO_OK;
}

enum adreno_status adreno_ir3_ref_load_const(adreno_ir3_ref_state *state,
                                             uint32_t dst_vec4,
                                             uint32_t units,
                                             const uint32_t *data)
{
    uint32_t words;

    if (!state || !data || units == 0 ||
        dst_vec4 + units > ADRENO_IR3_REF_MAX_CONST_VEC4)
        return ADRENO_ERR_INVALID;
    words = units * 4u;
    memcpy(&state->constants[dst_vec4 * 4u], data, words * 4u);
    if (dst_vec4 + units > state->const_vec4s)
        state->const_vec4s = dst_vec4 + units;
    return ADRENO_OK;
}

const uint8_t
    adreno_ir3_ssbo_test_shader[ADRENO_IR3_SSBO_TEST_SHADER_BYTES] = {
    0x04, 0x00, 0x00, 0x00, 0x00, 0x40, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x01, 0xc0, 0x2c, 0x20, 0x01, 0x00, 0x00, 0x00, 0x02, 0xc0, 0x2c, 0x20,
    0x01, 0x00, 0x00, 0x00, 0x02, 0xc0, 0x2c, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x03, 0xc6, 0xc0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xc2, 0xfb, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xca, 0xe3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const char *adreno_ir3_ref_name(void)
{
    return "a730-stg-c1";
}

enum adreno_status adreno_ir3_ref_exec_test(
    const uint8_t *shader, uint32_t shader_bytes, const uint32_t *constants,
    uint32_t const_vec4s, const uint32_t grid[3],
    adreno_ir3_ref_write32 write32, void *ctx)
{
    uint32_t regs[64 * 4] = {0};
    uint32_t stores = 0;
    bool ended = false;
    uint32_t i;

    if (!shader || !constants || !grid || !write32 || (shader_bytes & 7u))
        return ADRENO_ERR_INVALID;
    if (const_vec4s == 0 || grid[0] == 0 || grid[1] == 0 || grid[2] == 0)
        return ADRENO_ERR_INVALID;

    /*
     * Small IR3 interpreter for the compute subset emitted by adreno-ir3c:
     * cat1 MOV from the constant file, cat6 STG.u32 and cat0 NOP/END.
     * A single invocation is modeled; synchronization instructions have no
     * effect on the one-invocation test kernels.
     */
    for (i = 0; i < shader_bytes / 8u; i++) {
        const uint32_t *words = (const uint32_t *)(shader + i * 8u);
        uint64_t w = (uint64_t)words[0] | ((uint64_t)words[1] << 32);
        uint32_t cat = (uint32_t)((w >> 61) & 7u);

        if (cat == 1) {
            /* mov with a constant-file source (bits 53:54 == 01). */
            if (((w >> 57) & 3u) != 0 || ((w >> 53) & 3u) != 1)
                return ADRENO_ERR_UNSUPPORTED;
            uint32_t dst = (uint32_t)((w >> 32) & 0xffu);
            uint32_t src = (uint32_t)(w & 0x3ffu);
            uint32_t cvec = src >> 2;
            uint32_t cswz = src & 3u;

            if (cvec >= const_vec4s)
                return ADRENO_ERR_RANGE;
            regs[(dst >> 2) * 4u + (dst & 3u)] =
                constants[cvec * 4u + cswz];
        } else if (cat == 6) {
            uint32_t opc = (uint32_t)((w >> 54) & 0x1fu);
            uint32_t type = (uint32_t)((w >> 49) & 7u);
            uint32_t size = (uint32_t)((w >> 24) & 7u);
            uint32_t src1 = (uint32_t)((w >> 41) & 0xffu);
            uint32_t src3 = (uint32_t)((w >> 1) & 0xffu);
            uint32_t base_idx;
            uint32_t off;
            uint64_t addr;
            enum adreno_status st;

            if (opc != 3 || type != 3 || size != 1)
                return ADRENO_ERR_UNSUPPORTED;
            base_idx = (src1 >> 2) * 4u + (src1 & 3u);
            if (base_idx + 1u >= 64u * 4u)
                return ADRENO_ERR_RANGE;
            off = ((uint32_t)((w >> 9) & 0x1fu) << 8) |
                  (uint32_t)((w >> 32) & 0xffu);
            if (off & 0x1000u)
                off |= 0xffffe000u;
            addr = (uint64_t)regs[base_idx] |
                   ((uint64_t)regs[base_idx + 1u] << 32);
            st = write32(ctx, addr + off,
                         regs[(src3 >> 2) * 4u + (src3 & 3u)]);
            if (st != ADRENO_OK)
                return st;
            stores++;
        } else if (cat == 0) {
            uint32_t opc = (uint32_t)((w >> 55) & 0xfu);

            if (opc == 6) {
                ended = true;
                break;
            }
        }
    }

    if (!ended || stores == 0)
        return ADRENO_ERR_UNSUPPORTED;
    return ADRENO_OK;
}

enum adreno_status adreno_ir3_ref_exec_state(
    const adreno_ir3_ref_state *state, const uint32_t grid[3],
    adreno_ir3_ref_write32 write32, void *ctx)
{
    if (!state)
        return ADRENO_ERR_INVALID;
    return adreno_ir3_ref_exec_test(state->shader, state->shader_bytes,
                                    state->constants, state->const_vec4s,
                                    grid, write32, ctx);
}
