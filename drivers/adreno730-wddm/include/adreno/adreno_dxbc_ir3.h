/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 *
 * Original DXBC (DirectX Shader Model 4.0 / 5.0 / 5.1) to Adreno 730 IR3
 * compiler for the S22 native DirectX 12 UMD.
 */
#ifndef ADRENO_DXBC_IR3_H
#define ADRENO_DXBC_IR3_H

#include "adreno_types.h"
#include "adreno_ir3_asm.h"
#include "adreno_d3d12.h"
#include "adreno_a730_3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adreno_shader_stage {
    ADRENO_STAGE_PS = 0,
    ADRENO_STAGE_VS = 1,
    ADRENO_STAGE_CS = 5,
} adreno_shader_stage;

#define ADRENO_DXBC_MAX_IO     16u
#define ADRENO_DXBC_MAX_INSNS  2048u

typedef struct adreno_dxbc_io_elem {
    char     semantic[32];
    uint32_t semantic_idx;
    uint32_t sysval_type;   /* D3D_NAME_* (1 = SV_Position, 6 = SV_VertexID, 8 = SV_InstanceID) */
    uint32_t reg;           /* DXBC v# or o# register number */
    uint32_t mask;          /* 4-bit component mask */
    uint32_t ir3_regid;     /* assigned IR3 GPR regid (vec4 aligned) */
} adreno_dxbc_io_elem;

typedef struct adreno_dxbc_compiled {
    adreno_shader_stage stage;
    uint64_t            insns[ADRENO_DXBC_MAX_INSNS];
    uint32_t            insn_count;
    uint32_t            instrlen_units;  /* 128-byte units */
    uint32_t            code_bytes;
    uint32_t            fullregfootprint;
    uint32_t            halfregfootprint;
    uint32_t            constlen;        /* vec4s (multiple of 4 for A730 CONST_CONFIG) */
    /* VS metadata */
    uint32_t            pos_regid;
    uint32_t            vertexid_regid;
    uint32_t            instanceid_regid;
    uint32_t            num_inputs;
    adreno_dxbc_io_elem inputs[ADRENO_DXBC_MAX_IO];
    uint32_t            num_outputs;
    adreno_dxbc_io_elem outputs[ADRENO_DXBC_MAX_IO];
    uint32_t            num_varyings;
    uint32_t            varying_regid[ADRENO_3D_MAX_VARYINGS];
    uint32_t            varying_mask[ADRENO_3D_MAX_VARYINGS];
    /* PS metadata */
    uint32_t            color0_regid;
    uint32_t            ij_regid;
    uint32_t            num_varying_scalars;
    uint32_t            num_textures;
    uint32_t            num_samplers;
    /* CS metadata */
    uint32_t            local_size[3];
    adreno_d3d12_ir3_meta cs_meta;
} adreno_dxbc_compiled;

/*
 * Compile a standard DirectX DXBC (SM 4.0 / 5.0 / 5.1) shader container
 * (produced by fxc / D3DCompile) into native Adreno 730 64-bit IR3 code.
 */
enum adreno_status adreno_dxbc_compile_to_ir3(
    const void *dxbc_bytes, uint32_t dxbc_size,
    adreno_dxbc_compiled *out);

/*
 * Compile a D3D12 UMD DDI stripped SM4/SM5 token stream (pShaderCode from
 * D3D12DDIARG_CREATE_SHADER_0026 + parsed IO signatures) into Adreno 730 IR3.
 */
enum adreno_status adreno_dxbc_compile_tokens_to_ir3(
    const uint32_t *toks, uint32_t num_toks,
    const adreno_dxbc_io_elem *in_sig, uint32_t num_in,
    const adreno_dxbc_io_elem *out_sig, uint32_t num_out,
    adreno_dxbc_compiled *out, uint32_t *out_failed_opcode);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_DXBC_IR3_H */
