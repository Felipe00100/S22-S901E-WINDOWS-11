/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 *
 * Original DXBC (DirectX Shader Model 4.0 / 5.0 / 5.1) parser and native
 * Adreno 730 (IR3) shader compiler for the S22 DirectX 12 UMD.
 *
 * Translates standard HLSL DXBC containers (ISGN/OSGN + SHDR/SHEX) into
 * Adreno 730 64-bit IR3 machine code for Vertex, Pixel, and Compute shaders.
 */
#include "adreno/adreno_dxbc_ir3.h"

#include <string.h>

/* ---- DXBC Container & SM4/SM5 Constants -------------------------------- */

#define FOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))

#define DXBC_MAGIC   FOURCC('D','X','B','C')
#define CHUNK_ISGN   FOURCC('I','S','G','N')
#define CHUNK_ISG1   FOURCC('I','S','G','1')
#define CHUNK_OSGN   FOURCC('O','S','G','N')
#define CHUNK_OSG1   FOURCC('O','S','G','1')
#define CHUNK_SHDR   FOURCC('S','H','D','R')
#define CHUNK_SHEX   FOURCC('S','H','E','X')

/* D3D_NAME system value types */
#define DXBC_SV_UNDEFINED   0u
#define DXBC_SV_POSITION    1u
#define DXBC_SV_VERTEX_ID   6u
#define DXBC_SV_INSTANCE_ID 8u
#define DXBC_SV_TARGET      64u

/* SM4/SM5 opcodes */
enum dxbc_opcode {
    OP_ADD              = 0x00,
    OP_AND              = 0x01,
    OP_DIV              = 0x0e,
    OP_DP2              = 0x0f,
    OP_DP3              = 0x10,
    OP_DP4              = 0x11,
    OP_EQ               = 0x18,
    OP_EXP              = 0x19,
    OP_FRC              = 0x1a,
    OP_FTOI             = 0x1b,
    OP_FTOU             = 0x1c,
    OP_GE               = 0x1d,
    OP_IADD             = 0x1e,
    OP_IMUL             = 0x26,
    OP_ISHL             = 0x29,
    OP_ISHR             = 0x2a,
    OP_ITOF             = 0x2b,
    OP_LOG              = 0x2f,
    OP_LT               = 0x31,
    OP_MAD              = 0x32,
    OP_MIN              = 0x33,
    OP_MAX              = 0x34,
    OP_MOV              = 0x36,
    OP_MOVC             = 0x37,
    OP_MUL              = 0x38,
    OP_NE               = 0x39,
    OP_NOP              = 0x3a,
    OP_OR               = 0x3c,
    OP_RET              = 0x3e,
    OP_RSQ              = 0x44,
    OP_SQRT             = 0x4c,
    OP_SINCOS           = 0x4d,
    OP_UTOF             = 0x56,
    OP_USHR             = 0x55,
    OP_XOR              = 0x57,
    OP_DCL_RESOURCE     = 0x58,
    OP_DCL_CB           = 0x59,
    OP_DCL_SAMPLER      = 0x5a,
    OP_DCL_INPUT        = 0x5f,
    OP_DCL_INPUT_SGV    = 0x60,
    OP_DCL_INPUT_SIV    = 0x61,
    OP_DCL_INPUT_PS     = 0x62,
    OP_DCL_INPUT_PS_SGV = 0x63,
    OP_DCL_INPUT_PS_SIV = 0x64,
    OP_DCL_OUTPUT       = 0x65,
    OP_DCL_OUTPUT_SGV   = 0x66,
    OP_DCL_OUTPUT_SIV   = 0x67,
    OP_DCL_TEMPS        = 0x68,
    OP_DCL_GLOBAL_FLAGS = 0x6a,
    OP_RCP              = 0x86,
    OP_DCL_THREAD_GROUP = 0x9b,
    OP_DCL_UAV_RAW      = 0x9d,
    OP_DCL_UAV_STRUCT   = 0x9e,
    OP_LD_RAW           = 0xa5,
    OP_STORE_RAW        = 0xa6,
    OP_LD_STRUCTURED    = 0xa7,
    OP_STORE_STRUCTURED = 0xa8,
};

/* SM4/SM5 operand types */
enum dxbc_op_type {
    OPER_TEMP           = 0,
    OPER_INPUT          = 1,
    OPER_OUTPUT         = 2,
    OPER_IMMEDIATE32    = 4,
    OPER_SAMPLER        = 6,
    OPER_RESOURCE       = 7,
    OPER_CB             = 8,
    OPER_NULL           = 13,
    OPER_UAV            = 30,
    OPER_THREAD_ID      = 32,
    OPER_GROUP_ID       = 33,
    OPER_THREAD_ID_GRP  = 34,
};

typedef struct dxbc_operand {
    uint32_t type;
    uint32_t num_comps;  /* 0=none, 1=scalar, 2=4comp */
    uint32_t sel_mode;   /* 0=mask, 1=swizzle, 2=scalar */
    uint32_t writemask;  /* 4-bit mask */
    uint8_t  swizzle[4]; /* 0..3 per lane */
    uint32_t index_dim;
    uint32_t index[2];   /* register index, or cb[slot][vec4] */
    uint32_t imm[4];     /* immediate 32-bit values */
    bool     negate;
    bool     abs;
} dxbc_operand;

typedef struct dxbc_ctx {
    adreno_dxbc_compiled *out;
    ir3_asm_buf           a;
    /* Register file allocation (vec4 base register numbers, r0..r47):
     *   For VS:
     *     in_base   .. in_base + num_in - 1  : vertex inputs (VFD destination)
     *     temp_base .. temp_base + num_tmp -1: DXBC r# temporaries
     *     out_base  .. out_base + num_out - 1: DXBC o# outputs (SV_Position + varyings)
     *     scratch0, scratch1                 : temporary scratch vec4s for immediates/dp/div
     *   For PS:
     *     r0.xy                              : perspective pixel IJ barycentric coords (if varyings)
     *     in_base   .. in_base + num_in - 1  : bary.f-interpolated varyings
     *     temp_base .. temp_base + num_tmp -1: DXBC r# temporaries
     *     out_base  .. out_base + num_out - 1: DXBC o# outputs (SV_Target0)
     *     scratch0, scratch1                 : scratch vec4s
     *   For CS:
     *     r0.xyz                             : local thread ID (loaded by HW via localid_regid=0)
     *     temp_base .. temp_base + num_tmp -1: DXBC r# temporaries
     *     scratch0, scratch1                 : scratch vec4s (address calc, immediates)
     */
    uint32_t in_base;
    uint32_t temp_base;
    uint32_t out_base;
    uint32_t scratch0;
    uint32_t scratch1;
    uint32_t num_temps;
    uint32_t max_in_reg;
    uint32_t max_out_reg;
    uint32_t uav_stride[8];
} dxbc_ctx;

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static bool ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Parse ISGN/OSGN (or ISG1/OSG1) signature chunk. */
static void parse_sig_chunk(const uint8_t *chunk, uint32_t chunk_size,
                            bool is_v1, adreno_dxbc_io_elem *elems,
                            uint32_t *out_count)
{
    uint32_t count, i;
    uint32_t stride = is_v1 ? 32u : 24u;
    uint32_t off_base = 8u;

    *out_count = 0;
    if (!chunk || chunk_size < 8u)
        return;
    count = rd32(chunk);
    if (count > ADRENO_DXBC_MAX_IO)
        count = ADRENO_DXBC_MAX_IO;
    for (i = 0; i < count; i++) {
        uint32_t eo = off_base + i * stride;
        uint32_t name_off, sem_idx, sysval, reg, mask;
        adreno_dxbc_io_elem *el;
        uint32_t k;

        if (eo + stride > chunk_size)
            break;
        if (is_v1) {
            /* stream(0), name_off(4), sem_idx(8), sysval(12), comp_type(16), reg(20), mask(24) */
            name_off = rd32(chunk + eo + 4);
            sem_idx  = rd32(chunk + eo + 8);
            sysval   = rd32(chunk + eo + 12);
            reg      = rd32(chunk + eo + 20);
            mask     = chunk[eo + 24] & 0xfu;
        } else {
            /* name_off(0), sem_idx(4), sysval(8), comp_type(12), reg(16), mask(20) */
            name_off = rd32(chunk + eo + 0);
            sem_idx  = rd32(chunk + eo + 4);
            sysval   = rd32(chunk + eo + 8);
            reg      = rd32(chunk + eo + 16);
            mask     = chunk[eo + 20] & 0xfu;
        }
        el = &elems[*out_count];
        memset(el, 0, sizeof(*el));
        el->semantic_idx = sem_idx;
        el->sysval_type  = sysval;
        el->reg          = reg;
        el->mask         = mask ? mask : 0xfu;
        if (name_off < chunk_size) {
            for (k = 0; k < sizeof(el->semantic) - 1u && name_off + k < chunk_size; k++) {
                char c = (char)chunk[name_off + k];
                el->semantic[k] = c;
                if (c == 0) break;
            }
            el->semantic[sizeof(el->semantic) - 1u] = 0;
        }
        (*out_count)++;
    }
}

/* Decode one SM4/SM5 operand from token stream. */
static bool decode_operand(const uint32_t *toks, uint32_t end,
                           uint32_t *pos, dxbc_operand *op)
{
    uint32_t t, i;
    if (*pos >= end)
        return false;
    memset(op, 0, sizeof(*op));
    t = toks[(*pos)++];
    op->num_comps = t & 3u;
    op->sel_mode  = (t >> 2) & 3u;
    op->type      = (t >> 12) & 0xffu;
    op->index_dim = (t >> 20) & 3u;

    if (op->num_comps == 2u) {
        if (op->sel_mode == 0u) {
            op->writemask = (t >> 4) & 0xfu;
            for (i = 0; i < 4; i++) op->swizzle[i] = (uint8_t)i;
        } else if (op->sel_mode == 1u) {
            op->writemask = 0xfu;
            op->swizzle[0] = (uint8_t)((t >> 4) & 3u);
            op->swizzle[1] = (uint8_t)((t >> 6) & 3u);
            op->swizzle[2] = (uint8_t)((t >> 8) & 3u);
            op->swizzle[3] = (uint8_t)((t >> 10) & 3u);
        } else {
            uint8_t c = (uint8_t)((t >> 4) & 3u);
            op->writemask = (1u << c);
            op->swizzle[0] = op->swizzle[1] = op->swizzle[2] = op->swizzle[3] = c;
        }
    } else if (op->num_comps == 1u) {
        op->writemask = 1u;
        op->swizzle[0] = op->swizzle[1] = op->swizzle[2] = op->swizzle[3] = 0;
    }

    /* Extended operand token (modifiers: neg / abs) */
    if (t & 0x80000000u) {
        uint32_t ext;
        if (*pos >= end) return false;
        ext = toks[(*pos)++];
        if ((ext & 0x3fu) == 1u) {
            uint32_t mod = (ext >> 6) & 0xffu;
            if (mod & 1u) op->negate = true;
            if (mod & 2u) op->abs = true;
        }
    }

    /* Index tokens */
    for (i = 0; i < op->index_dim && i < 2u; i++) {
        uint32_t rep = (t >> (22u + i * 3u)) & 7u;
        if (rep == 0u) { /* immediate 32-bit */
            if (*pos >= end) return false;
            op->index[i] = toks[(*pos)++];
        } else if (rep == 1u) { /* immediate 64-bit */
            if (*pos + 1u >= end) return false;
            op->index[i] = toks[(*pos)++];
            (*pos)++;
        } else {
            /* Relative index (not supported in initial bring-up) */
            dxbc_operand dummy;
            if (rep == 3u && *pos < end) op->index[i] = toks[(*pos)++];
            if (!decode_operand(toks, end, pos, &dummy)) return false;
        }
    }

    if (op->type == OPER_IMMEDIATE32) {
        uint32_t n = (op->num_comps == 2u) ? 4u : 1u;
        if (*pos + n > end) return false;
        for (i = 0; i < n; i++)
            op->imm[i] = toks[(*pos)++];
        if (n == 1u)
            op->imm[1] = op->imm[2] = op->imm[3] = op->imm[0];
    }
    return true;
}

static ir3_src_mod op_to_mod(const dxbc_operand *op)
{
    if (op->abs && op->negate) return IR3_MOD_ABSNEG;
    if (op->abs)               return IR3_MOD_ABS;
    if (op->negate)            return IR3_MOD_NEG;
    return IR3_MOD_NONE;
}

/* Map a DXBC destination operand + lane (0..3) to an IR3 full GPR regid. */
static uint32_t resolve_dst_regid(dxbc_ctx *c, const dxbc_operand *op, uint32_t lane)
{
    switch (op->type) {
    case OPER_TEMP:
        return IR3_REG(c->temp_base + op->index[0], lane);
    case OPER_OUTPUT:
        return IR3_REG(c->out_base + op->index[0], lane);
    default:
        return IR3_REG(c->scratch1, lane);
    }
}

/*
 * Materialize a DXBC source operand for a single scalar lane into an ir3_src.
 * If the operand is an immediate32, loads it into scratch_regid first.
 */
static ir3_src resolve_src_lane(dxbc_ctx *c, const dxbc_operand *op,
                                uint32_t lane, uint32_t scratch_regid)
{
    uint32_t ch = op->swizzle[lane & 3u];
    ir3_src_mod mod = op_to_mod(op);

    switch (op->type) {
    case OPER_TEMP:
        return ir3_src_gpr_mod(IR3_REG(c->temp_base + op->index[0], ch), mod);
    case OPER_INPUT:
        return ir3_src_gpr_mod(IR3_REG(c->in_base + op->index[0], ch), mod);
    case OPER_OUTPUT:
        return ir3_src_gpr_mod(IR3_REG(c->out_base + op->index[0], ch), mod);
    case OPER_CB: {
        /* cb0[idx] -> c[idx], cb1[idx] -> c[16 + idx], etc. */
        uint32_t vec4_idx = op->index[0] * 16u + op->index[1];
        /* For CS, c0 is reserved for UAV0 64-bit base pointer -> offset CB by +1! */
        if (c->out->stage == ADRENO_STAGE_CS)
            vec4_idx += 1u;
        return ir3_src_const_mod(IR3_CONST(vec4_idx, ch), mod);
    }
    case OPER_THREAD_ID:
    case OPER_THREAD_ID_GRP:
        /* r0.xyz holds local thread ID from hardware (localid_regid = 0) */
        return ir3_src_gpr_mod(IR3_REG(0, ch), mod);
    case OPER_IMMEDIATE32: {
        uint32_t bits = op->imm[ch];
        if (op->abs)    bits &= 0x7fffffffu;
        if (op->negate) bits ^= 0x80000000u;
        ir3_emit_mov_imm_u32(&c->a, scratch_regid, bits, 0);
        return ir3_src_gpr(scratch_regid);
    }
    default:
        ir3_emit_mov_imm_u32(&c->a, scratch_regid, 0u, 0);
        return ir3_src_gpr(scratch_regid);
    }
}

/* Materialize a source operand unconditionally into a GPR (required for cat3 src2 or cov). */
static uint32_t resolve_src_as_gpr(dxbc_ctx *c, const dxbc_operand *op,
                                   uint32_t lane, uint32_t scratch_regid,
                                   bool *out_neg)
{
    uint32_t ch = op->swizzle[lane & 3u];
    if (out_neg) *out_neg = false;

    if (op->type == OPER_TEMP && !op->abs) {
        if (out_neg) *out_neg = op->negate;
        return IR3_REG(c->temp_base + op->index[0], ch);
    }
    if (op->type == OPER_INPUT && !op->abs) {
        if (out_neg) *out_neg = op->negate;
        return IR3_REG(c->in_base + op->index[0], ch);
    }
    if (op->type == OPER_CB) {
        uint32_t vec4_idx = op->index[0] * 16u + op->index[1];
        if (c->out->stage == ADRENO_STAGE_CS) vec4_idx += 1u;
        ir3_emit_mov_const(&c->a, scratch_regid, IR3_CONST(vec4_idx, ch), IR3_TYPE_U32, 0);
        if (op->abs) {
            ir3_emit_cat2(&c->a, IR3_OP_MAX_F, scratch_regid,
                          ir3_src_gpr_mod(scratch_regid, IR3_MOD_ABS),
                          ir3_src_gpr_mod(scratch_regid, IR3_MOD_ABS),
                          IR3_COND_LT, 0);
        }
        if (out_neg) *out_neg = op->negate;
        return scratch_regid;
    }
    if (op->type == OPER_IMMEDIATE32) {
        uint32_t bits = op->imm[ch];
        if (op->abs)    bits &= 0x7fffffffu;
        if (op->negate) bits ^= 0x80000000u;
        ir3_emit_mov_imm_u32(&c->a, scratch_regid, bits, 0);
        return scratch_regid;
    }
    if (op->type == OPER_THREAD_ID || op->type == OPER_THREAD_ID_GRP) {
        return IR3_REG(0, ch);
    }
    ir3_emit_mov_imm_u32(&c->a, scratch_regid, 0u, 0);
    return scratch_regid;
}

static bool emit_mov_lane(dxbc_ctx *c, uint32_t dst_regid,
                          const dxbc_operand *src, uint32_t lane,
                          uint32_t sat_flag)
{
    uint32_t ch = src->swizzle[lane & 3u];
    if (src->type == OPER_IMMEDIATE32 && !sat_flag) {
        uint32_t bits = src->imm[ch];
        if (src->abs)    bits &= 0x7fffffffu;
        if (src->negate) bits ^= 0x80000000u;
        return ir3_emit_mov_imm_u32(&c->a, dst_regid, bits, 0);
    }
    if (!src->abs && !src->negate && !sat_flag) {
        if (src->type == OPER_TEMP)
            return ir3_emit_mov_gpr(&c->a, dst_regid, IR3_REG(c->temp_base + src->index[0], ch), IR3_TYPE_F32, 0);
        if (src->type == OPER_INPUT)
            return ir3_emit_mov_gpr(&c->a, dst_regid, IR3_REG(c->in_base + src->index[0], ch), IR3_TYPE_F32, 0);
        if (src->type == OPER_CB) {
            uint32_t vec4_idx = src->index[0] * 16u + src->index[1];
            if (c->out->stage == ADRENO_STAGE_CS) vec4_idx += 1u;
            return ir3_emit_mov_const(&c->a, dst_regid, IR3_CONST(vec4_idx, ch), IR3_TYPE_F32, 0);
        }
        if (src->type == OPER_THREAD_ID || src->type == OPER_THREAD_ID_GRP)
            return ir3_emit_mov_gpr(&c->a, dst_regid, IR3_REG(0, ch), IR3_TYPE_U32, 0);
    }
    /* Fallback via add.f(sat) dst, src, 0.0 to apply abs/neg/saturate cleanly */
    {
        ir3_src s = resolve_src_lane(c, src, lane, IR3_REG(c->scratch0, 0));
        ir3_emit_mov_imm_f32(&c->a, IR3_REG(c->scratch0, 1), 0.0f, 0);
        return ir3_emit_cat2(&c->a, IR3_OP_ADD_F, dst_regid, s,
                             ir3_src_gpr(IR3_REG(c->scratch0, 1)),
                             IR3_COND_LT, sat_flag);
    }
}

/* ---- Main DXBC / SM4-5 Token -> IR3 Compiler --------------------------- */

enum adreno_status adreno_dxbc_compile_tokens_to_ir3(
    const uint32_t *toks, uint32_t num_toks,
    const adreno_dxbc_io_elem *in_sig, uint32_t num_in,
    const adreno_dxbc_io_elem *out_sig, uint32_t num_out,
    adreno_dxbc_compiled *out, uint32_t *out_failed_opcode)
{
    uint32_t i, pos, ver_tok, stage;
    dxbc_ctx ctx;
    uint32_t units = 0, code_bytes = 0;

    if (out_failed_opcode) *out_failed_opcode = 0;
    if (!toks || num_toks < 2u || !out)
        return ADRENO_ERR_INVALID;

    memset(out, 0, sizeof(*out));
    out->pos_regid        = IR3_REG_INVALID;
    out->vertexid_regid   = IR3_REG_INVALID;
    out->instanceid_regid = IR3_REG_INVALID;
    out->color0_regid     = IR3_REG_INVALID;
    out->ij_regid         = IR3_REG_INVALID;
    out->local_size[0]    = 1u;
    out->local_size[1]    = 1u;
    out->local_size[2]    = 1u;

    if (in_sig && num_in) {
        if (num_in > ADRENO_DXBC_MAX_IO) num_in = ADRENO_DXBC_MAX_IO;
        memcpy(out->inputs, in_sig, num_in * sizeof(*in_sig));
        out->num_inputs = num_in;
    }
    if (out_sig && num_out) {
        if (num_out > ADRENO_DXBC_MAX_IO) num_out = ADRENO_DXBC_MAX_IO;
        memcpy(out->outputs, out_sig, num_out * sizeof(*out_sig));
        out->num_outputs = num_out;
    }

    ver_tok = toks[0];
    stage = (ver_tok >> 16) & 0xffffu;
    if (stage != ADRENO_STAGE_PS && stage != ADRENO_STAGE_VS &&
        stage != ADRENO_STAGE_CS)
        return ADRENO_ERR_UNSUPPORTED;
    out->stage = (adreno_shader_stage)stage;

    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ir3_asm_init(&ctx.a, out->insns, ADRENO_DXBC_MAX_INSNS);

    /* Pass 1: Scan declarations */
    pos = 2u;
    while (pos < num_toks) {
        uint32_t op_tok = toks[pos];
        uint32_t opcode = op_tok & 0x7ffu;
        uint32_t len    = (opcode == 0x0bu /* CUSTOMDATA */)
                              ? ((pos + 1u < num_toks) ? toks[pos + 1u] : 0u)
                              : ((op_tok >> 24) & 0x7fu);
        if (len == 0u || pos + len > num_toks)
            break;
        if (opcode == OP_DCL_TEMPS && len >= 2u) {
            ctx.num_temps = toks[pos + 1u];
        } else if (opcode == OP_DCL_THREAD_GROUP && len >= 4u) {
            out->local_size[0] = toks[pos + 1u];
            out->local_size[1] = toks[pos + 2u];
            out->local_size[2] = toks[pos + 3u];
        } else if (opcode == OP_DCL_UAV_STRUCT && len >= 3u) {
            uint32_t p2 = pos + 1u;
            dxbc_operand uav;
            if (decode_operand(toks, pos + len, &p2, &uav) && p2 < pos + len) {
                if (uav.index[0] < 8u)
                    ctx.uav_stride[uav.index[0]] = toks[p2];
            }
        }
        pos += len;
    }

    for (i = 0; i < out->num_inputs; i++) {
        if (out->inputs[i].reg + 1u > ctx.max_in_reg)
            ctx.max_in_reg = out->inputs[i].reg + 1u;
    }
    for (i = 0; i < out->num_outputs; i++) {
        if (out->outputs[i].reg + 1u > ctx.max_out_reg)
            ctx.max_out_reg = out->outputs[i].reg + 1u;
    }

    /* Register layout */
    if (stage == ADRENO_STAGE_PS) {
        ctx.in_base = 1u; /* reserve r0.xy for perspective pixel barycentrics */
    } else if (stage == ADRENO_STAGE_CS) {
        ctx.in_base = 1u; /* reserve r0.xyz for local thread ID */
    } else {
        ctx.in_base = 0u; /* VS: VFD loads vertex attributes directly into r0.. */
    }
    ctx.temp_base = ctx.in_base + ctx.max_in_reg;
    ctx.out_base  = ctx.temp_base + ctx.num_temps;
    ctx.scratch0  = ctx.out_base + ctx.max_out_reg;
    ctx.scratch1  = ctx.scratch0 + 1u;
    if (ctx.scratch1 >= 46u)
        return ADRENO_ERR_RANGE;

    /* Assign IR3 register IDs to signature elements & emit PS bary.f prologue */
    for (i = 0; i < out->num_inputs; i++) {
        out->inputs[i].ir3_regid = IR3_REG(ctx.in_base + out->inputs[i].reg, 0);
        if (stage == ADRENO_STAGE_VS) {
            if (out->inputs[i].sysval_type == DXBC_SV_VERTEX_ID ||
                ci_eq(out->inputs[i].semantic, "SV_VERTEXID"))
                out->vertexid_regid = out->inputs[i].ir3_regid;
            else if (out->inputs[i].sysval_type == DXBC_SV_INSTANCE_ID ||
                     ci_eq(out->inputs[i].semantic, "SV_INSTANCEID"))
                out->instanceid_regid = out->inputs[i].ir3_regid;
        }
    }
    for (i = 0; i < out->num_outputs; i++) {
        out->outputs[i].ir3_regid = IR3_REG(ctx.out_base + out->outputs[i].reg, 0);
        if (stage == ADRENO_STAGE_VS) {
            if (out->outputs[i].sysval_type == DXBC_SV_POSITION ||
                ci_eq(out->outputs[i].semantic, "SV_POSITION")) {
                out->pos_regid = out->outputs[i].ir3_regid;
            } else if (out->num_varyings < ADRENO_3D_MAX_VARYINGS) {
                uint32_t vi = out->num_varyings++;
                out->varying_regid[vi] = out->outputs[i].ir3_regid;
                out->varying_mask[vi]  = out->outputs[i].mask;
            }
        } else if (stage == ADRENO_STAGE_PS) {
            if (out->outputs[i].reg == 0u ||
                ci_eq(out->outputs[i].semantic, "SV_TARGET")) {
                out->color0_regid = out->outputs[i].ir3_regid;
            }
        }
    }

    /* PS prologue: interpolate every non-SV_Position input via bary.f from r0.x */
    if (stage == ADRENO_STAGE_PS) {
        uint32_t total_var = 0;
        for (i = 0; i < out->num_inputs; i++) {
            uint32_t b;
            if (out->inputs[i].sysval_type == DXBC_SV_POSITION ||
                ci_eq(out->inputs[i].semantic, "SV_POSITION"))
                continue;
            for (b = 0; b < 4u; b++)
                if (out->inputs[i].mask & (1u << b)) total_var++;
        }
        out->num_varying_scalars = total_var;
        if (total_var > 0u) {
            uint32_t slot = 0;
            out->ij_regid = IR3_REG(0, 0);
            for (i = 0; i < out->num_inputs; i++) {
                uint32_t b;
                if (out->inputs[i].sysval_type == DXBC_SV_POSITION ||
                    ci_eq(out->inputs[i].semantic, "SV_POSITION"))
                    continue;
                for (b = 0; b < 4u; b++) {
                    if (!(out->inputs[i].mask & (1u << b)))
                        continue;
                    uint32_t fl = (slot + 1u == total_var) ? IR3_FLAG_EI : 0u;
                    ir3_emit_bary_f(&ctx.a,
                                    IR3_REG(ctx.in_base + out->inputs[i].reg, b),
                                    slot, out->ij_regid, fl);
                    slot++;
                }
            }
            /* Safety NOP to ensure bary.f results settle before ALU */
            ir3_emit_nop(&ctx.a, 5, 0);
        }
    }

    /* Pass 2: Translate SM4/SM5 instructions to Adreno 730 IR3 */
    pos = 2u;
    while (pos < num_toks && !ctx.a.overflow) {
        uint32_t op_tok   = toks[pos];
        uint32_t opcode   = op_tok & 0x7ffu;
        uint32_t len      = (opcode == 0x0bu /* CUSTOMDATA */)
                                ? ((pos + 1u < num_toks) ? toks[pos + 1u] : 0u)
                                : ((op_tok >> 24) & 0x7fu);
        uint32_t sat_flag = ((op_tok >> 13) & 1u) ? IR3_FLAG_SAT : 0u;
        uint32_t end_inst = pos + len;
        uint32_t pcur     = pos + 1u;
        dxbc_operand d, s0, s1, s2;
        uint32_t lane;

        if (len == 0u || end_inst > num_toks)
            break;

        /* Skip extended opcode tokens (if bit 31 is set) */
        if (op_tok & 0x80000000u) {
            while (pcur < end_inst && (toks[pcur] & 0x80000000u))
                pcur++;
            if (pcur < end_inst) pcur++;
        }

        switch (opcode) {
        case OP_NOP:
        case OP_DCL_RESOURCE:
        case OP_DCL_CB:
        case OP_DCL_SAMPLER:
        case OP_DCL_INPUT:
        case OP_DCL_INPUT_SGV:
        case OP_DCL_INPUT_SIV:
        case OP_DCL_INPUT_PS:
        case OP_DCL_INPUT_PS_SGV:
        case OP_DCL_INPUT_PS_SIV:
        case OP_DCL_OUTPUT:
        case OP_DCL_OUTPUT_SGV:
        case OP_DCL_OUTPUT_SIV:
        case OP_DCL_TEMPS:
        case OP_DCL_GLOBAL_FLAGS:
        case OP_DCL_THREAD_GROUP:
        case OP_DCL_UAV_RAW:
        case OP_DCL_UAV_STRUCT:
        case 0x0bu: /* CUSTOMDATA */
        case 0x69u: /* DCL_INDEX_RANGE */
        case 0x9cu: /* DCL_UAV_TYPED */
            break;

        case OP_RET:
            if (stage == ADRENO_STAGE_CS) {
                ir3_emit_cs_end(&ctx.a);
            } else {
                ir3_emit_nop(&ctx.a, 5, 0);
                ir3_emit_end(&ctx.a);
            }
            break;

        case OP_MOV:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0)) {
                for (lane = 0; lane < 4u; lane++) {
                    if (!(d.writemask & (1u << lane))) continue;
                    emit_mov_lane(&ctx, resolve_dst_regid(&ctx, &d, lane),
                                  &s0, lane, sat_flag);
                }
            }
            break;

        case OP_ADD:
        case OP_MUL:
        case OP_MIN:
        case OP_MAX:
        case OP_IADD:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_ISHL:
        case OP_USHR:
        case OP_ISHR:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0) &&
                decode_operand(toks, end_inst, &pcur, &s1)) {
                ir3_cat2_op ir_op =
                    (opcode == OP_ADD)  ? IR3_OP_ADD_F  :
                    (opcode == OP_MUL)  ? IR3_OP_MUL_F  :
                    (opcode == OP_MIN)  ? IR3_OP_MIN_F  :
                    (opcode == OP_MAX)  ? IR3_OP_MAX_F  :
                    (opcode == OP_IADD) ? IR3_OP_ADD_U  :
                    (opcode == OP_AND)  ? IR3_OP_AND_B  :
                    (opcode == OP_OR)   ? IR3_OP_OR_B   :
                    (opcode == OP_XOR)  ? IR3_OP_XOR_B  :
                    (opcode == OP_ISHL) ? IR3_OP_SHL_B  :
                    (opcode == OP_USHR) ? IR3_OP_SHR_B  : IR3_OP_ASHR_B;
                for (lane = 0; lane < 4u; lane++) {
                    if (!(d.writemask & (1u << lane))) continue;
                    ir3_src a0 = resolve_src_lane(&ctx, &s0, lane, IR3_REG(ctx.scratch0, 0));
                    ir3_src a1 = resolve_src_lane(&ctx, &s1, lane, IR3_REG(ctx.scratch0, 1));
                    /* Cat2 cannot take two constants simultaneously -> move a1 to scratch */
                    if (a0.kind == IR3_SRC_CONST && a1.kind == IR3_SRC_CONST) {
                        ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch0, 1), a1.val, IR3_TYPE_F32, 0);
                        a1 = ir3_src_gpr_mod(IR3_REG(ctx.scratch0, 1), a1.mod);
                    }
                    ir3_emit_cat2(&ctx.a, ir_op, resolve_dst_regid(&ctx, &d, lane),
                                  a0, a1, IR3_COND_LT, sat_flag);
                }
            }
            break;

        case OP_MAD:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0) &&
                decode_operand(toks, end_inst, &pcur, &s1) &&
                decode_operand(toks, end_inst, &pcur, &s2)) {
                for (lane = 0; lane < 4u; lane++) {
                    bool s1_neg = false;
                    uint32_t s1_gpr;
                    ir3_src a0, a2;
                    if (!(d.writemask & (1u << lane))) continue;
                    a0 = resolve_src_lane(&ctx, &s0, lane, IR3_REG(ctx.scratch0, 0));
                    s1_gpr = resolve_src_as_gpr(&ctx, &s1, lane, IR3_REG(ctx.scratch0, 1), &s1_neg);
                    a2 = resolve_src_lane(&ctx, &s2, lane, IR3_REG(ctx.scratch0, 2));
                    if (a0.kind == IR3_SRC_CONST && a2.kind == IR3_SRC_CONST) {
                        ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch0, 2), a2.val, IR3_TYPE_F32, 0);
                        a2 = ir3_src_gpr_mod(IR3_REG(ctx.scratch0, 2), a2.mod);
                    }
                    ir3_emit_cat3(&ctx.a, IR3_OP_MAD_F32,
                                  resolve_dst_regid(&ctx, &d, lane),
                                  a0, s1_gpr, s1_neg, a2, sat_flag);
                }
            }
            break;

        case OP_DP2:
        case OP_DP3:
        case OP_DP4:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0) &&
                decode_operand(toks, end_inst, &pcur, &s1)) {
                uint32_t n = (opcode == OP_DP2) ? 2u : (opcode == OP_DP3) ? 3u : 4u;
                uint32_t acc = IR3_REG(ctx.scratch1, 0);
                uint32_t k;
                for (k = 0; k < n; k++) {
                    ir3_src a0 = resolve_src_lane(&ctx, &s0, k, IR3_REG(ctx.scratch0, 0));
                    if (k == 0u) {
                        ir3_src a1 = resolve_src_lane(&ctx, &s1, k, IR3_REG(ctx.scratch0, 1));
                        if (a0.kind == IR3_SRC_CONST && a1.kind == IR3_SRC_CONST) {
                            ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch0, 1), a1.val, IR3_TYPE_F32, 0);
                            a1 = ir3_src_gpr_mod(IR3_REG(ctx.scratch0, 1), a1.mod);
                        }
                        ir3_emit_cat2(&ctx.a, IR3_OP_MUL_F, acc, a0, a1, IR3_COND_LT, 0);
                    } else {
                        bool s1_neg = false;
                        uint32_t s1_gpr = resolve_src_as_gpr(&ctx, &s1, k, IR3_REG(ctx.scratch0, 1), &s1_neg);
                        uint32_t fl = (k + 1u == n) ? sat_flag : 0u;
                        ir3_emit_cat3(&ctx.a, IR3_OP_MAD_F32, acc, a0, s1_gpr, s1_neg,
                                      ir3_src_gpr(acc), fl);
                    }
                }
                for (lane = 0; lane < 4u; lane++) {
                    if (!(d.writemask & (1u << lane))) continue;
                    ir3_emit_mov_gpr(&ctx.a, resolve_dst_regid(&ctx, &d, lane),
                                     acc, IR3_TYPE_F32, 0);
                }
            }
            break;

        case OP_RCP:
        case OP_RSQ:
        case OP_LOG:
        case OP_EXP:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0)) {
                ir3_cat4_op c4 = (opcode == OP_RCP) ? IR3_OP_RCP  :
                                 (opcode == OP_RSQ) ? IR3_OP_RSQ  :
                                 (opcode == OP_LOG) ? IR3_OP_LOG2 : IR3_OP_EXP2;
                for (lane = 0; lane < 4u; lane++) {
                    if (!(d.writemask & (1u << lane))) continue;
                    ir3_src a0 = resolve_src_lane(&ctx, &s0, lane, IR3_REG(ctx.scratch0, 0));
                    ir3_emit_cat4(&ctx.a, c4, resolve_dst_regid(&ctx, &d, lane), a0, sat_flag);
                    ir3_emit_nop(&ctx.a, 0, IR3_FLAG_SS);
                }
            }
            break;

        case OP_DIV:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0) &&
                decode_operand(toks, end_inst, &pcur, &s1)) {
                for (lane = 0; lane < 4u; lane++) {
                    if (!(d.writemask & (1u << lane))) continue;
                    ir3_src a0 = resolve_src_lane(&ctx, &s0, lane, IR3_REG(ctx.scratch0, 0));
                    ir3_src a1 = resolve_src_lane(&ctx, &s1, lane, IR3_REG(ctx.scratch0, 1));
                    ir3_emit_cat4(&ctx.a, IR3_OP_RCP, IR3_REG(ctx.scratch1, 0), a1, 0);
                    ir3_emit_cat2(&ctx.a, IR3_OP_MUL_F, resolve_dst_regid(&ctx, &d, lane),
                                  a0, ir3_src_gpr(IR3_REG(ctx.scratch1, 0)),
                                  IR3_COND_LT, sat_flag | IR3_FLAG_SS);
                }
            }
            break;

        case OP_SQRT:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0)) {
                for (lane = 0; lane < 4u; lane++) {
                    if (!(d.writemask & (1u << lane))) continue;
                    ir3_src a0 = resolve_src_lane(&ctx, &s0, lane, IR3_REG(ctx.scratch0, 0));
                    ir3_emit_cat4(&ctx.a, IR3_OP_RSQ, IR3_REG(ctx.scratch1, 0), a0, 0);
                    ir3_emit_cat4(&ctx.a, IR3_OP_RCP, resolve_dst_regid(&ctx, &d, lane),
                                  ir3_src_gpr(IR3_REG(ctx.scratch1, 0)),
                                  sat_flag | IR3_FLAG_SS);
                    ir3_emit_nop(&ctx.a, 0, IR3_FLAG_SS);
                }
            }
            break;

        case OP_ITOF:
        case OP_UTOF:
        case OP_FTOI:
        case OP_FTOU:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0)) {
                ir3_type dt = (opcode == OP_ITOF || opcode == OP_UTOF) ? IR3_TYPE_F32 :
                              (opcode == OP_FTOI) ? IR3_TYPE_S32 : IR3_TYPE_U32;
                ir3_type st = (opcode == OP_ITOF) ? IR3_TYPE_S32 :
                              (opcode == OP_UTOF) ? IR3_TYPE_U32 : IR3_TYPE_F32;
                for (lane = 0; lane < 4u; lane++) {
                    bool dummy_neg = false;
                    uint32_t sg;
                    if (!(d.writemask & (1u << lane))) continue;
                    sg = resolve_src_as_gpr(&ctx, &s0, lane, IR3_REG(ctx.scratch0, 0), &dummy_neg);
                    ir3_emit_cov(&ctx.a, resolve_dst_regid(&ctx, &d, lane), sg, dt, st, 0);
                }
            }
            break;

        case OP_LT:
        case OP_GE:
        case OP_EQ:
        case OP_NE:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0) &&
                decode_operand(toks, end_inst, &pcur, &s1)) {
                ir3_cond cond = (opcode == OP_LT) ? IR3_COND_LT :
                                (opcode == OP_GE) ? IR3_COND_GE :
                                (opcode == OP_EQ) ? IR3_COND_EQ : IR3_COND_NE;
                for (lane = 0; lane < 4u; lane++) {
                    uint32_t dst_r = resolve_dst_regid(&ctx, &d, lane);
                    ir3_src a0, a1;
                    if (!(d.writemask & (1u << lane))) continue;
                    a0 = resolve_src_lane(&ctx, &s0, lane, IR3_REG(ctx.scratch0, 0));
                    a1 = resolve_src_lane(&ctx, &s1, lane, IR3_REG(ctx.scratch0, 1));
                    if (a0.kind == IR3_SRC_CONST && a1.kind == IR3_SRC_CONST) {
                        ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch0, 1), a1.val, IR3_TYPE_F32, 0);
                        a1 = ir3_src_gpr_mod(IR3_REG(ctx.scratch0, 1), a1.mod);
                    }
                    /* cmps.f outputs 1/0; DXBC expects 0xFFFFFFFF / 0x00000000 */
                    ir3_emit_cat2(&ctx.a, IR3_OP_CMPS_F, IR3_REG(ctx.scratch1, 0), a0, a1, cond, 0);
                    ir3_emit_mov_imm_u32(&ctx.a, IR3_REG(ctx.scratch0, 2), 0xffffffffu, 0);
                    ir3_emit_mov_imm_u32(&ctx.a, IR3_REG(ctx.scratch0, 3), 0x00000000u, 0);
                    ir3_emit_cat3(&ctx.a, IR3_OP_SEL_B32, dst_r,
                                  ir3_src_gpr(IR3_REG(ctx.scratch0, 2)),
                                  IR3_REG(ctx.scratch1, 0), false,
                                  ir3_src_gpr(IR3_REG(ctx.scratch0, 3)), 0);
                }
            }
            break;

        case OP_MOVC:
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0) &&
                decode_operand(toks, end_inst, &pcur, &s1) &&
                decode_operand(toks, end_inst, &pcur, &s2)) {
                for (lane = 0; lane < 4u; lane++) {
                    bool cneg = false;
                    uint32_t cg;
                    ir3_src a1, a2;
                    if (!(d.writemask & (1u << lane))) continue;
                    cg = resolve_src_as_gpr(&ctx, &s0, lane, IR3_REG(ctx.scratch0, 0), &cneg);
                    a1 = resolve_src_lane(&ctx, &s1, lane, IR3_REG(ctx.scratch0, 1));
                    a2 = resolve_src_lane(&ctx, &s2, lane, IR3_REG(ctx.scratch0, 2));
                    if (a1.kind == IR3_SRC_CONST && a2.kind == IR3_SRC_CONST) {
                        ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch0, 2), a2.val, IR3_TYPE_U32, 0);
                        a2 = ir3_src_gpr_mod(IR3_REG(ctx.scratch0, 2), a2.mod);
                    }
                    ir3_emit_cat3(&ctx.a, IR3_OP_SEL_B32, resolve_dst_regid(&ctx, &d, lane),
                                  a1, cg, false, a2, 0);
                }
            }
            break;

        case OP_STORE_RAW:
            /* store_raw u0, byte_offset, src_val */
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0) &&
                decode_operand(toks, end_inst, &pcur, &s1)) {
                uint32_t wmask = d.writemask ? d.writemask : 1u;
                int32_t imm_base_off = 0;
                /* Load 64-bit UAV0 base from c0.xy into scratch1.xy */
                ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch1, 0), IR3_CONST(0, 0), IR3_TYPE_U32, 0);
                ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch1, 1), IR3_CONST(0, 1), IR3_TYPE_U32, 0);
                if (s0.type == OPER_IMMEDIATE32 && s0.imm[0] <= 2048u) {
                    imm_base_off = (int32_t)s0.imm[0];
                } else {
                    ir3_src off_s = resolve_src_lane(&ctx, &s0, 0, IR3_REG(ctx.scratch0, 0));
                    ir3_emit_nop(&ctx.a, 5, 0);
                    ir3_emit_cat2(&ctx.a, IR3_OP_ADD_U, IR3_REG(ctx.scratch1, 0),
                                  ir3_src_gpr(IR3_REG(ctx.scratch1, 0)), off_s, IR3_COND_LT, 0);
                }
                for (lane = 0; lane < 4u; lane++) {
                    bool dummy = false;
                    uint32_t vg;
                    if (!(wmask & (1u << lane))) continue;
                    vg = resolve_src_as_gpr(&ctx, &s1, lane, IR3_REG(ctx.scratch0, 2), &dummy);
                    ir3_emit_nop(&ctx.a, 5, 0);
                    ir3_emit_stg(&ctx.a, IR3_REG(ctx.scratch1, 0),
                                 imm_base_off + (int32_t)(lane * 4u),
                                 vg, 1u, IR3_TYPE_U32, 0);
                }
            }
            break;

        case OP_LD_RAW:
            /* ld_raw dst, byte_offset, u0 */
            if (decode_operand(toks, end_inst, &pcur, &d) &&
                decode_operand(toks, end_inst, &pcur, &s0) &&
                decode_operand(toks, end_inst, &pcur, &s1)) {
                ir3_src off_s = resolve_src_lane(&ctx, &s0, 0, IR3_REG(ctx.scratch0, 0));
                ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch1, 0), IR3_CONST(0, 0), IR3_TYPE_U32, 0);
                ir3_emit_mov_const(&ctx.a, IR3_REG(ctx.scratch1, 1), IR3_CONST(0, 1), IR3_TYPE_U32, 0);
                ir3_emit_cat2(&ctx.a, IR3_OP_ADD_U, IR3_REG(ctx.scratch1, 0),
                              ir3_src_gpr(IR3_REG(ctx.scratch1, 0)), off_s, IR3_COND_LT, 0);
                for (lane = 0; lane < 4u; lane++) {
                    if (!(d.writemask & (1u << lane))) continue;
                    ir3_emit_ldg(&ctx.a, resolve_dst_regid(&ctx, &d, lane),
                                 IR3_REG(ctx.scratch1, 0), (int32_t)(lane * 4u),
                                 1u, IR3_TYPE_U32, 0);
                }
                ir3_emit_nop(&ctx.a, 0, IR3_FLAG_SY);
            }
            break;

        default:
            /* Unsupported opcode: fail cleanly instead of emitting broken code. */
            if (out_failed_opcode) *out_failed_opcode = opcode;
            return ADRENO_ERR_UNSUPPORTED;
        }

        pos = end_inst;
    }

    /* Ensure shader ends with IR3 END instruction */
    if (ctx.a.count == 0 || ctx.a.insns[ctx.a.count - 1u] != ir3_enc_end(0)) {
        if (stage == ADRENO_STAGE_CS)
            ir3_emit_cs_end(&ctx.a);
        else
            ir3_emit_end(&ctx.a);
    }

    if (!ir3_asm_finalize(&ctx.a, &units, &code_bytes))
        return ADRENO_ERR_OVERFLOW;

    out->insn_count       = ctx.a.count;
    out->instrlen_units   = units;
    out->code_bytes       = code_bytes;
    out->fullregfootprint = ctx.a.max_full_reg ? ctx.a.max_full_reg : 1u;
    out->halfregfootprint = 0u;
    /* A730 SP_*_CONST_CONFIG expects constlen aligned to 4 vec4s (min 8 -> 0x102). */
    out->constlen         = (ctx.a.max_const_vec4 + 3u) & ~3u;
    if (out->constlen < 8u) out->constlen = 8u;

    if (stage == ADRENO_STAGE_CS) {
        out->cs_meta.instrlen_units   = units;
        out->cs_meta.constlen         = out->constlen;
        out->cs_meta.fullregfootprint = out->fullregfootprint < 4u ? 4u : out->fullregfootprint;
        out->cs_meta.halfregfootprint = 0u;
        out->cs_meta.branchstack      = 0u;
        out->cs_meta.threadsize       = 0u; /* THREAD64 (matches silicon-proven A730 CS self-test) */
        out->cs_meta.shared_size      = 0u;
        out->cs_meta.constantrammode  = 0u;
        out->cs_meta.mergedregs       = 1u;
        out->cs_meta.localid_regid    = 0u; /* r0.x */
        out->cs_meta.wgid_constid     = IR3_REG_INVALID;
        out->cs_meta.local_size[0]    = out->local_size[0];
        out->cs_meta.local_size[1]    = out->local_size[1];
        out->cs_meta.local_size[2]    = out->local_size[2];
    }

    return ADRENO_OK;
}

enum adreno_status adreno_dxbc_compile_to_ir3(
    const void *dxbc_bytes, uint32_t dxbc_size,
    adreno_dxbc_compiled *out)
{
    const uint8_t *p = (const uint8_t *)dxbc_bytes;
    const uint8_t *sh_chunk = NULL;
    uint32_t sh_size = 0, chunk_count, i;
    adreno_dxbc_io_elem in_sig[ADRENO_DXBC_MAX_IO], out_sig[ADRENO_DXBC_MAX_IO];
    uint32_t num_in = 0, num_out = 0;

    if (!dxbc_bytes || dxbc_size < 32u || !out)
        return ADRENO_ERR_INVALID;
    if (rd32(p) != DXBC_MAGIC)
        return ADRENO_ERR_INVALID;
    chunk_count = rd32(p + 28u);
    if (chunk_count == 0 || 32u + chunk_count * 4u > dxbc_size)
        return ADRENO_ERR_INVALID;

    for (i = 0; i < chunk_count; i++) {
        uint32_t off = rd32(p + 32u + i * 4u);
        uint32_t fcc, csz;
        if (off + 8u > dxbc_size) continue;
        fcc = rd32(p + off);
        csz = rd32(p + off + 4u);
        if (off + 8u + csz > dxbc_size) continue;
        if (fcc == CHUNK_ISGN || fcc == CHUNK_ISG1)
            parse_sig_chunk(p + off + 8u, csz, (fcc == CHUNK_ISG1), in_sig, &num_in);
        else if (fcc == CHUNK_OSGN || fcc == CHUNK_OSG1)
            parse_sig_chunk(p + off + 8u, csz, (fcc == CHUNK_OSG1), out_sig, &num_out);
        else if (fcc == CHUNK_SHDR || fcc == CHUNK_SHEX) {
            sh_chunk = p + off + 8u;
            sh_size  = csz;
        }
    }
    if (!sh_chunk || sh_size < 8u)
        return ADRENO_ERR_UNSUPPORTED;
    return adreno_dxbc_compile_tokens_to_ir3(
        (const uint32_t *)sh_chunk, sh_size / 4u,
        in_sig, num_in, out_sig, num_out, out, NULL);
}
