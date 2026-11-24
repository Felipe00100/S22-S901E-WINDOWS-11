/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 *
 * Original Adreno 730 (A7xx / IR3) 64-bit ISA assembler.
 * All encodings follow the Adreno A6xx/A7xx 64-bit VLIW/scalar ISA format.
 */
#include "adreno/adreno_ir3_asm.h"

#include <string.h>

static uint64_t ir3_common_flags(uint32_t flags)
{
    uint64_t w = 0;
    if (flags & IR3_FLAG_SS) w |= (1ull << 44);
    if (flags & IR3_FLAG_JP) w |= (1ull << 59);
    if (flags & IR3_FLAG_SY) w |= (1ull << 60);
    return w;
}

static uint32_t ir3_enc_multisrc(ir3_src s)
{
    uint32_t v = ((uint32_t)(s.mod & 3u) << 14);
    switch (s.kind) {
    case IR3_SRC_GPR:
        /* bits 13:11 = 000, bits 7:0 = regid */
        v |= (s.val & 0xffu);
        break;
    case IR3_SRC_CONST:
        /* bits 13:11 = 010, bits 10:0 = constid */
        v |= (2u << 11) | (s.val & 0x7ffu);
        break;
    case IR3_SRC_IMMED:
        /* bits 13:11 = 100, bits 10:0 = signed 11-bit immediate */
        v |= (4u << 11) | (s.val & 0x7ffu);
        break;
    }
    return v;
}

static uint32_t ir3_enc_cat3_src(ir3_src s, bool *out_neg)
{
    uint32_t v = 0;
    if (out_neg)
        *out_neg = (s.mod == IR3_MOD_NEG || s.mod == IR3_MOD_ABSNEG);
    if (s.kind == IR3_SRC_CONST)
        v = (2u << 11) | (s.val & 0x7ffu);
    else
        v = (s.val & 0xffu); /* GPR */
    return v;
}

static void ir3_track_reg(ir3_asm_buf *b, uint32_t regid)
{
    uint32_t rnum;
    if (!b || regid >= 192u) /* ignore special/predicate regs >= r48 */
        return;
    rnum = regid >> 2;
    if (rnum + 1u > b->max_full_reg)
        b->max_full_reg = rnum + 1u;
}

static void ir3_track_src(ir3_asm_buf *b, ir3_src s)
{
    if (!b)
        return;
    if (s.kind == IR3_SRC_GPR) {
        ir3_track_reg(b, s.val);
    } else if (s.kind == IR3_SRC_CONST) {
        uint32_t v4 = (s.val >> 2) + 1u;
        if (v4 > b->max_const_vec4)
            b->max_const_vec4 = v4;
    }
}

void ir3_asm_init(ir3_asm_buf *b, uint64_t *storage, uint32_t capacity)
{
    if (!b)
        return;
    b->insns = storage;
    b->capacity = storage ? capacity : 0;
    b->count = 0;
    b->max_full_reg = 0;
    b->max_const_vec4 = 0;
    b->overflow = false;
}

bool ir3_asm_emit(ir3_asm_buf *b, uint64_t insn)
{
    if (!b || b->overflow || b->count >= b->capacity) {
        if (b)
            b->overflow = true;
        return false;
    }
    b->insns[b->count++] = insn;
    return true;
}

bool ir3_asm_finalize(ir3_asm_buf *b, uint32_t *out_units, uint32_t *out_bytes)
{
    uint32_t padded;
    if (!b || b->overflow || b->count == 0)
        return false;
    /* A730 shader instruction preload works in 16-instruction (128-byte) units. */
    padded = (b->count + 15u) & ~15u;
    while (b->count < padded) {
        if (!ir3_asm_emit(b, ir3_enc_nop(0, 0)))
            return false;
    }
    if (out_units)
        *out_units = padded / 16u;
    if (out_bytes)
        *out_bytes = padded * 8u;
    return true;
}

/* ---- Cat0: Control Flow ------------------------------------------------ */

uint64_t ir3_enc_nop(uint32_t repeat, uint32_t flags)
{
    uint64_t w = (0ull << 61) | ir3_common_flags(flags);
    w |= ((uint64_t)(repeat & 7u) << 40);
    /* opc = 0000 at bits 58:55 */
    return w;
}

uint64_t ir3_enc_end(uint32_t flags)
{
    uint64_t w = (0ull << 61) | ir3_common_flags(flags);
    w |= (6ull << 55); /* opc = 0110 (end) */
    return w;
}

uint64_t ir3_enc_chsh(void)
{
    return (0ull << 61) | (7ull << 55);
}

uint64_t ir3_enc_chmask(void)
{
    return (0ull << 61) | (8ull << 55);
}

uint64_t ir3_enc_br(int32_t rel_insns, uint32_t pred_comp, bool inv, uint32_t flags)
{
    uint64_t w = (0ull << 61) | ir3_common_flags(flags);
    w |= (1ull << 55); /* branch opc = 0001, subop 000 at bits 39:37 */
    w |= ((uint64_t)(pred_comp & 3u) << 53);
    if (inv) w |= (1ull << 52);
    w |= (uint64_t)(uint32_t)rel_insns;
    return w;
}

uint64_t ir3_enc_kill(uint32_t pred_comp, bool inv, uint32_t flags)
{
    uint64_t w = (0ull << 61) | ir3_common_flags(flags);
    w |= (5ull << 55); /* kill opc = 0101 */
    w |= ((uint64_t)(pred_comp & 3u) << 53);
    if (inv) w |= (1ull << 52);
    return w;
}

/* ---- Cat1: Move / Convert ---------------------------------------------- */

static uint64_t ir3_cat1_base(uint32_t dst_regid, ir3_type dst_t,
                              ir3_type src_t, uint32_t repeat,
                              bool src_r, uint32_t flags)
{
    uint64_t w = (1ull << 61) | ir3_common_flags(flags);
    if (flags & IR3_FLAG_UL) w |= (1ull << 45);
    w |= ((uint64_t)(dst_regid & 0xffu) << 32);
    w |= ((uint64_t)(repeat & 3u) << 40);
    if (src_r) w |= (1ull << 43);
    w |= ((uint64_t)(dst_t & 7u) << 46);
    w |= ((uint64_t)(src_t & 7u) << 50);
    return w;
}

uint64_t ir3_enc_mov_gpr(uint32_t dst_regid, uint32_t src_regid,
                         ir3_type dst_t, ir3_type src_t,
                         uint32_t repeat, bool src_r, uint32_t flags)
{
    uint64_t w = ir3_cat1_base(dst_regid, dst_t, src_t, repeat, src_r, flags);
    /* bits 54:53 = 00 (GPR) */
    w |= (uint64_t)(src_regid & 0xffu);
    return w;
}

uint64_t ir3_enc_mov_const(uint32_t dst_regid, uint32_t constid,
                           ir3_type dst_t, ir3_type src_t,
                           uint32_t repeat, bool src_r, uint32_t flags)
{
    uint64_t w = ir3_cat1_base(dst_regid, dst_t, src_t, repeat, src_r, flags);
    w |= (1ull << 53); /* bits 54:53 = 01 (CONST) */
    w |= (uint64_t)(constid & 0x7ffu);
    return w;
}

uint64_t ir3_enc_mov_imm(uint32_t dst_regid, uint32_t imm32,
                         ir3_type dst_t, ir3_type src_t, uint32_t flags)
{
    uint64_t w = ir3_cat1_base(dst_regid, dst_t, src_t, 0, false, flags);
    w |= (2ull << 53); /* bits 54:53 = 10 (IMMED) */
    w |= (uint64_t)imm32;
    return w;
}

/* ---- Cat2: 2-Source ALU & bary.f --------------------------------------- */

uint64_t ir3_enc_cat2(ir3_cat2_op op, uint32_t dst_regid,
                      ir3_src src1, ir3_src src2,
                      ir3_cond cond, uint32_t repeat,
                      bool src1_r, bool src2_r, uint32_t flags)
{
    uint64_t w = (2ull << 61) | ir3_common_flags(flags);
    w |= (uint64_t)ir3_enc_multisrc(src1);
    w |= ((uint64_t)ir3_enc_multisrc(src2) << 16);
    w |= ((uint64_t)(dst_regid & 0xffu) << 32);
    w |= ((uint64_t)(repeat & 3u) << 40);
    if (flags & IR3_FLAG_SAT) w |= (1ull << 42);
    if (src1_r) w |= (1ull << 43);
    if (flags & IR3_FLAG_UL)  w |= (1ull << 45);
    if (flags & IR3_FLAG_EI)  w |= (1ull << 47);
    if (op == IR3_OP_CMPS_F)
        w |= ((uint64_t)(cond & 7u) << 48);
    if (src2_r) w |= (1ull << 51);
    w |= (1ull << 52); /* full = 1 (32-bit) */
    w |= ((uint64_t)(op & 0x3fu) << 53);
    return w;
}

uint64_t ir3_enc_bary_f(uint32_t dst_regid, uint32_t varying_slot,
                        uint32_t ij_regid, uint32_t repeat, uint32_t flags)
{
    /* bary.f: src1 = immed(varying_slot), src2 = gpr(ij_regid), src1_r = (repeat > 0) */
    return ir3_enc_cat2(IR3_OP_BARY_F, dst_regid,
                        ir3_src_imm((int32_t)varying_slot),
                        ir3_src_gpr(ij_regid),
                        IR3_COND_LT, repeat, (repeat > 0), false, flags);
}

/* ---- Cat3: 3-Source ALU ------------------------------------------------ */

uint64_t ir3_enc_cat3(ir3_cat3_op op, uint32_t dst_regid,
                      ir3_src src1, uint32_t src2_gpr, bool src2_neg,
                      ir3_src src3, uint32_t flags)
{
    bool s1_neg = false, s3_neg = false;
    uint32_t s1 = ir3_enc_cat3_src(src1, &s1_neg);
    uint32_t s3 = ir3_enc_cat3_src(src3, &s3_neg);
    uint64_t w = (3ull << 61) | ir3_common_flags(flags);

    w |= (uint64_t)(s1 & 0x1fffu);
    if (s1_neg) w |= (1ull << 14);
    w |= ((uint64_t)(s3 & 0x1fffu) << 16);
    if (src2_neg) w |= (1ull << 30);
    if (s3_neg)   w |= (1ull << 31);
    w |= ((uint64_t)(dst_regid & 0xffu) << 32);
    if (flags & IR3_FLAG_SAT) w |= (1ull << 42);
    if (flags & IR3_FLAG_UL)  w |= (1ull << 45);
    w |= ((uint64_t)(src2_gpr & 0xffu) << 47);
    w |= ((uint64_t)(op & 0xfu) << 55);
    return w;
}

/* ---- Cat4: SFU --------------------------------------------------------- */

uint64_t ir3_enc_cat4(ir3_cat4_op op, uint32_t dst_regid,
                      ir3_src src, uint32_t flags)
{
    uint64_t w = (4ull << 61) | ir3_common_flags(flags);
    w |= (uint64_t)ir3_enc_multisrc(src);
    w |= ((uint64_t)(dst_regid & 0xffu) << 32);
    if (flags & IR3_FLAG_SAT) w |= (1ull << 42);
    if (flags & IR3_FLAG_UL)  w |= (1ull << 45);
    w |= (1ull << 52); /* full = 1 */
    w |= ((uint64_t)(op & 0x3fu) << 53);
    return w;
}

/* ---- Cat6: Global Load/Store (64-bit IOVA) ----------------------------- */

uint64_t ir3_enc_ldg(uint32_t dst_regid, uint32_t base_regid,
                     int32_t byte_off, uint32_t comps, ir3_type type,
                     uint32_t flags)
{
    uint64_t w = (6ull << 61) | ir3_common_flags(flags);
    uint32_t off13 = (uint32_t)byte_off & 0x1fffu;
    if (comps < 1u) comps = 1u;
    if (comps > 4u) comps = 4u;
    w |= (1ull << 0);                        /* pattern bit 0 = 1 */
    w |= ((uint64_t)off13 << 1);             /* bits 13:1 = signed offset */
    w |= ((uint64_t)(base_regid & 0xffu) << 14); /* bits 21:14 = src1 base */
    /* bit 22 = 0 (non-indexed) */
    w |= (1ull << 23);                       /* pattern bit 23 = 1 */
    w |= ((uint64_t)(comps & 7u) << 24);     /* bits 26:24 = size (1..4) */
    w |= ((uint64_t)(dst_regid & 0xffu) << 32);  /* bits 39:32 = dst */
    w |= ((uint64_t)(type & 7u) << 49);      /* bits 51:49 = type */
    /* bits 58:54 = 00000 (ldg) */
    return w;
}

uint64_t ir3_enc_stg(uint32_t base_regid, int32_t byte_off,
                     uint32_t val_regid, uint32_t comps, ir3_type type,
                     uint32_t flags)
{
    uint64_t w = (6ull << 61) | ir3_common_flags(flags);
    uint32_t off13 = (uint32_t)byte_off & 0x1fffu;
    if (comps < 1u) comps = 1u;
    if (comps > 4u) comps = 4u;
    w |= ((uint64_t)(val_regid & 0xffu) << 1);       /* bits 8:1 = src3 value */
    w |= ((uint64_t)((off13 >> 8) & 0x1fu) << 9);    /* bits 13:9 = off_hi */
    w |= (1ull << 23);                               /* pattern bit 23 = 1, bit 22 = 0 */
    w |= ((uint64_t)(comps & 7u) << 24);             /* bits 26:24 = size */
    w |= ((uint64_t)(off13 & 0xffu) << 32);          /* bits 39:32 = off_lo */
    w |= (1ull << 40);                               /* dst_off = 1 (signed off) */
    w |= ((uint64_t)(base_regid & 0xffu) << 41);     /* bits 48:41 = src1 base */
    w |= ((uint64_t)(type & 7u) << 49);              /* bits 51:49 = type */
    w |= (3ull << 54);                               /* bits 58:54 = 00011 (stg) */
    return w;
}

/* ---- Cat7: A7xx Compute Epilogue Sync (lock / unlock) ------------------ */

uint64_t ir3_enc_lock(uint32_t flags)
{
    return (7ull << 61) | ir3_common_flags(flags) |
           (7ull << 55) | (0x210ull << 45);
}

uint64_t ir3_enc_unlock(uint32_t flags)
{
    return (7ull << 61) | ir3_common_flags(flags) |
           (7ull << 55) | (0x250ull << 45);
}

/* ---- Buffer Emit Helpers ----------------------------------------------- */

bool ir3_emit_nop(ir3_asm_buf *b, uint32_t repeat, uint32_t flags)
{
    return ir3_asm_emit(b, ir3_enc_nop(repeat, flags));
}

bool ir3_emit_end(ir3_asm_buf *b)
{
    return ir3_asm_emit(b, ir3_enc_end(0));
}

bool ir3_emit_cs_end(ir3_asm_buf *b)
{
    /* Adreno 730 requires (sy)(ss)(jp)lock; unlock; before CS end so
     * in-flight global memory stores commit cleanly before wave retirement. */
    return ir3_asm_emit(b, ir3_enc_lock(IR3_FLAG_SY | IR3_FLAG_SS | IR3_FLAG_JP)) &&
           ir3_asm_emit(b, ir3_enc_unlock(0)) &&
           ir3_asm_emit(b, ir3_enc_end(0));
}

bool ir3_emit_mov_gpr(ir3_asm_buf *b, uint32_t dst, uint32_t src, ir3_type t, uint32_t flags)
{
    ir3_track_reg(b, dst);
    ir3_track_reg(b, src);
    return ir3_asm_emit(b, ir3_enc_mov_gpr(dst, src, t, t, 0, false, flags));
}

bool ir3_emit_mov_const(ir3_asm_buf *b, uint32_t dst, uint32_t c, ir3_type t, uint32_t flags)
{
    ir3_track_reg(b, dst);
    ir3_track_src(b, ir3_src_const(c));
    return ir3_asm_emit(b, ir3_enc_mov_const(dst, c, t, t, 0, false, flags));
}

bool ir3_emit_mov_imm_f32(ir3_asm_buf *b, uint32_t dst, float f, uint32_t flags)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    ir3_track_reg(b, dst);
    return ir3_asm_emit(b, ir3_enc_mov_imm(dst, bits, IR3_TYPE_F32, IR3_TYPE_F32, flags));
}

bool ir3_emit_mov_imm_u32(ir3_asm_buf *b, uint32_t dst, uint32_t u, uint32_t flags)
{
    ir3_track_reg(b, dst);
    return ir3_asm_emit(b, ir3_enc_mov_imm(dst, u, IR3_TYPE_U32, IR3_TYPE_U32, flags));
}

bool ir3_emit_cov(ir3_asm_buf *b, uint32_t dst, uint32_t src,
                  ir3_type dst_t, ir3_type src_t, uint32_t flags)
{
    ir3_track_reg(b, dst);
    ir3_track_reg(b, src);
    return ir3_asm_emit(b, ir3_enc_mov_gpr(dst, src, dst_t, src_t, 0, false, flags));
}

bool ir3_emit_cat2(ir3_asm_buf *b, ir3_cat2_op op, uint32_t dst,
                   ir3_src s1, ir3_src s2, ir3_cond cond, uint32_t flags)
{
    ir3_track_reg(b, dst);
    ir3_track_src(b, s1);
    ir3_track_src(b, s2);
    return ir3_asm_emit(b, ir3_enc_cat2(op, dst, s1, s2, cond, 0, false, false, flags));
}

bool ir3_emit_bary_f(ir3_asm_buf *b, uint32_t dst, uint32_t slot,
                     uint32_t ij_regid, uint32_t flags)
{
    ir3_track_reg(b, dst);
    ir3_track_reg(b, ij_regid);
    return ir3_asm_emit(b, ir3_enc_bary_f(dst, slot, ij_regid, 0, flags));
}

bool ir3_emit_cat3(ir3_asm_buf *b, ir3_cat3_op op, uint32_t dst,
                   ir3_src s1, uint32_t s2_gpr, bool s2_neg, ir3_src s3,
                   uint32_t flags)
{
    ir3_track_reg(b, dst);
    ir3_track_src(b, s1);
    ir3_track_reg(b, s2_gpr);
    ir3_track_src(b, s3);
    return ir3_asm_emit(b, ir3_enc_cat3(op, dst, s1, s2_gpr, s2_neg, s3, flags));
}

bool ir3_emit_cat4(ir3_asm_buf *b, ir3_cat4_op op, uint32_t dst,
                   ir3_src s1, uint32_t flags)
{
    ir3_track_reg(b, dst);
    ir3_track_src(b, s1);
    return ir3_asm_emit(b, ir3_enc_cat4(op, dst, s1, flags));
}

bool ir3_emit_ldg(ir3_asm_buf *b, uint32_t dst, uint32_t base,
                  int32_t off, uint32_t comps, ir3_type t, uint32_t flags)
{
    uint32_t i;
    for (i = 0; i < comps; i++)
        ir3_track_reg(b, dst + i);
    ir3_track_reg(b, base);
    ir3_track_reg(b, base + 1u);
    return ir3_asm_emit(b, ir3_enc_ldg(dst, base, off, comps, t, flags));
}

bool ir3_emit_stg(ir3_asm_buf *b, uint32_t base, int32_t off,
                  uint32_t val, uint32_t comps, ir3_type t, uint32_t flags)
{
    uint32_t i;
    ir3_track_reg(b, base);
    ir3_track_reg(b, base + 1u);
    for (i = 0; i < comps; i++)
        ir3_track_reg(b, val + i);
    return ir3_asm_emit(b, ir3_enc_stg(base, off, val, comps, t, flags));
}
