/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 *
 * Original Adreno 730 (A7xx / IR3) 64-bit ISA assembler for the
 * S22 native DirectX 12 UMD.
 */
#ifndef ADRENO_IR3_ASM_H
#define ADRENO_IR3_ASM_H

#include "adreno_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register ID encoding: regid = (reg_num << 2) | comp (0=x, 1=y, 2=z, 3=w). */
#define IR3_REG(n, c)     (((uint32_t)(n) << 2) | ((uint32_t)(c) & 3u))
#define IR3_CONST(n, c)   (((uint32_t)(n) << 2) | ((uint32_t)(c) & 3u))
#define IR3_REG_INVALID   0xfcu

/* Instruction sync / flow flags (bits 44, 59, 60). */
#define IR3_FLAG_SS       (1u << 0) /* short-sync (wait for SFU / ALU) */
#define IR3_FLAG_SY       (1u << 1) /* long-sync (wait for memory / tex) */
#define IR3_FLAG_JP       (1u << 2) /* join point */
#define IR3_FLAG_SAT      (1u << 3) /* saturate [0,1] */
#define IR3_FLAG_EI       (1u << 4) /* end-of-inputs (last bary.f) */
#define IR3_FLAG_UL       (1u << 5) /* unlock */

/* IR3 scalar data types (#type). */
typedef enum ir3_type {
    IR3_TYPE_F16   = 0,
    IR3_TYPE_F32   = 1,
    IR3_TYPE_U16   = 2,
    IR3_TYPE_U32   = 3,
    IR3_TYPE_S16   = 4,
    IR3_TYPE_S32   = 5,
    IR3_TYPE_U8    = 6,
    IR3_TYPE_U8_32 = 7,
} ir3_type;

/* IR3 cat2 comparison conditions (#cond). */
typedef enum ir3_cond {
    IR3_COND_LT = 0,
    IR3_COND_LE = 1,
    IR3_COND_GT = 2,
    IR3_COND_GE = 3,
    IR3_COND_EQ = 4,
    IR3_COND_NE = 5,
} ir3_cond;

/* IR3 source operand modifier (abs/neg). */
typedef enum ir3_src_mod {
    IR3_MOD_NONE   = 0,
    IR3_MOD_NEG    = 1,
    IR3_MOD_ABS    = 2,
    IR3_MOD_ABSNEG = 3,
} ir3_src_mod;

/* Source operand kind for cat2/cat3/cat4 instructions. */
typedef enum ir3_src_kind {
    IR3_SRC_GPR   = 0,
    IR3_SRC_CONST = 1,
    IR3_SRC_IMMED = 2,
} ir3_src_kind;

typedef struct ir3_src {
    ir3_src_kind kind;
    uint32_t     val; /* regid, constid, or signed 11-bit immediate */
    ir3_src_mod  mod;
} ir3_src;

static __inline ir3_src ir3_src_gpr(uint32_t regid)
{
    ir3_src s;
    s.kind = IR3_SRC_GPR;
    s.val  = regid & 0xffu;
    s.mod  = IR3_MOD_NONE;
    return s;
}

static __inline ir3_src ir3_src_gpr_mod(uint32_t regid, ir3_src_mod mod)
{
    ir3_src s;
    s.kind = IR3_SRC_GPR;
    s.val  = regid & 0xffu;
    s.mod  = mod;
    return s;
}

static __inline ir3_src ir3_src_const(uint32_t constid)
{
    ir3_src s;
    s.kind = IR3_SRC_CONST;
    s.val  = constid & 0x7ffu;
    s.mod  = IR3_MOD_NONE;
    return s;
}

static __inline ir3_src ir3_src_const_mod(uint32_t constid, ir3_src_mod mod)
{
    ir3_src s;
    s.kind = IR3_SRC_CONST;
    s.val  = constid & 0x7ffu;
    s.mod  = mod;
    return s;
}

static __inline ir3_src ir3_src_imm(int32_t imm11)
{
    ir3_src s;
    s.kind = IR3_SRC_IMMED;
    s.val  = (uint32_t)imm11 & 0x7ffu;
    s.mod  = IR3_MOD_NONE;
    return s;
}

/* Cat2 opcodes (bits 58:53). */
typedef enum ir3_cat2_op {
    IR3_OP_ADD_F  = 0x00,
    IR3_OP_MIN_F  = 0x01,
    IR3_OP_MAX_F  = 0x02,
    IR3_OP_MUL_F  = 0x03,
    IR3_OP_CMPS_F = 0x05,
    IR3_OP_ADD_U  = 0x10,
    IR3_OP_SUB_U  = 0x12,
    IR3_OP_AND_B  = 0x1c,
    IR3_OP_OR_B   = 0x1d,
    IR3_OP_XOR_B  = 0x1f,
    IR3_OP_SHL_B  = 0x36,
    IR3_OP_SHR_B  = 0x37,
    IR3_OP_ASHR_B = 0x38,
    IR3_OP_BARY_F = 0x39,
} ir3_cat2_op;

/* Cat3 opcodes (bits 58:55). */
typedef enum ir3_cat3_op {
    IR3_OP_MAD_F32 = 0x07,
    IR3_OP_SEL_B32 = 0x09,
} ir3_cat3_op;

/* Cat4 SFU opcodes (bits 58:53). */
typedef enum ir3_cat4_op {
    IR3_OP_RCP  = 0x00,
    IR3_OP_RSQ  = 0x01,
    IR3_OP_LOG2 = 0x02,
    IR3_OP_EXP2 = 0x03,
    IR3_OP_SIN  = 0x04,
    IR3_OP_COS  = 0x05,
} ir3_cat4_op;

/* Dynamic/fixed instruction buffer. */
typedef struct ir3_asm_buf {
    uint64_t *insns;
    uint32_t  capacity;
    uint32_t  count;
    uint32_t  max_full_reg; /* highest rN full register index used (0..47) */
    uint32_t  max_const_vec4;
    bool      overflow;
} ir3_asm_buf;

void ir3_asm_init(ir3_asm_buf *b, uint64_t *storage, uint32_t capacity);
bool ir3_asm_emit(ir3_asm_buf *b, uint64_t insn);
/* Pad instruction count up to a multiple of 16 instructions (128-byte unit) with NOPs. */
bool ir3_asm_finalize(ir3_asm_buf *b, uint32_t *out_units, uint32_t *out_bytes);

/* Cat0: control flow */
uint64_t ir3_enc_nop(uint32_t repeat, uint32_t flags);
uint64_t ir3_enc_end(uint32_t flags);
uint64_t ir3_enc_chsh(void);
uint64_t ir3_enc_chmask(void);
uint64_t ir3_enc_br(int32_t rel_insns, uint32_t pred_comp, bool inv, uint32_t flags);
uint64_t ir3_enc_kill(uint32_t pred_comp, bool inv, uint32_t flags);

/* Cat1: mov / convert */
uint64_t ir3_enc_mov_gpr(uint32_t dst_regid, uint32_t src_regid,
                         ir3_type dst_t, ir3_type src_t,
                         uint32_t repeat, bool src_r, uint32_t flags);
uint64_t ir3_enc_mov_const(uint32_t dst_regid, uint32_t constid,
                           ir3_type dst_t, ir3_type src_t,
                           uint32_t repeat, bool src_r, uint32_t flags);
uint64_t ir3_enc_mov_imm(uint32_t dst_regid, uint32_t imm32,
                         ir3_type dst_t, ir3_type src_t, uint32_t flags);

/* Cat2: 2-src ALU & bary.f */
uint64_t ir3_enc_cat2(ir3_cat2_op op, uint32_t dst_regid,
                      ir3_src src1, ir3_src src2,
                      ir3_cond cond, uint32_t repeat,
                      bool src1_r, bool src2_r, uint32_t flags);
uint64_t ir3_enc_bary_f(uint32_t dst_regid, uint32_t varying_slot,
                        uint32_t ij_regid, uint32_t repeat, uint32_t flags);

/* Cat3: 3-src ALU (src2 must be a GPR regid) */
uint64_t ir3_enc_cat3(ir3_cat3_op op, uint32_t dst_regid,
                      ir3_src src1, uint32_t src2_gpr, bool src2_neg,
                      ir3_src src3, uint32_t flags);

/* Cat4: SFU (1-src transcendental; consumers should set IR3_FLAG_SS) */
uint64_t ir3_enc_cat4(ir3_cat4_op op, uint32_t dst_regid,
                      ir3_src src, uint32_t flags);

/* Cat5: texture sample (non-bindless t#/s#) */
uint64_t ir3_enc_sam(uint32_t dst_regid, uint32_t wrmask,
                     uint32_t coord_regid, uint32_t tex_idx,
                     uint32_t samp_idx, uint32_t flags);
uint64_t ir3_enc_saml(uint32_t dst_regid, uint32_t wrmask,
                      uint32_t coord_regid, uint32_t lod_regid,
                      uint32_t tex_idx, uint32_t samp_idx, uint32_t flags);

/* Cat6: global memory load/store (64-bit base in src1_regid:src1_regid+1) */
uint64_t ir3_enc_ldg(uint32_t dst_regid, uint32_t base_regid,
                     int32_t byte_off, uint32_t comps, ir3_type type,
                     uint32_t flags);
uint64_t ir3_enc_stg(uint32_t base_regid, int32_t byte_off,
                     uint32_t val_regid, uint32_t comps, ir3_type type,
                     uint32_t flags);

/* Cat7: A7xx compute epilogue synchronization (lock/unlock) */
uint64_t ir3_enc_lock(uint32_t flags);
uint64_t ir3_enc_unlock(uint32_t flags);

/* Buffer emit helpers that also track register/const footprint automatically. */
bool ir3_emit_nop(ir3_asm_buf *b, uint32_t repeat, uint32_t flags);
bool ir3_emit_end(ir3_asm_buf *b);
bool ir3_emit_cs_end(ir3_asm_buf *b);
bool ir3_emit_mov_gpr(ir3_asm_buf *b, uint32_t dst, uint32_t src, ir3_type t, uint32_t flags);
bool ir3_emit_mov_const(ir3_asm_buf *b, uint32_t dst, uint32_t c, ir3_type t, uint32_t flags);
bool ir3_emit_mov_imm_f32(ir3_asm_buf *b, uint32_t dst, float f, uint32_t flags);
bool ir3_emit_mov_imm_u32(ir3_asm_buf *b, uint32_t dst, uint32_t u, uint32_t flags);
bool ir3_emit_cov(ir3_asm_buf *b, uint32_t dst, uint32_t src, ir3_type dst_t, ir3_type src_t, uint32_t flags);
bool ir3_emit_cat2(ir3_asm_buf *b, ir3_cat2_op op, uint32_t dst, ir3_src s1, ir3_src s2, ir3_cond cond, uint32_t flags);
bool ir3_emit_bary_f(ir3_asm_buf *b, uint32_t dst, uint32_t slot, uint32_t ij_regid, uint32_t flags);
bool ir3_emit_cat3(ir3_asm_buf *b, ir3_cat3_op op, uint32_t dst, ir3_src s1, uint32_t s2_gpr, bool s2_neg, ir3_src s3, uint32_t flags);
bool ir3_emit_cat4(ir3_asm_buf *b, ir3_cat4_op op, uint32_t dst, ir3_src s1, uint32_t flags);
bool ir3_emit_sam(ir3_asm_buf *b, uint32_t dst, uint32_t wrmask, uint32_t coord, uint32_t tex_idx, uint32_t samp_idx, uint32_t flags);
bool ir3_emit_saml(ir3_asm_buf *b, uint32_t dst, uint32_t wrmask, uint32_t coord, uint32_t lod, uint32_t tex_idx, uint32_t samp_idx, uint32_t flags);
bool ir3_emit_ldg(ir3_asm_buf *b, uint32_t dst, uint32_t base, int32_t off, uint32_t comps, ir3_type t, uint32_t flags);
bool ir3_emit_stg(ir3_asm_buf *b, uint32_t base, int32_t off, uint32_t val, uint32_t comps, ir3_type t, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_IR3_ASM_H */
