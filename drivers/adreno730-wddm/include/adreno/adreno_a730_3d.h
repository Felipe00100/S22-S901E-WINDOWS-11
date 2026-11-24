/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 *
 * Original Adreno 730 (A7xx) hardware 2D blit/clear and 3D sysmem direct-render
 * PM4 state & draw command encoder for the native DirectX 12 UMD.
 */
#ifndef ADRENO_A730_3D_H
#define ADRENO_A730_3D_H

#include "adreno_types.h"
#include "adreno_pm4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A6xx/A7xx color & vertex formats (hardware enum values). */
#define ADRENO_FMT_R8G8B8A8_UNORM      0x30u
#define ADRENO_FMT_R32_FLOAT           0x4au
#define ADRENO_FMT_R32G32_FLOAT        0x67u
#define ADRENO_FMT_R32G32B32_FLOAT     0x74u
#define ADRENO_FMT_R32G32B32A32_FLOAT  0x82u

/* A6xx/A7xx color channel swap modes. */
#define ADRENO_SWAP_WZYX               0u /* RGBA in memory (R at byte 0) */
#define ADRENO_SWAP_WXYZ               2u /* BGRA in memory (B at byte 0) */

/* A6xx/A7xx primitive topology (DI_PT_*). */
#define ADRENO_PRIM_POINTLIST          9u
#define ADRENO_PRIM_LINELIST           2u
#define ADRENO_PRIM_LINESTRIP          3u
#define ADRENO_PRIM_TRILIST            4u
#define ADRENO_PRIM_TRISTRIP           6u

/* A6xx/A7xx state blocks for CP_LOAD_STATE6_GEOM / FRAG. */
#define ADRENO_SB6_VS_SHADER           8u
#define ADRENO_SB6_FS_SHADER           12u

/* Maximum vertex attributes & varyings supported by the initial 3D encoder. */
#define ADRENO_3D_MAX_VBUFS            8u
#define ADRENO_3D_MAX_VATTRS           16u
#define ADRENO_3D_MAX_VARYINGS         8u

typedef struct adreno_a730_rtv {
    adreno_iova_t iova;
    uint32_t      width;
    uint32_t      height;
    uint32_t      pitch;       /* bytes per row (must be multiple of 64 for A730 RB) */
    uint32_t      fmt;         /* ADRENO_FMT_* */
    uint32_t      swap;        /* ADRENO_SWAP_* */
} adreno_a730_rtv;

typedef struct adreno_a730_vbuf {
    adreno_iova_t iova;
    uint32_t      size;
    uint32_t      stride;
} adreno_a730_vbuf;

typedef struct adreno_a730_vattr {
    uint32_t buf_idx;          /* vertex buffer slot (0..7) */
    uint32_t offset;           /* byte offset within vertex */
    uint32_t fmt;              /* ADRENO_FMT_* */
    uint32_t dst_regid;        /* destination VS register ID (IR3_REG(n, c)) */
    uint32_t writemask;        /* 4-bit component mask (e.g. 0xF for vec4, 0x7 for vec3) */
    bool     is_float;         /* true for float formats */
} adreno_a730_vattr;

typedef struct adreno_a730_vs_desc {
    adreno_iova_t iova;
    uint32_t      instrlen_units;  /* 128-byte units */
    uint32_t      fullregfootprint;
    uint32_t      halfregfootprint;
    uint32_t      constlen;        /* vec4s */
    uint32_t      pos_regid;       /* VS output register for SV_Position (vec4) */
    uint32_t      vertexid_regid;  /* regid for SV_VertexID, or 0xFC if unused */
    uint32_t      instanceid_regid;/* regid for SV_InstanceID, or 0xFC if unused */
    uint32_t      num_varyings;    /* number of vec4 varyings (excluding SV_Position) */
    uint32_t      varying_regid[ADRENO_3D_MAX_VARYINGS];
    uint32_t      varying_mask[ADRENO_3D_MAX_VARYINGS]; /* 4-bit mask per varying */
    const uint32_t *constants;     /* optional inline VS constants (const_vec4s * 4 dwords) */
    uint32_t      const_vec4s;
} adreno_a730_vs_desc;

typedef struct adreno_a730_ps_desc {
    adreno_iova_t iova;
    uint32_t      instrlen_units;  /* 128-byte units */
    uint32_t      fullregfootprint;
    uint32_t      halfregfootprint;
    uint32_t      constlen;        /* vec4s */
    uint32_t      color0_regid;    /* PS output register for SV_Target0 (vec4) */
    uint32_t      ij_regid;        /* regid for perspective pixel barycentrics, or 0xFC */
    uint32_t      num_varying_scalars; /* total scalar varying components read via bary.f */
    const uint32_t *constants;     /* optional inline PS constants (const_vec4s * 4 dwords) */
    uint32_t      const_vec4s;
} adreno_a730_ps_desc;

typedef struct adreno_a730_draw_desc {
    adreno_a730_rtv     rtv;
    float               vp_x, vp_y, vp_w, vp_h, vp_min_z, vp_max_z;
    uint32_t            scissor_x0, scissor_y0, scissor_x1, scissor_y1;
    uint32_t            prim_type;     /* ADRENO_PRIM_* */
    uint32_t            vertex_count;
    uint32_t            instance_count;
    uint32_t            start_vertex;
    uint32_t            start_instance;
    bool                indexed;
    adreno_iova_t       index_iova;
    uint32_t            index_size_bytes; /* 2 or 4 */
    uint32_t            index_buf_bytes;
    uint32_t            num_vbufs;
    adreno_a730_vbuf    vbufs[ADRENO_3D_MAX_VBUFS];
    uint32_t            num_vattrs;
    adreno_a730_vattr   vattrs[ADRENO_3D_MAX_VATTRS];
    adreno_a730_vs_desc vs;
    adreno_a730_ps_desc ps;
    bool                blend_enable;
} adreno_a730_draw_desc;

/* Emit A730 2D hardware solid-color clear (RB_A2D + CP_BLIT). */
bool adreno_a730_emit_clear_2d(adreno_pm4 *pm4, const adreno_a730_rtv *rtv,
                               const float rgba[4]);

/* Emit A730 2D hardware linear-to-linear rectangle copy (TPL1_A2D + RB_A2D + CP_BLIT). */
bool adreno_a730_emit_copy_2d(adreno_pm4 *pm4,
                              const adreno_a730_rtv *dst, uint32_t dst_x, uint32_t dst_y,
                              const adreno_a730_rtv *src, uint32_t src_x, uint32_t src_y,
                              uint32_t copy_w, uint32_t copy_h);

/* Emit full A730 3D sysmem direct-render state + VS/PS shaders + draw kick. */
bool adreno_a730_emit_draw_3d(adreno_pm4 *pm4, const adreno_a730_draw_desc *d);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_A730_3D_H */
