/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 *
 * Original Adreno 730 (A7xx) hardware 2D blit/clear and 3D sysmem direct-render
 * PM4 packet encoder for the S22 native DirectX 12 UMD.
 */
#include "adreno/adreno_a730_3d.h"

#include <string.h>

/* Extra A7xx PM4 opcodes & events not yet in a730_regs.h */
#define CP_SET_VISIBILITY_OVERRIDE     0x64u
#define CP_SKIP_IB2_ENABLE_GLOBAL      0x1du
#define ADRENO_RM6_DIRECT_RENDER       1u
#define ADRENO_RM6_BLIT2DSCALE         12u
#define ADRENO_CCU_FLUSH_COLOR         0x1du
#define ADRENO_CCU_INVALIDATE_COLOR    0x19u
#define ADRENO_BLIT_OP_SCALE           3u
#define ADRENO_BUFFERS_IN_SYSMEM       3u

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint32_t clamp_u8(float v)
{
    int32_t i;
    if (v <= 0.0f) return 0u;
    if (v >= 1.0f) return 255u;
    i = (int32_t)(v * 255.0f + 0.5f);
    if (i < 0) return 0u;
    if (i > 255) return 255u;
    return (uint32_t)i;
}

static uint32_t clamp_u16_unorm(float v)
{
    int32_t i;
    if (v <= 0.0f) return 0u;
    if (v >= 1.0f) return 65535u;
    i = (int32_t)(v * 65535.0f + 0.5f);
    if (i < 0) return 0u;
    if (i > 65535) return 65535u;
    return (uint32_t)i;
}

/* Emit A730 3D/2D hardware initialization chicken bits & CCU setup. */
static bool a730_emit_preamble(adreno_pm4 *pm4)
{
    static const uint32_t regs[][2] = {
        {REG_A6XX_UCHE_CACHE_WAYS,            0x00840004u},
        {REG_A6XX_TPL1_DBG_ECO_CNTL,          0x01000000u},
        {REG_A6XX_TPL1_DBG_ECO_CNTL1,         0x00040724u},
        {REG_A6XX_SP_CHICKEN_BITS,            0x00001400u},
        {REG_A7XX_SP_CHICKEN_BITS_1,          0x00402400u},
        {REG_A7XX_SP_CHICKEN_BITS_2,          0x00000000u},
        {REG_A7XX_SP_CHICKEN_BITS_3,          0x00000000u},
        {REG_A7XX_UCHE_UNKNOWN_0E10,          0x00000000u},
        {REG_A7XX_UCHE_UNKNOWN_0E11,          0x00000040u},
        {REG_A7XX_SP_HLSQ_DBG_ECO_CNTL,       0x00008000u},
        {REG_A6XX_SP_DBG_ECO_CNTL,            0x10000000u},
        {REG_A6XX_PC_MODE_CNTL,               0x00001f1fu},
        {REG_A6XX_PC_DBG_ECO_CNTL,            0x20080000u},
        {REG_A7XX_PC_UNKNOWN_9E24,            0x21fc7f00u},
        {REG_A7XX_VFD_DBG_ECO_CNTL,           0x00000000u},
        {REG_A7XX_SP_ISDB_CNTL,               0x00000000u},
        {REG_A7XX_SP_UNKNOWN_AE6A,            0x00000000u},
        {REG_A7XX_SP_HLSQ_TIMEOUT_THRESHOLD_DP, 0x00000080u},
        {REG_A7XX_SP_HLSQ_DBG_ECO_CNTL_1,     0x00000000u},
        {REG_A7XX_SP_HLSQ_MODE_CNTL,          0x00000000u},
        {REG_A7XX_SP_UNKNOWN_AB01,            0x00000000u},
        {REG_A7XX_SP_UNKNOWN_AB22,            0x00000000u},
        {REG_A7XX_SP_UNKNOWN_B310,            0x00000000u},
        {REG_A7XX_GRAS_ROTATION_CNTL,         0x00000000u},
        {REG_A6XX_GRAS_DBG_ECO_CNTL,          0x00000800u},
        {REG_A6XX_RB_RBP_CNTL,                0x00000000u},
        {REG_A7XX_RB_UNKNOWN_8E79,            0x00000000u},
        {REG_A7XX_RB_LRZ_CNTL2,               0x00000000u},
        {REG_A7XX_RB_CCU_DBG_ECO_CNTL,        0x02080000u},
        {REG_A6XX_VPC_DBG_ECO_CNTL,           0x02000000u},
        {REG_A6XX_UCHE_UNKNOWN_0E12,          0x03200000u},
        {REG_A6XX_RB_DBG_ECO_CNTL,            0x00000000u},
        {REG_A6XX_SP_MODE_CNTL,               0x00000005u},
        {REG_A6XX_TPL1_MODE_CNTL,             0x00000000u},
        {REG_A6XX_RB_MODE_CNTL,               0x00000000u},
        {REG_A6XX_GRAS_MODE_CNTL,             0x00000002u},
        {REG_A7XX_SP_UPDATE_CNTL,             0x000000bfu},
        {REG_A7XX_RB_CCU_CNTL,                0x00000000u},
    };
    uint32_t i;

    /* Switch to BR pipe with concurrent binning disabled for direct sysmem execution. */
    if (!adreno_pm4_thread_control(pm4, 0x08000001u))
        return false;
    if (!adreno_pm4_wait_for_idle(pm4))
        return false;
    for (i = 0; i < ADRENO_ARRAY_SIZE(regs); i++) {
        if (!adreno_pm4_reg_write(pm4, regs[i][0], regs[i][1]))
            return false;
    }
    return true;
}

static bool a730_emit_load_state6_geom_shader(adreno_pm4 *pm4,
                                              uint32_t state_block,
                                              adreno_iova_t iova,
                                              uint32_t units)
{
    uint32_t ctrl = ((uint32_t)ADRENO_ST6_SHADER << 14) |
                    ((uint32_t)ADRENO_SS6_INDIRECT << 16) |
                    ((state_block & 0xfu) << 18) |
                    ((units & 0x3ffu) << 22);
    return adreno_pm4_emit_pkt7(pm4, CP_LOAD_STATE6_GEOM, 3) &&
           adreno_pm4_emit(pm4, ctrl) &&
           adreno_pm4_emit(pm4, (uint32_t)iova) &&
           adreno_pm4_emit(pm4, (uint32_t)(iova >> 32));
}

static bool a730_emit_load_state6_geom_const(adreno_pm4 *pm4,
                                             uint32_t state_block,
                                             uint32_t dst_vec4,
                                             const uint32_t *data,
                                             uint32_t vec4s)
{
    uint32_t ctrl, i;
    if (!data || vec4s == 0)
        return true;
    ctrl = (dst_vec4 & 0x3fffu) |
           ((uint32_t)ADRENO_ST6_CONSTANTS << 14) |
           ((uint32_t)ADRENO_SS6_DIRECT << 16) |
           ((state_block & 0xfu) << 18) |
           ((vec4s & 0x3ffu) << 22);
    if (!adreno_pm4_emit_pkt7(pm4, CP_LOAD_STATE6_GEOM, 3u + vec4s * 4u) ||
        !adreno_pm4_emit(pm4, ctrl) ||
        !adreno_pm4_emit(pm4, 0u) ||
        !adreno_pm4_emit(pm4, 0u))
        return false;
    for (i = 0; i < vec4s * 4u; i++) {
        if (!adreno_pm4_emit(pm4, data[i]))
            return false;
    }
    return true;
}

static bool a730_emit_flush_color(adreno_pm4 *pm4)
{
    return adreno_pm4_event_write_1(pm4, ADRENO_CCU_FLUSH_COLOR) &&
           adreno_pm4_event_write_1(pm4, CACHE_CLEAN) &&
           adreno_pm4_wait_for_idle(pm4);
}

/* ---- A730 2D Hardware Solid Clear (CP_BLIT) ---------------------------- */

bool adreno_a730_emit_clear_2d(adreno_pm4 *pm4, const adreno_a730_rtv *rtv,
                               const float rgba[4])
{
    uint32_t blt_cntl, dst_info, sp_out;
    uint32_t clear_dw[4] = {0, 0, 0, 0};

    if (!pm4 || !rtv || !rgba || rtv->width == 0 || rtv->height == 0 ||
        !rtv->iova || rtv->pitch == 0)
        return false;

    if (!a730_emit_preamble(pm4))
        return false;

    /* Set 2D blit render marker and flush/invalidate CCU color. */
    if (!adreno_pm4_set_marker(pm4, ADRENO_RM6_BLIT2DSCALE) ||
        !adreno_pm4_event_write_1(pm4, ADRENO_CCU_FLUSH_COLOR) ||
        !adreno_pm4_event_write_1(pm4, ADRENO_CCU_INVALIDATE_COLOR) ||
        !adreno_pm4_wait_for_idle(pm4))
        return false;

    /* R2D_UNORM8 expects 16-bit normalized values per channel in DW0..DW3. */
    if (rtv->fmt == ADRENO_FMT_R32_FLOAT ||
        rtv->fmt == ADRENO_FMT_R32G32B32A32_FLOAT) {
        clear_dw[0] = f2u(rgba[0]);
        clear_dw[1] = f2u(rgba[1]);
        clear_dw[2] = f2u(rgba[2]);
        clear_dw[3] = f2u(rgba[3]);
    } else {
        /* R2D_UNORM8: 8-bit normalized channels DW0..DW3 = R,G,B,A (HW applies swap). */
        clear_dw[0] = clamp_u8(rgba[0]);
        clear_dw[1] = clamp_u8(rgba[1]);
        clear_dw[2] = clamp_u8(rgba[2]);
        clear_dw[3] = clamp_u8(rgba[3]);
        (void)clamp_u16_unorm;
    }

    /* SOLID_COLOR (bit 7) | COLOR_FORMAT (15:8) | MASK=0xF (23:20) | IFMT (26:24) */
    blt_cntl = (1u << 7) |
               ((rtv->fmt & 0xffu) << 8) |
               (0xfu << 20) |
               (((rtv->fmt == ADRENO_FMT_R32_FLOAT ||
                  rtv->fmt == ADRENO_FMT_R32G32B32A32_FLOAT) ? 4u : 0u) << 24);

    dst_info = (rtv->fmt & 0xffu) |
               (0u << 8) | /* TILE6_LINEAR */
               ((rtv->swap & 3u) << 10);

    sp_out = (0u << 0) | /* full precision */
             (0u << 1) | /* OUTPUT_IFMT_2D_FLOAT */
             ((rtv->fmt & 0xffu) << 3) |
             (0xfu << 12);

    if (!adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_PIXEL_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_BLT_CNTL, blt_cntl) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_BLT_CNTL, blt_cntl) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_TPL1_A2D_BLT_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_A2D_OUTPUT_INFO, sp_out) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_DEST_BUFFER_INFO, dst_info) ||
        !adreno_pm4_reg_write64(pm4, REG_A6XX_RB_A2D_DEST_BUFFER_BASE, rtv->iova) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_DEST_BUFFER_PITCH, rtv->pitch / 64u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_RB_A2D_DEST_BUFFER_ARRAY_PITCH, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_CLEAR_COLOR_DW0, clear_dw[0]) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_CLEAR_COLOR_DW1, clear_dw[1]) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_CLEAR_COLOR_DW2, clear_dw[2]) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_CLEAR_COLOR_DW3, clear_dw[3]) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_DEST_TL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_DEST_BR,
                              ((rtv->width - 1u) & 0x3fffu) |
                              (((rtv->height - 1u) & 0x3fffu) << 16)) ||
        !adreno_pm4_wait_for_idle(pm4) ||
        !adreno_pm4_emit_pkt7(pm4, CP_BLIT, 1) ||
        !adreno_pm4_emit(pm4, ADRENO_BLIT_OP_SCALE) ||
        !a730_emit_flush_color(pm4))
        return false;

    return true;
}

/* ---- A730 2D Hardware Rectangle Copy (CP_BLIT) ------------------------- */

bool adreno_a730_emit_copy_2d(adreno_pm4 *pm4,
                              const adreno_a730_rtv *dst, uint32_t dst_x, uint32_t dst_y,
                              const adreno_a730_rtv *src, uint32_t src_x, uint32_t src_y,
                              uint32_t copy_w, uint32_t copy_h)
{
    uint32_t blt_cntl, dst_info, src_info, sp_out;

    if (!pm4 || !dst || !src || copy_w == 0 || copy_h == 0 ||
        !dst->iova || !src->iova)
        return false;

    if (!a730_emit_preamble(pm4))
        return false;

    if (!adreno_pm4_set_marker(pm4, ADRENO_RM6_BLIT2DSCALE) ||
        !adreno_pm4_event_write_1(pm4, ADRENO_CCU_FLUSH_COLOR) ||
        !adreno_pm4_event_write_1(pm4, ADRENO_CCU_INVALIDATE_COLOR) ||
        !adreno_pm4_wait_for_idle(pm4))
        return false;

    blt_cntl = ((dst->fmt & 0xffu) << 8) | (0xfu << 20);
    dst_info = (dst->fmt & 0xffu) | ((dst->swap & 3u) << 10);
    src_info = (src->fmt & 0xffu) | ((src->swap & 3u) << 10);
    sp_out   = ((dst->fmt & 0xffu) << 3) | (0xfu << 12);

    if (!adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_PIXEL_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_BLT_CNTL, blt_cntl) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_BLT_CNTL, blt_cntl) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_TPL1_A2D_BLT_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_A2D_OUTPUT_INFO, sp_out) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_TPL1_A2D_SRC_TEXTURE_INFO, src_info) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_TPL1_A2D_SRC_TEXTURE_SIZE,
                              (src->width & 0x7fffu) | ((src->height & 0x7fffu) << 15)) ||
        !adreno_pm4_reg_write64(pm4, REG_A7XX_TPL1_A2D_SRC_TEXTURE_BASE, src->iova) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_TPL1_A2D_SRC_TEXTURE_PITCH, src->pitch << 3) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_DEST_BUFFER_INFO, dst_info) ||
        !adreno_pm4_reg_write64(pm4, REG_A6XX_RB_A2D_DEST_BUFFER_BASE, dst->iova) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_A2D_DEST_BUFFER_PITCH, dst->pitch / 64u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_RB_A2D_DEST_BUFFER_ARRAY_PITCH, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_SRC_XMIN, src_x) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_SRC_XMAX, src_x + copy_w - 1u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_SRC_YMIN, src_y) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_SRC_YMAX, src_y + copy_h - 1u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_DEST_TL,
                              (dst_x & 0x3fffu) | ((dst_y & 0x3fffu) << 16)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_A2D_DEST_BR,
                              ((dst_x + copy_w - 1u) & 0x3fffu) |
                              (((dst_y + copy_h - 1u) & 0x3fffu) << 16)) ||
        !adreno_pm4_wait_for_idle(pm4) ||
        !adreno_pm4_emit_pkt7(pm4, CP_BLIT, 1) ||
        !adreno_pm4_emit(pm4, ADRENO_BLIT_OP_SCALE) ||
        !a730_emit_flush_color(pm4))
        return false;

    return true;
}

/* ---- A730 TPL1 Texture & Sampler Descriptor Encoders ------------------- */

void adreno_a730_encode_tex_memobj(const adreno_a730_tex_desc *t, uint32_t out_dw[16])
{
    uint32_t array_pitch_4k;

    memset(out_dw, 0, 16u * sizeof(uint32_t));
    if (!t || !t->iova || t->width == 0 || t->height == 0)
        return;

    /* DW0: TILE6_LINEAR(0) | SRGB(bit2) | SWIZ X/Y/Z/W=(0,1,2,3) | MIPLVLS=0 | SAMPLES=0 | FMT(29:22) | SWAP(31:30) */
    out_dw[0] = (0u << 0) |
                (t->srgb ? (1u << 2) : 0u) |
                (0u << 4) | (1u << 7) | (2u << 10) | (3u << 13) |
                ((t->fmt & 0xffu) << 22) |
                ((t->swap & 3u) << 30);
    /* DW1: WIDTH(14:0) | HEIGHT(29:15) */
    out_dw[1] = (t->width & 0x7fffu) | ((t->height & 0x7fffu) << 15);
    /* DW2: PITCHALIGN(3:0)=0 (64B) | PITCH(28:7) in bytes | TYPE(31:29)=1 (A6XX_TEX_2D) */
    out_dw[2] = ((t->pitch & 0x3fffffu) << 7) | (1u << 29);
    /* DW3: ARRAY_PITCH(22:0) in 4KB units */
    array_pitch_4k = (uint32_t)(((uint64_t)t->pitch * t->height + 4095ull) >> 12);
    out_dw[3] = array_pitch_4k & 0x7fffffu;
    /* DW4: BASE_LO (bits 31:5) */
    out_dw[4] = (uint32_t)(t->iova & 0xffffffe0ull);
    /* DW5: BASE_HI (bits 16:0) | DEPTH(29:17)=1 */
    out_dw[5] = ((uint32_t)(t->iova >> 32) & 0x1ffffu) | (1u << 17);
}

void adreno_a730_encode_tex_samp(const adreno_a730_samp_desc *s, uint32_t out_dw[4])
{
    uint32_t mag = (s && s->mag_linear) ? 1u : 0u;
    uint32_t min = (s && s->min_linear) ? 1u : 0u;
    uint32_t mip = (s && s->mip_linear) ? 1u : 0u;
    uint32_t wu  = s ? (s->wrap_u & 7u) : ADRENO_TEX_WRAP_CLAMP_TO_EDGE;
    uint32_t wv  = s ? (s->wrap_v & 7u) : ADRENO_TEX_WRAP_CLAMP_TO_EDGE;
    uint32_t ww  = s ? (s->wrap_w & 7u) : ADRENO_TEX_WRAP_CLAMP_TO_EDGE;

    out_dw[0] = (mip << 0) | (mag << 1) | (min << 3) |
                (wu << 5) | (wv << 8) | (ww << 11);
    /* Normalized coordinates (0..1), clamp LOD to [0, 0] for single-mip textures */
    out_dw[1] = 0u;
    out_dw[2] = 0u;
    out_dw[3] = 0u;
}

static bool a730_emit_load_state6_frag_indirect(adreno_pm4 *pm4,
                                                uint32_t state_type,
                                                uint32_t state_block,
                                                adreno_iova_t iova,
                                                uint32_t num_units)
{
    uint32_t ctrl = ((state_type & 3u) << 14) |
                    (2u << 16) | /* SS6_INDIRECT */
                    ((state_block & 0xfu) << 18) |
                    ((num_units & 0x3ffu) << 22);
    return adreno_pm4_emit_pkt7(pm4, CP_LOAD_STATE6_FRAG, 3) &&
           adreno_pm4_emit(pm4, ctrl) &&
           adreno_pm4_emit(pm4, (uint32_t)iova) &&
           adreno_pm4_emit(pm4, (uint32_t)(iova >> 32));
}

/* ---- A730 3D Hardware Sysmem Direct-Render Draw ------------------------ */

bool adreno_a730_emit_draw_3d(adreno_pm4 *pm4, const adreno_a730_draw_desc *d)
{
    uint32_t i;
    uint32_t bin_cntl;
    uint32_t vs_cntl0, ps_cntl0, ps_cntl1;
    uint32_t total_varyings_scalars = 0;
    uint32_t pos_loc, stride_in_vpc;
    uint32_t out_reg_count = 0;
    uint32_t out_regids[16];
    uint32_t out_masks[16];
    uint32_t out_locs[16];
    uint32_t mrt_ctrl, mrt_blend, mrt_buf_info;
    uint32_t draw_init;
    float xscale, xoffset, yscale, yoffset, zscale, zoffset;
    uint32_t sc_x0, sc_y0, sc_x1, sc_y1;
    bool has_varyings;

    if (!pm4 || !d || !d->rtv.iova || d->rtv.width == 0 || d->rtv.height == 0 ||
        !d->vs.iova || !d->ps.iova || d->vertex_count == 0 || d->instance_count == 0)
        return false;

    if (!a730_emit_preamble(pm4))
        return false;

    /* 1. Sysmem Direct-Render pass setup */
    bin_cntl = (0u << 18) |                      /* RENDERING_PASS */
               (1u << 21) |                      /* FORCE_LRZ_WRITE_DIS */
               (ADRENO_BUFFERS_IN_SYSMEM << 22) |/* BUFFERS_IN_SYSMEM */
               (1u << 27);                       /* FORCE_LRZ_DIS */

    if (!adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR,
                              ((d->rtv.width - 1u) & 0x3fffu) |
                              (((d->rtv.height - 1u) & 0x3fffu) << 16)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_WINDOW_OFFSET, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_WINDOW_OFFSET, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_TPL1_WINDOW_OFFSET, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SC_BIN_CNTL, bin_cntl) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_CNTL, bin_cntl) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_RB_BUFFER_CNTL, 0x000003ffu) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_RB_CLEAR_TARGET, 0x00000001u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_GRAS_BIN_FOVEAT, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_RB_BIN_FOVEAT, 0u) ||
        !adreno_pm4_set_marker(pm4, ADRENO_RM6_DIRECT_RENDER) ||
        !adreno_pm4_emit_pkt7(pm4, CP_SKIP_IB2_ENABLE_GLOBAL, 1) ||
        !adreno_pm4_emit(pm4, 0u) ||
        !adreno_pm4_event_write_1(pm4, ADRENO_CCU_FLUSH_COLOR) ||
        !adreno_pm4_event_write_1(pm4, ADRENO_CCU_INVALIDATE_COLOR) ||
        !adreno_pm4_event_write_1(pm4, CACHE_INVALIDATE7) ||
        !adreno_pm4_wait_for_idle(pm4) ||
        !adreno_pm4_emit_pkt7(pm4, CP_SET_VISIBILITY_OVERRIDE, 1) ||
        !adreno_pm4_emit(pm4, 1u) ||
        !adreno_pm4_emit_pkt7(pm4, CP_SET_MODE, 1) ||
        !adreno_pm4_emit(pm4, 0u))
        return false;

    /* 2. Viewport, Scissor, Rasterizer & MSAA (1x) */
    xscale  = d->vp_w * 0.5f;
    xoffset = d->vp_x + xscale;
    /* D3D12 NDC +Y is UP, framebuffer +Y is DOWN -> negate yscale! */
    yscale  = -d->vp_h * 0.5f;
    yoffset = d->vp_y + d->vp_h * 0.5f;
    zscale  = d->vp_max_z - d->vp_min_z;
    zoffset = d->vp_min_z;

    sc_x0 = d->scissor_x0;
    sc_y0 = d->scissor_y0;
    sc_x1 = (d->scissor_x1 > sc_x0 && d->scissor_x1 <= d->rtv.width)  ? (d->scissor_x1 - 1u) : (d->rtv.width - 1u);
    sc_y1 = (d->scissor_y1 > sc_y0 && d->scissor_y1 <= d->rtv.height) ? (d->scissor_y1 - 1u) : (d->rtv.height - 1u);

    if (!adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_CL_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_CL_VS_CLIP_CULL_DISTANCE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_CL_DS_CLIP_CULL_DISTANCE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_CL_GS_CLIP_CULL_DISTANCE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_CL_ARRAY_SIZE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ, 0u) ||
        !adreno_pm4_emit_pkt4(pm4, REG_A6XX_GRAS_CL_VIEWPORT(0), 6) ||
        !adreno_pm4_emit(pm4, f2u(xoffset)) ||
        !adreno_pm4_emit(pm4, f2u(xscale)) ||
        !adreno_pm4_emit(pm4, f2u(yoffset)) ||
        !adreno_pm4_emit(pm4, f2u(yscale)) ||
        !adreno_pm4_emit(pm4, f2u(zoffset)) ||
        !adreno_pm4_emit(pm4, f2u(zscale)) ||
        !adreno_pm4_emit_pkt4(pm4, REG_A6XX_GRAS_CL_VIEWPORT_ZCLAMP(0), 2) ||
        !adreno_pm4_emit(pm4, f2u(d->vp_min_z)) ||
        !adreno_pm4_emit(pm4, f2u(d->vp_max_z)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_VIEWPORT_ZCLAMP_MIN, f2u(d->vp_min_z)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_VIEWPORT_ZCLAMP_MAX, f2u(d->vp_max_z)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SU_CNTL, (1u << 2) /* FRONT_CW (D3D12) */) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_GRAS_SU_RENDER_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SU_DEPTH_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SU_STENCIL_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SC_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SC_RAS_MSAA_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SC_DEST_MSAA_CNTL, 0x4u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_SC_SCREEN_SCISSOR_CNTL, 0u) ||
        !adreno_pm4_emit_pkt4(pm4, REG_A6XX_GRAS_SC_SCREEN_SCISSOR(0), 2) ||
        !adreno_pm4_emit(pm4, (sc_x0 & 0x3fffu) | ((sc_y0 & 0x3fffu) << 16)) ||
        !adreno_pm4_emit(pm4, (sc_x1 & 0x3fffu) | ((sc_y1 & 0x3fffu) << 16)) ||
        !adreno_pm4_emit_pkt4(pm4, REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR(0), 2) ||
        !adreno_pm4_emit(pm4, (sc_x0 & 0x3fffu) | ((sc_y0 & 0x3fffu) << 16)) ||
        !adreno_pm4_emit(pm4, (sc_x1 & 0x3fffu) | ((sc_y1 & 0x3fffu) << 16)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_LRZ_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_GRAS_LRZ_CNTL2, 0u))
        return false;

    /* 3. Render Backend (RB) MRT0 + Depth/Stencil disabled */
    mrt_ctrl = (0xfu << 7) | (d->blend_enable ? 3u : 0u);
    /* Standard alpha blend: SrcAlpha / InvSrcAlpha */
    mrt_blend = (4u << 0) | (0u << 5) | (5u << 8) |
                (1u << 16) | (0u << 21) | (5u << 24);
    mrt_buf_info = (d->rtv.fmt & 0xffu) |
                   (0u << 8) | /* TILE6_LINEAR */
                   ((d->rtv.swap & 3u) << 13);

    if (!adreno_pm4_reg_write(pm4, REG_A7XX_RB_RENDER_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_RAS_MSAA_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_DEST_MSAA_CNTL, 0x4u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_DEPTH_PLANE_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_DEPTH_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_STENCIL_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_LRZ_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_ALPHA_TEST_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_BLEND_CNTL,
                              (d->blend_enable ? 1u : 0u) | (0xfu << 16)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_BLEND_CNTL,
                              (d->blend_enable ? 1u : 0u)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_SRGB_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_SRGB_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_DITHER_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_DITHER_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_TPL1_RAS_MSAA_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_TPL1_DEST_MSAA_CNTL, 0x4u) ||
        !adreno_pm4_emit_pkt4(pm4, REG_A6XX_RB_MRT(0), 8) ||
        !adreno_pm4_emit(pm4, mrt_ctrl) ||
        !adreno_pm4_emit(pm4, mrt_blend) ||
        !adreno_pm4_emit(pm4, mrt_buf_info) ||
        !adreno_pm4_emit(pm4, d->rtv.pitch / 64u) || /* RB_MRT_PITCH is in 64-byte units! */
        !adreno_pm4_emit(pm4, 0u) ||                /* ARRAY_PITCH */
        !adreno_pm4_emit(pm4, (uint32_t)d->rtv.iova) ||
        !adreno_pm4_emit(pm4, (uint32_t)(d->rtv.iova >> 32)) ||
        !adreno_pm4_emit(pm4, 0u))                  /* BASE_GMEM */
        return false;

    /* 4. Vertex Fetch (VFD) */
    if (!adreno_pm4_reg_write(pm4, REG_A6XX_VFD_RENDER_MODE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_STEREO_RENDERING_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_INDEX_OFFSET, d->start_vertex) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_INSTANCE_START_OFFSET, d->start_instance) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_CNTL_0,
                              (d->num_vattrs & 0x3fu) | ((d->num_vattrs & 0x3fu) << 8)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_CNTL_1,
                              (d->vs.vertexid_regid & 0xffu) |
                              ((d->vs.instanceid_regid & 0xffu) << 8) |
                              (0xfcu << 16) | (0xfcu << 24)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_CNTL_2, 0x0000fcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_CNTL_3, 0xfcfcfcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_CNTL_4, 0x000000fcu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_CNTL_5, 0x0000fcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_CNTL_6, 0u))
        return false;

    for (i = 0; i < d->num_vbufs && i < ADRENO_3D_MAX_VBUFS; i++) {
        if (!adreno_pm4_emit_pkt4(pm4, REG_A6XX_VFD_VERTEX_BUFFER(i), 4) ||
            !adreno_pm4_emit(pm4, (uint32_t)d->vbufs[i].iova) ||
            !adreno_pm4_emit(pm4, (uint32_t)(d->vbufs[i].iova >> 32)) ||
            !adreno_pm4_emit(pm4, d->vbufs[i].size) ||
            !adreno_pm4_emit(pm4, d->vbufs[i].stride))
            return false;
    }

    for (i = 0; i < d->num_vattrs && i < ADRENO_3D_MAX_VATTRS; i++) {
        const adreno_a730_vattr *va = &d->vattrs[i];
        uint32_t instr = (va->buf_idx & 0x1fu) |
                         ((va->offset & 0xfffu) << 5) |
                         ((va->fmt & 0xffu) << 20) |
                         (va->is_float ? (1u << 31) : 0u);
        uint32_t dest = (va->writemask & 0xfu) |
                        ((va->dst_regid & 0xffu) << 4);
        if (!adreno_pm4_emit_pkt4(pm4, REG_A6XX_VFD_FETCH_INSTR(i), 2) ||
            !adreno_pm4_emit(pm4, instr) ||
            !adreno_pm4_emit(pm4, 0u) ||
            !adreno_pm4_reg_write(pm4, REG_A6XX_VFD_DEST_CNTL(i), dest))
            return false;
    }

    /* 5. VS -> VPC -> PS Linkage */
    /* Pack user varyings first starting at VPC location 0, then SV_Position at the end. */
    for (i = 0; i < d->vs.num_varyings && i < ADRENO_3D_MAX_VARYINGS; i++) {
        uint32_t m = d->vs.varying_mask[i] & 0xfu;
        uint32_t comps = 0, b;
        for (b = 0; b < 4; b++) if (m & (1u << b)) comps++;
        if (comps == 0) continue;
        out_regids[out_reg_count] = d->vs.varying_regid[i];
        out_masks[out_reg_count]  = m;
        out_locs[out_reg_count]   = total_varyings_scalars;
        out_reg_count++;
        total_varyings_scalars += comps;
    }
    pos_loc = total_varyings_scalars;
    out_regids[out_reg_count] = d->vs.pos_regid;
    out_masks[out_reg_count]  = 0xfu;
    out_locs[out_reg_count]   = pos_loc;
    out_reg_count++;
    stride_in_vpc = pos_loc + 4u;

    /* 6. Vertex Shader (SP_VS_* + VPC_VS_* + PC_VS_*) */
    vs_cntl0 = ((d->vs.halfregfootprint & 0x3fu) << 1) |
               ((d->vs.fullregfootprint & 0x3fu) << 7) |
               (1u << 20); /* MERGEDREGS */

    if (!adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_CNTL_0, vs_cntl0) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_BOOLEAN_CF_MASK, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_OUTPUT_CNTL, out_reg_count & 0x3fu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_PROGRAM_COUNTER_OFFSET, 0u) ||
        !adreno_pm4_reg_write64(pm4, REG_A6XX_SP_VS_BASE, d->vs.iova) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_INSTR_SIZE, d->vs.instrlen_units) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_CONFIG, 0x100u /* ENABLED */) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_VS_CONST_CONFIG,
                              ((d->vs.constlen >> 2) & 0xffu) | 0x100u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_VS_VGS_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_PVT_MEM_PARAM, 0u) ||
        !adreno_pm4_reg_write64(pm4, REG_A6XX_SP_VS_PVT_MEM_BASE, 0ull) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_PVT_MEM_SIZE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_PVT_MEM_STACK_OFFSET, 0u) ||
        /* Disable HS / DS / GS stages */
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_HS_CONFIG, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_HS_CONST_CONFIG, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_DS_CONFIG, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_DS_CONST_CONFIG, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_GS_CONFIG, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_GS_CONST_CONFIG, 0u))
        return false;

    for (i = 0; i < (out_reg_count + 1u) / 2u; i++) {
        uint32_t v = (out_regids[i * 2u] & 0xffu) |
                     ((out_masks[i * 2u] & 0xfu) << 8);
        if (i * 2u + 1u < out_reg_count) {
            v |= ((out_regids[i * 2u + 1u] & 0xffu) << 16) |
                 ((out_masks[i * 2u + 1u] & 0xfu) << 24);
        }
        if (!adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_OUTPUT(i), v))
            return false;
    }
    for (i = 0; i < (out_reg_count + 3u) / 4u; i++) {
        uint32_t v = 0, k;
        for (k = 0; k < 4u && (i * 4u + k) < out_reg_count; k++)
            v |= ((out_locs[i * 4u + k] & 0xffu) << (k * 8u));
        if (!adreno_pm4_reg_write(pm4, REG_A6XX_SP_VS_VPC_DEST(i), v))
            return false;
    }

    if (!adreno_pm4_reg_write(pm4, REG_A6XX_VPC_VS_CNTL,
                              (stride_in_vpc & 0xffu) |
                              ((pos_loc & 0xffu) << 8) |
                              (0xffu << 16)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_VS_CLIP_CULL_CNTL, 0x00ffff00u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_VS_CLIP_CULL_CNTL_V2, 0x00ffff00u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_VS_SIV_CNTL, 0x00ffffffu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_VS_SIV_CNTL_V2, 0x00ffffffu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_RAST_STREAM_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_VPC_RAST_STREAM_CNTL_V2, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_RAST_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_SO_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_SO_OVERRIDE, 1u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_ROTATION_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_VPC_STEREO_RENDERING_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_VPC_STEREO_RENDERING_VIEWMASK, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_VPC_PC_CNTL, (1u << 2) /* D3D_VERTEX_ORDERING */) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_PC_CNTL, (1u << 2)) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_PC_VS_CNTL, stride_in_vpc & 0xffu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_PC_PS_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_PC_STEREO_RENDERING_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_PC_DGEN_SO_OVERRIDE, 1u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_PC_DGEN_SO_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_PC_DGEN_RAST_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_PC_RAST_STREAM_CNTL, 0u))
        return false;

    /* Load VS instructions + optional inline constants */
    if (!a730_emit_load_state6_geom_shader(pm4, ADRENO_SB6_VS_SHADER,
                                           d->vs.iova, d->vs.instrlen_units))
        return false;
    if (d->vs.constants && d->vs.const_vec4s) {
        if (!a730_emit_load_state6_geom_const(pm4, ADRENO_SB6_VS_SHADER, 0u,
                                              d->vs.constants, d->vs.const_vec4s))
            return false;
    }

    /* 7. Pixel Shader (SP_PS_* + VPC_PS_* + RB_PS_*) */
    has_varyings = (d->ps.num_varying_scalars > 0u && d->ps.ij_regid != 0xfcu);
    ps_cntl0 = ((d->ps.halfregfootprint & 0x3fu) << 1) |
               ((d->ps.fullregfootprint & 0x3fu) << 7) |
               (1u << 20) | /* THREAD128 */
               (has_varyings ? (1u << 22) : 0u) | /* VARYING */
               (1u << 31);  /* MERGEDREGS */
    /* A7xx SP_PS_CNTL_1: IJ_PERSP_PIXEL occupies 2 scalar sysval regs (r0.x, r0.y),
     * defer_wave_alloc_dis=1 (bit 8), evict_buf_mode=1 (bits 9..10). */
    ps_cntl1 = ((has_varyings ? 2u : 0u) & 0xffu) |
               (1u << 8) |
               (1u << 9) |
               ((d->ps.num_varying_scalars & 0xffu) << 13);

    if (!adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_CNTL_0, ps_cntl0) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_PS_CNTL_1, ps_cntl1) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_BOOLEAN_CF_MASK, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_PS_WAVE_CNTL,
                              1u | (has_varyings ? 2u : 0u)) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_RENDER_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_PROGRAM_COUNTER_OFFSET, 0u) ||
        !adreno_pm4_reg_write64(pm4, REG_A6XX_SP_PS_BASE, d->ps.iova) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_INSTR_SIZE, d->ps.instrlen_units) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_CONFIG,
                              0x100u /* ENABLED */ |
                              ((d->ps.num_textures & 0xffu) << 9) |
                              ((d->ps.num_samplers & 0x1fu) << 17)) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_PS_CONST_CONFIG,
                              ((d->ps.constlen >> 2) & 0xffu) | 0x100u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_TSIZE, d->ps.num_textures) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_TPL1_PS_SWIZZLE_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_TPL1_MODE_CNTL,
                              2u /* ISAMMODE_GL */ | (1u << 7) /* DESTDATATYPEOVERRIDE */) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_PS_VGS_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_PVT_MEM_PARAM, 0u) ||
        !adreno_pm4_reg_write64(pm4, REG_A6XX_SP_PS_PVT_MEM_BASE, 0ull) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_PVT_MEM_SIZE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_PVT_MEM_STACK_OFFSET, 0u) ||
        /* Initial tex load disabled (COUNT=0, IJ_WRITE_DISABLE=bit3, CONSTSLOTID=6..14, CONSTSLOTID4COORD=16..24) */
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_INITIAL_TEX_LOAD_CNTL,
                              (has_varyings ? 0u : (1u << 3)) |
                              (0x1ffu << 6) | (0x1ffu << 16)) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_LB_PARAM_LIMIT, 64u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_REG_PROG_ID_0, 0xfcfcfcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_REG_PROG_ID_1,
                              (has_varyings ? (d->ps.ij_regid & 0xffu) : 0xfcu) |
                              (0xfcu << 8) | (0xfcu << 16) | (0xfcu << 24)) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_REG_PROG_ID_2, 0xfcfcfcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_REG_PROG_ID_3, 0x0000fcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_OUTPUT_CNTL, 0xfcfcfc00u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_OUTPUT(0), d->ps.color0_regid & 0xffu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_MRT_CNTL, 1u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_MRT(0), d->rtv.fmt & 0xffu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_PS_OUTPUT_MASK, 0x0000000fu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_PS_MRT_CNTL, 1u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_PS_OUTPUT_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_PS_OUTPUT_MASK, 0x0000000fu) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_PS_INPUT_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_RB_INTERP_CNTL,
                              has_varyings ? ((1u << 0) | (1u << 10)) : 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_GRAS_CL_INTERP_CNTL,
                              has_varyings ? (1u << 0) : 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_PS_CNTL,
                              (d->ps.num_varying_scalars & 0xffu) |
                              (0xffu << 8) |
                              (has_varyings ? (1u << 16) : 0u) |
                              (0xffu << 24)))
        return false;

    /* Varying interpolation mode = smooth (0) and LM transfer mask */
    for (i = 0; i < 8u; i++) {
        if (!adreno_pm4_reg_write(pm4, REG_A6XX_VPC_VARYING_INTERP_MODE(i), 0u) ||
            !adreno_pm4_reg_write(pm4, REG_A6XX_VPC_VARYING_REPLACE_MODE(i), 0u))
            return false;
    }
    for (i = 0; i < 4u; i++) {
        uint32_t mask = 0u;
        uint32_t base_var = i * 32u;
        if (d->ps.num_varying_scalars > base_var) {
            uint32_t rem = d->ps.num_varying_scalars - base_var;
            mask = (rem >= 32u) ? 0xffffffffu : ((1u << rem) - 1u);
        }
        /* VPC_VARYING_LM_TRANSFER_CNTL uses inverted enable bits (0 = transfer, 1 = disable) */
        if (!adreno_pm4_reg_write(pm4, REG_A6XX_VPC_VARYING_LM_TRANSFER_CNTL(i), ~mask))
            return false;
    }

    /* Load PS instructions + optional inline constants + TPL1 textures/samplers */
    if (!adreno_pm4_load_state6_shader(pm4, ADRENO_SB6_FS_SHADER,
                                       d->ps.iova, d->ps.instrlen_units))
        return false;
    if (d->ps.constants && d->ps.const_vec4s) {
        if (!adreno_pm4_load_state6_const(pm4, ADRENO_SB6_FS_SHADER, 0u,
                                          d->ps.constants, d->ps.const_vec4s))
            return false;
    }
    if (d->ps.num_samplers > 0u && d->ps.tex_samp_iova) {
        if (!a730_emit_load_state6_frag_indirect(pm4, ADRENO_ST6_SHADER,
                                                 ADRENO_SB6_FS_TEX,
                                                 d->ps.tex_samp_iova,
                                                 d->ps.num_samplers) ||
            !adreno_pm4_reg_write64(pm4, REG_A6XX_SP_PS_SAMPLER_BASE,
                                    d->ps.tex_samp_iova))
            return false;
    }
    if (d->ps.num_textures > 0u && d->ps.tex_memobj_iova) {
        if (!adreno_pm4_event_write_1(pm4, CACHE_INVALIDATE7) ||
            !adreno_pm4_wait_for_idle(pm4) ||
            !a730_emit_load_state6_frag_indirect(pm4, ADRENO_ST6_CONSTANTS,
                                                 ADRENO_SB6_FS_TEX,
                                                 d->ps.tex_memobj_iova,
                                                 d->ps.num_textures) ||
            !adreno_pm4_reg_write64(pm4, REG_A6XX_SP_PS_TEXMEMOBJ_BASE,
                                    d->ps.tex_memobj_iova))
            return false;
    }

    /* 8. Kick Draw (CP_DRAW_INDX_OFFSET) */
    if (d->indexed && d->index_iova) {
        uint32_t idx_sz = (d->index_size_bytes == 2u) ? 1u : 2u; /* 1=16bit, 2=32bit */
        draw_init = (d->prim_type & 0x3fu) |
                    (0u << 6) |     /* DI_SRC_SEL_DMA */
                    (0u << 8) |     /* IGNORE_VISIBILITY */
                    (idx_sz << 10);
        if (!adreno_pm4_emit_pkt7(pm4, CP_DRAW_INDX_OFFSET, 7) ||
            !adreno_pm4_emit(pm4, draw_init) ||
            !adreno_pm4_emit(pm4, d->instance_count) ||
            !adreno_pm4_emit(pm4, d->vertex_count) ||
            !adreno_pm4_emit(pm4, 0u) || /* FIRST_INDX */
            !adreno_pm4_emit(pm4, (uint32_t)d->index_iova) ||
            !adreno_pm4_emit(pm4, (uint32_t)(d->index_iova >> 32)) ||
            !adreno_pm4_emit(pm4, d->index_buf_bytes / (d->index_size_bytes ? d->index_size_bytes : 4u)))
            return false;
    } else {
        draw_init = (d->prim_type & 0x3fu) |
                    (2u << 6) |     /* DI_SRC_SEL_AUTO_INDEX */
                    (0u << 8);      /* IGNORE_VISIBILITY */
        if (!adreno_pm4_emit_pkt7(pm4, CP_DRAW_INDX_OFFSET, 3) ||
            !adreno_pm4_emit(pm4, draw_init) ||
            !adreno_pm4_emit(pm4, d->instance_count) ||
            !adreno_pm4_emit(pm4, d->vertex_count))
            return false;
    }

    /* 9. Flush color CCU + UCHE to system memory so CPU/Display sees exact pixels */
    return a730_emit_flush_color(pm4);
}
