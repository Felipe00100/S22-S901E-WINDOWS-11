/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_smmu.h"

#include <string.h>
#ifdef _M_ARM64
#include <intrin.h>
#define SMMU_MB() __dmb(_ARM64_BARRIER_SY)
#else
#define SMMU_MB() do {} while (0)
#endif

#define PT_DESC_TYPE_MASK 0x3ull
#define PT_DESC_TYPE_PAGE 0x3ull
#define PT_DESC_TYPE_TABLE 0x3ull
#define PT_DESC_AF (1ull << 10)
#define PT_DESC_SH_INNER (3ull << 8)
#define PT_DESC_ATTR(idx) ((uint64_t)(idx) << 2)
#define PT_DESC_AP_RW (1ull << 6) /* AP[2:1] = 01: EL1 & EL0 Read/Write (unprivileged IB1/shader/buffer access!) */
#define PT_DESC_NS (1ull << 5)
#define PT_DESC_XN ((1ull << 53) | (1ull << 54))
#define PT_ADDR_MASK 0x0000fffffffff000ull

#define SMMU_SMR_VALID (1u << 31)
#define SMMU_S2CR_TYPE_TRANS (0u << 16)
#define SMMU_S2CR_CBNDX(n) ((n) & 0xffu)
#define SMMU_SCTLR_M (1u << 0)
#define SMMU_SCTLR_TRE (1u << 1)
#define SMMU_SCTLR_AFE (1u << 2)
#define SMMU_SCTLR_E (1u << 4)
#define SMMU_SCTLR_HUPCF (1u << 8)
#define SMMU_TCR_EAE (1u << 31)
#define SMMU_TCR_EPD1 (1u << 23)
#define SMMU_TCR_TG0_4K (0u << 14)
#define SMMU_TCR_SH0_INNER (3u << 12)
#define SMMU_TCR_ORGN0_WB (3u << 10)
#define SMMU_TCR_IRGN0_WB (3u << 8)
#define SMMU_TCR_T0SZ_39 (25u)
#define SMMU_CBAR_TYPE_S1_S2_BYPASS (1u << 16)
#define SMMU_CBAR_S1_MEMATTR_WB (0xfu << 12)
#define SMMU_CBAR_S1_BPSHCFG_NSH (3u << 8)
#define SMMU_CBA2R_VA64 (1u << 0)
#define SMMU_sCR0_CLIENTPD (1u << 0)
#define SMMU_sCR0_USFCFG (1u << 10)
#define SMMU_sCR0_GFRE (1u << 1)
#define SMMU_sCR0_GFIE (1u << 2)
#define SMMU_sCR0_GCFGFRE (1u << 4)
#define SMMU_sCR0_GCFGFIE (1u << 5)

void adreno_pt_pool_init(adreno_pt_pool *pool, void *base, size_t size,
                         uint64_t base_pa)
{
    uintptr_t p = (uintptr_t)base;
    size_t skip = (size_t)((0x1000 - (p & 0xfff)) & 0xfff);

    memset(pool, 0, sizeof(*pool));
    if (!base || size <= skip)
        return;
    pool->base = (uint8_t *)base + skip;
    pool->base_pa = base_pa + skip;
    pool->size = size - skip;
    pool->used = 0;
}

static uint64_t *pt_alloc_table(adreno_pt_pool *pool)
{
    uint8_t *p;

    if (!pool || !pool->base || pool->used + 4096 > pool->size)
        return NULL;
    p = pool->base + pool->used;
    memset(p, 0, 4096);
    pool->used += 4096;
    return (uint64_t *)p;
}

static uint64_t pt_table_pa(const adreno_pt *pt, const uint64_t *table)
{
    return pt->pool->base_pa +
           (uint64_t)((const uint8_t *)table - pt->pool->base);
}

static uint64_t *pt_pa_to_table(const adreno_pt *pt, uint64_t pa)
{
    uint64_t off;

    if (pa < pt->pool->base_pa)
        return NULL;
    off = pa - pt->pool->base_pa;
    if (off + 4096 > pt->pool->size)
        return NULL;
    return (uint64_t *)(pt->pool->base + off);
}

enum adreno_status adreno_pt_init(adreno_pt *pt, adreno_pt_pool *pool)
{
    uint64_t *root;

    if (!pt || !pool || !pool->base)
        return ADRENO_ERR_INVALID;
    root = pt_alloc_table(pool);
    if (!root)
        return ADRENO_ERR_NOMEM;
    ptrdiff_t off = (const uint8_t *)root - pool->base;
    pt->pool = pool;
    pt->root = root;
    pt->root_pa = pool->base_pa + (uint64_t)off;
    pt->mapped_pages = 0;
    pt->va_bits = 39;
    return ADRENO_OK;
}

static uint64_t pt_page_desc(uint64_t va, uint64_t pa, bool device)
{
    /* Kernel/FW exec windows (GMU icache 0x4000, ring 0x10000000, fence 0x10010000, SQE 0x10020000)
     * execute at EL1 where AP[1]=1 implies PXN, so use AP=00 (EL1 RWX).
     * IB (0x10040000) and all user allocations (>= 0x20000000) use AP=01 (EL1 RW + EL0 RWX). */
    uint64_t ap = (va < 0x10040000ull) ? (0ull << 6) : (1ull << 6);
    uint64_t desc = (pa & PT_ADDR_MASK) | PT_DESC_TYPE_PAGE | PT_DESC_AF | ap;
    if (device)
        desc |= PT_DESC_ATTR(1) | PT_DESC_XN;
    else
        desc |= PT_DESC_ATTR(0) | PT_DESC_SH_INNER | PT_DESC_NS;
    return desc;
}

enum adreno_status adreno_pt_map(adreno_pt *pt, uint64_t iova, uint64_t pa,
                                 uint64_t size, bool device)
{
    uint64_t off;

    if (!pt || !pt->root || (iova & 0xfff) || (pa & 0xfff) || !size)
        return ADRENO_ERR_INVALID;
    size = (size + 0xfff) & ~0xfffull;

    for (off = 0; off < size; off += 0x1000) {
        uint64_t va = iova + off;
        uint64_t phys = pa + off;
        uint32_t l1i = (uint32_t)((va >> 30) & 0x1ff);
        uint32_t l2i = (uint32_t)((va >> 21) & 0x1ff);
        uint32_t l3i = (uint32_t)((va >> 12) & 0x1ff);
        uint64_t *l2, *l3;
        uint64_t table_pa;

        if ((pt->root[l1i] & PT_DESC_TYPE_MASK) == 0) {
            l2 = pt_alloc_table(pt->pool);
            if (!l2)
                return ADRENO_ERR_NOMEM;
            table_pa = pt_table_pa(pt, l2);
            pt->root[l1i] = (table_pa & PT_ADDR_MASK) | PT_DESC_TYPE_TABLE;
        } else if ((pt->root[l1i] & PT_DESC_TYPE_MASK) !=
                   PT_DESC_TYPE_TABLE) {
            return ADRENO_ERR_STATE;
        }
        l2 = pt_pa_to_table(pt, pt->root[l1i] & PT_ADDR_MASK);
        if (!l2)
            return ADRENO_ERR_RANGE;

        if ((l2[l2i] & PT_DESC_TYPE_MASK) == 0) {
            l3 = pt_alloc_table(pt->pool);
            if (!l3)
                return ADRENO_ERR_NOMEM;
            table_pa = pt_table_pa(pt, l3);
            l2[l2i] = (table_pa & PT_ADDR_MASK) | PT_DESC_TYPE_TABLE;
        } else if ((l2[l2i] & PT_DESC_TYPE_MASK) != PT_DESC_TYPE_TABLE) {
            return ADRENO_ERR_STATE;
        }
        l3 = pt_pa_to_table(pt, l2[l2i] & PT_ADDR_MASK);
        if (!l3)
            return ADRENO_ERR_RANGE;

        if (l3[l3i] & 1) {
            if ((l3[l3i] & PT_ADDR_MASK) != (phys & PT_ADDR_MASK))
                return ADRENO_ERR_STATE;
            continue;
        }
        l3[l3i] = pt_page_desc(va, phys, device);
        pt->mapped_pages++;
    }
    return ADRENO_OK;
}

static enum adreno_status pt_lookup(adreno_pt *pt, uint64_t iova,
                                    uint64_t **l3_out, uint32_t *idx_out)
{
    uint32_t l1i = (uint32_t)((iova >> 30) & 0x1ff);
    uint32_t l2i = (uint32_t)((iova >> 21) & 0x1ff);
    uint32_t l3i = (uint32_t)((iova >> 12) & 0x1ff);
    uint64_t *l2, *l3;

    if (!pt || !pt->root)
        return ADRENO_ERR_INVALID;
    if ((pt->root[l1i] & 3) != PT_DESC_TYPE_TABLE)
        return ADRENO_ERR_RANGE;
    l2 = pt_pa_to_table(pt, pt->root[l1i] & PT_ADDR_MASK);
    if (!l2 || (l2[l2i] & 3) != PT_DESC_TYPE_TABLE)
        return ADRENO_ERR_RANGE;
    l3 = pt_pa_to_table(pt, l2[l2i] & PT_ADDR_MASK);
    if (!l3)
        return ADRENO_ERR_RANGE;
    *l3_out = l3;
    *idx_out = l3i;
    return ADRENO_OK;
}

uint64_t adreno_pt_walk(const adreno_pt *pt, uint64_t iova, bool *mapped,
                        bool *device)
{
    adreno_pt *p = (adreno_pt *)pt;
    uint64_t *l3;
    uint32_t idx;
    uint64_t desc;

    if (mapped)
        *mapped = false;
    if (device)
        *device = false;
    if (!pt || pt_lookup(p, iova, &l3, &idx) != ADRENO_OK)
        return 0;
    desc = l3[idx];
    if (!(desc & 1))
        return 0;
    if (mapped)
        *mapped = true;
    if (device)
        *device = ((desc >> 2) & 7) == 1;
    return (desc & PT_ADDR_MASK) | (iova & 0xfff);
}

enum adreno_status adreno_pt_unmap(adreno_pt *pt, uint64_t iova,
                                   uint64_t size)
{
    uint64_t off;

    if (!pt || !pt->root || (iova & 0xfff) || !size)
        return ADRENO_ERR_INVALID;
    size = (size + 0xfff) & ~0xfffull;
    for (off = 0; off < size; off += 0x1000) {
        uint64_t *l3;
        uint32_t idx;
        if (pt_lookup(pt, iova + off, &l3, &idx) != ADRENO_OK)
            continue;
        if (l3[idx] & 1) {
            l3[idx] = 0;
            if (pt->mapped_pages)
                pt->mapped_pages--;
        }
    }
    return ADRENO_OK;
}

/* ---- SMMU-500 ---------------------------------------------------------- */

static uint32_t smmu_read(adreno_smmu *smmu, uint32_t byte_offset)
{
    return adreno_read(smmu->gpu, ADRENO_SPACE_SMMU, byte_offset / 4);
}

static void smmu_write(adreno_smmu *smmu, uint32_t byte_offset,
                       uint32_t value)
{
    adreno_write(smmu->gpu, ADRENO_SPACE_SMMU, byte_offset / 4, value);
}

static uint32_t cb_offset(const adreno_smmu *smmu, uint32_t cb)
{
    return (smmu->numpage + cb) << smmu->pgshift;
}

enum adreno_status adreno_smmu_probe(adreno_smmu *smmu, adreno_a730 *gpu)
{
    uint32_t id0, id1, numpage_raw;

    if (!smmu || !gpu)
        return ADRENO_ERR_INVALID;
    memset(smmu, 0, sizeof(*smmu));
    smmu->gpu = gpu;
    id0 = smmu_read(smmu, ADRENO_SMMU_GR0_ID0);
    id1 = smmu_read(smmu, ADRENO_SMMU_GR0_ID1);
    if (id1 == 0xffffffffu)
        return ADRENO_ERR_IO;

    smmu->num_cbs = id1 & ADRENO_SMMU_ID1_NUMCB_MASK;
    numpage_raw = (id1 >> ADRENO_SMMU_ID1_NUMPAGENDXB_SHIFT) &
                  ADRENO_SMMU_ID1_NUMPAGENDXB_MASK;
    smmu->pgshift = (id1 & (1u << 31)) ? 16 : 12;
    smmu->numpage = 1u << (numpage_raw + 1);
    smmu->gpu_cb = 0;
    smmu->gmu_cb = 5;
    if (smmu->pgshift != 12)
        return ADRENO_ERR_UNSUPPORTED; /* 64K page tables not implemented */
    if (smmu->num_cbs <= smmu->gmu_cb)
        return ADRENO_ERR_UNSUPPORTED;
    if (smmu->numpage > 64)
        return ADRENO_ERR_RANGE;
    (void)id0;
    return ADRENO_OK;
}

static void smmu_cb_write(adreno_smmu *smmu, uint32_t cb, uint32_t reg,
                          uint32_t value)
{
    smmu_write(smmu, cb_offset(smmu, cb) + reg, value);
}

static void smmu_cb_write64(adreno_smmu *smmu, uint32_t cb, uint32_t reg,
                            uint64_t value)
{
    smmu_cb_write(smmu, cb, reg, (uint32_t)value);
    smmu_cb_write(smmu, cb, reg + 4, (uint32_t)(value >> 32));
}

void adreno_smmu_tlb_invalidate(adreno_smmu *smmu)
{
    uint32_t i;

    if (!smmu || !smmu->gpu)
        return;
    smmu_write(smmu, ADRENO_SMMU_GR0_TLBIALLNSNH, 0);
    smmu_write(smmu, ADRENO_SMMU_GR0_sTLBGSYNC, 0);
    for (i = 0; i < 100000; i++) {
        if (!(smmu_read(smmu, ADRENO_SMMU_GR0_sTLBGSTATUS) & 1))
            break;
        if (smmu->gpu->hw.udelay)
            smmu->gpu->hw.udelay(smmu->gpu->hw.ctx, 1);
    }
    {
        uint32_t cbs[2] = { smmu->gpu_cb, smmu->gmu_cb };
        for (i = 0; i < 2; i++) {
            smmu_cb_write(smmu, cbs[i], ADRENO_SMMU_CB_TLBSYNC, 0);
            (void)smmu_read(smmu, cb_offset(smmu, cbs[i]) + ADRENO_SMMU_CB_TLBSTATUS);
        }
    }
}

enum adreno_status adreno_smmu_setup(adreno_smmu *smmu, adreno_pt *gpu_pt,
                                     adreno_pt *gmu_pt, uint16_t gpu_sid,
                                     uint16_t gmu_sid)
{
    uint32_t i, scr0, num_smr;
    uint32_t id0;

    if (!smmu || !smmu->gpu || !gpu_pt || !gmu_pt)
        return ADRENO_ERR_INVALID;
    if (!gpu_pt->root_pa || !gmu_pt->root_pa || (gpu_pt->root_pa & 0xfff) ||
        (gmu_pt->root_pa & 0xfff))
        return ADRENO_ERR_INVALID;

    /* Disable translation before touching SMR/S2CR/CB registers. */
    scr0 = smmu_read(smmu, ADRENO_SMMU_GR0_sCR0);
    scr0 |= SMMU_sCR0_CLIENTPD;
    smmu_write(smmu, ADRENO_SMMU_GR0_sCR0, scr0);
    adreno_smmu_tlb_invalidate(smmu);

    /* Only clear the two HLOS context banks we own (gpu_cb and gmu_cb); never touch TZ-locked CBs */
    {
        uint32_t cbs[2] = { smmu->gpu_cb, smmu->gmu_cb };
        for (i = 0; i < 2; i++) {
            smmu_cb_write(smmu, cbs[i], ADRENO_SMMU_CB_SCTLR, 0);
            smmu_cb_write(smmu, cbs[i], ADRENO_SMMU_CB_FSR, 0xffffffffu);
        }
    }

    id0 = smmu_read(smmu, ADRENO_SMMU_GR0_ID0);
    num_smr = (id0 & 0xffu) + 1u;
    if (num_smr > 128)
        num_smr = 128;
    if (gpu_sid < num_smr) {
        smmu_write(smmu, ADRENO_SMMU_GR0_SMR(gpu_sid), SMMU_SMR_VALID | (0x400u << 16) | gpu_sid);
        smmu_write(smmu, ADRENO_SMMU_GR0_S2CR(gpu_sid),
                     SMMU_S2CR_TYPE_TRANS | SMMU_S2CR_CBNDX(smmu->gpu_cb));
    }
    /* SID 1 is the second GPU stream ID (TTBR1/non-secure alias). */
    if (gpu_sid + 1u < num_smr) {
        uint16_t sid = (uint16_t)(gpu_sid + 1u);
        smmu_write(smmu, ADRENO_SMMU_GR0_SMR(sid), SMMU_SMR_VALID | (0x400u << 16) | sid);
        smmu_write(smmu, ADRENO_SMMU_GR0_S2CR(sid),
                     SMMU_S2CR_TYPE_TRANS | SMMU_S2CR_CBNDX(smmu->gpu_cb));
    }
    if (gmu_sid < num_smr) {
        smmu_write(smmu, ADRENO_SMMU_GR0_SMR(gmu_sid), SMMU_SMR_VALID | (0x400u << 16) | gmu_sid);
        smmu_write(smmu, ADRENO_SMMU_GR0_S2CR(gmu_sid),
                     SMMU_S2CR_TYPE_TRANS | SMMU_S2CR_CBNDX(smmu->gmu_cb));
    }

    /* Program CBAR/CBA2R ONLY for our two HLOS context banks (gpu_cb, gmu_cb) */
    {
        uint32_t cbs[2] = { smmu->gpu_cb, smmu->gmu_cb };
        for (i = 0; i < 2; i++) {
            uint32_t cbar = SMMU_CBAR_TYPE_S1_S2_BYPASS |
                            SMMU_CBAR_S1_MEMATTR_WB |
                            SMMU_CBAR_S1_BPSHCFG_NSH;
            smmu_write(smmu, ADRENO_SMMU_GR1 + ADRENO_SMMU_GR1_CBAR(cbs[i]), cbar);
            smmu_write(smmu, ADRENO_SMMU_GR1 + ADRENO_SMMU_GR1_CBA2R(cbs[i]),
                         SMMU_CBA2R_VA64);
        }
    }

    for (i = 0; i < 2; i++) {
        uint32_t cb = i == 0 ? smmu->gpu_cb : smmu->gmu_cb;
        adreno_pt *pt = i == 0 ? gpu_pt : gmu_pt;
        /* Non-Cacheable PT walks matching MmWriteCombined (Normal NC) on non-coherent SM8450 */
        uint32_t tcr = SMMU_TCR_EAE | SMMU_TCR_TG0_4K |
                       SMMU_TCR_SH0_INNER | SMMU_TCR_EPD1 |
                       SMMU_TCR_T0SZ_39;

        {
            uint32_t id2 = smmu_read(smmu, 0x0028u); /* GR0_ID2 */
            uint32_t oas = (id2 >> 4) & 0x7u;        /* OAS -> PASIZE */
            uint32_t tcr2 = (0x7u << 15) | oas;      /* SEP_UPSTREAM | PASIZE */
            smmu_cb_write(smmu, cb, ADRENO_SMMU_CB_SCTLR, 0);
            smmu_cb_write(smmu, cb, 0x0010u, tcr2);  /* ARM_SMMU_CB_TCR2 */
            smmu_cb_write64(smmu, cb, ADRENO_SMMU_CB_TTBR0, pt->root_pa);
            smmu_cb_write64(smmu, cb, 0x0028u, 0ull); /* ARM_SMMU_CB_TTBR1 */
            smmu_cb_write(smmu, cb, ADRENO_SMMU_CB_TCR, tcr);
            smmu_cb_write(smmu, cb, ADRENO_SMMU_CB_S1_MAIR0, 0x00000044u); /* AttrIndx0 = Normal NC */
            smmu_cb_write(smmu, cb, ADRENO_SMMU_CB_S1_MAIR1, 0x00000000u);
            smmu_cb_write(smmu, cb, ADRENO_SMMU_CB_FSR, 0xffffffffu);
            SMMU_MB();
            /* Little-Endian! NEVER set SMMU_SCTLR_E (BIT 4 = Big-Endian) on ARM64! */
            smmu_cb_write(smmu, cb, ADRENO_SMMU_CB_SCTLR,
                          SMMU_SCTLR_M | SMMU_SCTLR_TRE | SMMU_SCTLR_AFE |
                              SMMU_SCTLR_HUPCF | (1u << 12) /* S1_ASIDPNE */);
        }
    }

    adreno_smmu_tlb_invalidate(smmu);

    scr0 = smmu_read(smmu, ADRENO_SMMU_GR0_sCR0);
    scr0 &= ~SMMU_sCR0_CLIENTPD;
    scr0 |= SMMU_sCR0_USFCFG | SMMU_sCR0_GFRE | SMMU_sCR0_GFIE |
            SMMU_sCR0_GCFGFRE | SMMU_sCR0_GCFGFIE;
    smmu_write(smmu, ADRENO_SMMU_GR0_sCR0, scr0);
    return ADRENO_OK;
}


