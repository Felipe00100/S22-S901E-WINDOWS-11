/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_a730.h"
#include "adreno/a730_regs.h"

#include <string.h>

#define CP_THREAD_CONTROL_CONCURRENT_BIN_DISABLE 0x08000000u
#define A730_CHIPID_GMU_FORMAT(chip)                                        \
    (((chip) & 0xffff0000u) | (((chip) << 4) & 0x0000f000u) |               \
     (((chip) << 8) & 0x00000f00u))

static void log_msg(adreno_a730 *g, const char *msg)
{
    if (g->hw.log)
        g->hw.log(g->hw.ctx, msg);
}

void adreno_a730_init(adreno_a730 *g, const adreno_hw_ops *hw,
                      uint32_t chip_id)
{
    memset(g, 0, sizeof(*g));
    if (hw)
        g->hw = *hw;
    g->chip_id = chip_id ? chip_id : ADRENO_CHIP_ID_A730;
    g->info = adreno_gpu_info_from_chip_id(g->chip_id);
    if (!g->info)
        g->info = &adreno_a730_info;
}

enum adreno_status adreno_a730_add_window(adreno_a730 *g, const char *name,
                                          adreno_iova_t iova, uint32_t size,
                                          void *host)
{
    if (!g || !name || !host || size == 0 ||
        g->num_windows >= ADRENO_A730_MAX_WINDOWS)
        return ADRENO_ERR_INVALID;
    g->windows[g->num_windows].name = name;
    g->windows[g->num_windows].iova = iova;
    g->windows[g->num_windows].size = size;
    g->windows[g->num_windows].host = (uint8_t *)host;
    g->num_windows++;
    return ADRENO_OK;
}

adreno_mem_window *adreno_a730_find_window(adreno_a730 *g, const char *name)
{
    uint32_t i;
    for (i = 0; i < g->num_windows; i++)
        if (strcmp(g->windows[i].name, name) == 0)
            return &g->windows[i];
    return NULL;
}

adreno_mem_window *adreno_a730_window_for_iova(adreno_a730 *g,
                                               adreno_iova_t iova,
                                               uint32_t size)
{
    uint32_t i;
    for (i = 0; i < g->num_windows; i++) {
        adreno_mem_window *w = &g->windows[i];
        if (iova >= w->iova && (uint64_t)iova + size <=
                                    (uint64_t)w->iova + w->size)
            return w;
    }
    return NULL;
}

enum adreno_status adreno_hw_poll(adreno_a730 *g, adreno_space space,
                                  uint32_t reg, uint32_t mask,
                                  uint32_t expected, uint32_t timeout_us)
{
    uint32_t waited = 0;

    for (;;) {
        uint32_t val = adreno_read(g, space, reg);
        if ((val & mask) == expected)
            return ADRENO_OK;
        if (waited >= timeout_us)
            return ADRENO_ERR_TIMEOUT;
        if (g->hw.udelay)
            g->hw.udelay(g->hw.ctx, 10);
        waited += 10;
    }
}

enum adreno_status adreno_a730_hw_init(adreno_a730 *g,
                                        adreno_iova_t uche_trap_iova)
{
    if (!g || !g->hw.read32 || !g->hw.write32)
        return ADRENO_ERR_INVALID;

    /* Clear GBIF halts in case the GX domain survived the bootloader. */
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_GBIF_HALT, 0);
    (void)adreno_read(g, ADRENO_SPACE_GPU, REG_A6XX_GBIF_HALT);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_GBIF_HALT, 0);
    (void)adreno_read(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_GBIF_HALT);

    /* Do not carve out a trusted memory range; we do not use secure GPU. */
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_SECVID_TSB_CNTL, 0);
    adreno_write64(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE,
                   0);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE,
                 0);

    /* A730 values from hw_init()'s adreno_is_a7xx() branches. */
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_GBIF_CLIENT_QOS_CNTL,
                 0x02120212u);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_UCHE_GBIF_GX_CONFIG,
                 0x010240e0u);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED,
                 0xffffffffu);
    if (uche_trap_iova) {
        adreno_write64(g, ADRENO_SPACE_GPU, REG_A6XX_UCHE_TRAP_BASE,
                       uche_trap_iova);
        adreno_write64(g, ADRENO_SPACE_GPU, REG_A6XX_UCHE_WRITE_THRU_BASE,
                       uche_trap_iova);
    }
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_UCHE_CACHE_WAYS,
                 ADRENO_BIT(23));
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_CP_AHB_CNTL, 1);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_PERFCTR_CNTL, 1);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_INTERFACE_HANG_INT_CNTL,
                 (1u << 30) | 0x00cfffffu);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_UCHE_CLIENT_PF,
                 ADRENO_BIT(7) | 1u);

    /* Leave RBBM interrupts masked until the DPC path is installed. */
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_RBBM_INT_0_MASK, 0);
    return ADRENO_OK;
}

enum adreno_status adreno_a730_gmu_load_fw(adreno_a730 *g,
                                           const adreno_fw_image *fw)
{
    uint32_t i;

    if (!g || !fw || !fw->data)
        return ADRENO_ERR_INVALID;

    for (i = 0; i < fw->count; i++) {
        const adreno_fw_block *b = &fw->blocks[i];
        const uint8_t *data = fw->data + b->data_offset;
        adreno_mem_window *w;

        if (b->size == 0)
            continue;

        if (b->region == ADRENO_FW_REGION_ITCM) {
            if (!g->hw.write_bulk)
                return ADRENO_ERR_UNSUPPORTED;
            g->hw.write_bulk(g->hw.ctx, ADRENO_SPACE_GMU,
                             REG_A6XX_GMU_CM3_ITCM_START +
                                 ((b->addr - ADRENO_GMU_ITCM_BASE) >> 2),
                             data, b->size);
        } else if (b->region == ADRENO_FW_REGION_DTCM) {
            if (!g->hw.write_bulk)
                return ADRENO_ERR_UNSUPPORTED;
            g->hw.write_bulk(g->hw.ctx, ADRENO_SPACE_GMU,
                             REG_A6XX_GMU_CM3_DTCM_START +
                                 ((b->addr - ADRENO_GMU_DTCM_BASE) >> 2),
                             data, b->size);
        } else {
            w = adreno_a730_window_for_iova(g, b->addr, b->size);
            if (!w || !w->host)
                return ADRENO_ERR_RANGE;
            memcpy(w->host + (b->addr - w->iova), data, b->size);
        }
    }
    log_msg(g, "GMU firmware blocks loaded");
    return ADRENO_OK;
}

static enum adreno_status gmu_rscc_wake(adreno_a730 *g)
{
    enum adreno_status st;

    /*
     * On a true cold boot the RSC is in its sleep state. The sequence is
     * harmless when the bootloader already woke it; failures are ignored by
     * the caller because some platforms do not expose RSCC at all.
     */
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_RSCC_CONTROL_REQ,
                 ADRENO_BIT(1));
    st = adreno_hw_poll(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_RSCC_CONTROL_ACK,
                        ADRENO_BIT(1), ADRENO_BIT(1), 10000);
    if (st != ADRENO_OK)
        return st;
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_RSCC_CONTROL_REQ, 0);
    return ADRENO_OK;
}

enum adreno_status adreno_a730_gmu_boot(adreno_a730 *g,
                                        const adreno_fw_image *fw,
                                        uint32_t boot_state,
                                        uint32_t timeout_us)
{
    adreno_mem_window *hfi = adreno_a730_find_window(g, "hfi");
    adreno_mem_window *log = adreno_a730_find_window(g, "log");
    adreno_mem_window *icache = adreno_a730_find_window(g, "icache");
    adreno_mem_window *dummy = adreno_a730_find_window(g, "dummy");
    uint32_t chipid, ver, mask, expected;
    enum adreno_status st;

    if (!hfi || hfi->size < ADRENO_HFI_TABLE_SIZE || !log || !icache ||
        !dummy)
        return ADRENO_ERR_INVALID;

    /* FAL10 veto and TCM retention (a6xx_gmu_fw_start). */
    adreno_write(g, ADRENO_SPACE_GMU,
                 REG_A6XX_GPU_GMU_CX_GMU_CX_FALNEXT_INTF, 1);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GPU_GMU_CX_GMU_CX_FAL_INTF, 1);
    adreno_write(g, ADRENO_SPACE_CX_MISC, REG_A7XX_CX_MISC_TCM_RET_CNTL, 1);

    st = gmu_rscc_wake(g);
    if (st != ADRENO_OK)
        log_msg(g, "RSCC wake timed out; assuming GX is already on");

    st = adreno_a730_gmu_load_fw(g, fw);
    if (st != ADRENO_OK)
        return st;

    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_CM3_FW_INIT_RESULT, 0);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_CM3_BOOT_CONFIG, 0x02);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_HFI_QTBL_ADDR,
                 (uint32_t)hfi->iova);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_HFI_QTBL_INFO, 1);

    /* A7xx fence range: upper 0x32, lower 0x8a0. */
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_AHB_FENCE_RANGE_0,
                 (1u << 31) | (0x32u << 18) | 0x8a0u);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_CM3_CFG, 0x4052);

    chipid = A730_CHIPID_GMU_FORMAT(g->chip_id);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A7XX_GMU_GENERAL_10, chipid);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A7XX_GMU_GENERAL_8,
                 ((uint32_t)log->iova & 0xfffff000u) |
                     ((log->size / 4096u - 1u) & 0xffu));

    /* GBIF CX configuration must precede GMU wake-up on A7xx. */
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_GBIF_QSB_SIDE0, 0x00071620);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_GBIF_QSB_SIDE1, 0x00071620);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_GBIF_QSB_SIDE2, 0x00071620);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_GBIF_QSB_SIDE3, 0x00071620);

    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_SYS_BUS_CONFIG, 0x1);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_ICACHE_CONFIG, 0x1);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_DCACHE_CONFIG, 0x1);

    /*
     * The last DTCM word tells the firmware ABI version. Very old firmware
     * reports <= 0x20010004 and uses the 0xbabeface status word.
     */
    ver = adreno_read(g, ADRENO_SPACE_GMU,
                      REG_A6XX_GMU_CM3_DTCM_START + 0xff8u);
    if (ver <= 0x20010004u) {
        mask = 0xffffffffu;
        expected = 0xbabefaceu;
    } else {
        mask = 0x1ffu;
        expected = 0x100u;
    }

    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_CM3_SYSRESET, 1);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A7XX_GMU_GENERAL_9, 0);
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_CM3_SYSRESET, 0);

    st = adreno_hw_poll(g, ADRENO_SPACE_GMU,
                        REG_A6XX_GMU_CM3_FW_INIT_RESULT, mask, expected,
                        timeout_us);
    if (st != ADRENO_OK)
        return st;

    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_HFI_CTRL_INIT, 1);
    st = adreno_hw_poll(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_HFI_CTRL_STATUS, 1,
                        1, 10000);
    if (st != ADRENO_OK)
        return st;

    g->gmu_booted = true;
    log_msg(g, "GMU booted and HFI queues started");
    (void)boot_state;
    return ADRENO_OK;
}

enum adreno_status adreno_a730_ring_init(adreno_a730 *g)
{
    adreno_mem_window *w;
    uint32_t size_dwords;

    if (!g)
        return ADRENO_ERR_INVALID;
    w = adreno_a730_find_window(g, "ring");
    if (!w || !w->host || w->size < 4096)
        return ADRENO_ERR_INVALID;
    size_dwords = w->size / 4;
    if (!adreno_is_pow2_u32(size_dwords))
        return ADRENO_ERR_INVALID;

    memset(w->host, 0, w->size);
    if (adreno_ring_init(&g->ring, (uint32_t *)w->host, size_dwords,
                         w->iova) != ADRENO_OK)
        return ADRENO_ERR_INVALID;
    g->ring_ready = true;
    return ADRENO_OK;
}

enum adreno_status adreno_a730_cp_start(adreno_a730 *g,
                                        adreno_iova_t pwrup_reglist_iova,
                                        uint32_t timeout_us)
{
    adreno_mem_window *sqe = adreno_a730_find_window(g, "sqe");
    adreno_mem_window *shadow = adreno_a730_find_window(g, "shadow");
    uint32_t *p;
    adreno_pm4 pm4;
    uint32_t wptr;
    enum adreno_status st;

    if (!g || !sqe || !g->ring_ready)
        return ADRENO_ERR_INVALID;

    adreno_write64(g, ADRENO_SPACE_GPU, REG_A6XX_CP_SQE_INSTR_BASE,
                   sqe->iova);
    adreno_write64(g, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_BASE,
                   g->ring.iova);
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_CNTL,
                 adreno_ring_rb_cntl(g->ring.size_dwords));
    if (shadow && shadow->size >= 0x100) {
        memset(shadow->host, 0, shadow->size);
        adreno_write64(g, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_RPTR_ADDR,
                       shadow->iova + 0x80);
        adreno_write64(g, ADRENO_SPACE_GPU, REG_A7XX_CP_BV_RB_RPTR_ADDR,
                       shadow->iova + 0x90);
    }
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_CP_SQE_CNTL, 1);

    /* a7xx_cp_init(): disable concurrent binning, then CP_ME_INIT. */
    st = adreno_ring_reserve(&g->ring, 32, &p);
    if (st != ADRENO_OK)
        return st;
    adreno_pm4_init(&pm4, p, 32);
    if (!adreno_pm4_thread_control(&pm4,
                                   CP_THREAD_CONTROL_CONCURRENT_BIN_DISABLE) ||
        !adreno_pm4_me_init(&pm4, pwrup_reglist_iova)) {
        g->ring.pending = false;
        return ADRENO_ERR_OVERFLOW;
    }
    adreno_ring_commit(&g->ring, pm4.count);
    adreno_a730_ring_kick(g, &g->ring);

    wptr = g->ring.wptr;
    st = adreno_hw_poll(g, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_RPTR,
                        0xffffffffu, wptr, timeout_us);
    if (st != ADRENO_OK)
        return st;

    g->cp_started = true;
    log_msg(g, "CP started and ME_INIT completed");
    return ADRENO_OK;
}

static enum adreno_status hfi_send_wait(adreno_a730 *g, adreno_hfi *hfi,
                                        uint32_t id, uint32_t *msg,
                                        uint32_t dwords, uint32_t timeout_us)
{
    uint32_t seq = 0;
    uint32_t waited = 0;
    uint32_t reply[64];
    uint32_t reply_dwords;
    enum adreno_status st;

    st = adreno_hfi_send(hfi, id, msg, dwords, &seq);
    if (st != ADRENO_OK)
        return st;
    adreno_write(g, ADRENO_SPACE_GMU, REG_A6XX_GMU_HOST2GMU_INTR_SET, 1);

    for (;;) {
        reply_dwords = 0;
        st = adreno_hfi_queue_read(hfi, ADRENO_HFI_RESPONSE_QUEUE, reply,
                                   ADRENO_ARRAY_SIZE(reply), &reply_dwords);
        if (st == ADRENO_OK && reply_dwords) {
            st = adreno_hfi_decode_ack(reply, reply_dwords, seq, NULL, NULL,
                                       NULL);
            if (st == ADRENO_OK)
                return ADRENO_OK;
            if (st == ADRENO_ERR_FIRMWARE)
                return st;
        }
        if (waited >= timeout_us)
            return ADRENO_ERR_TIMEOUT;
        if (g->hw.udelay)
            g->hw.udelay(g->hw.ctx, 10);
        waited += 10;
    }
}

enum adreno_status adreno_a730_hfi_start(adreno_a730 *g, adreno_hfi *hfi,
                                         uint32_t timeout_us)
{
    adreno_hfi_perf_table perf;
    adreno_hfi_bw_table bw;
    adreno_hfi_msg_core_fw_start core;
    adreno_hfi_msg_start start;
    enum adreno_status st;

    if (!g || !hfi || !g->gmu_booted)
        return ADRENO_ERR_STATE;

    /*
     * Non-legacy GMU firmware expects the performance table as its first
     * HFI command (a6xx_hfi_start). A single safe level is enough to bring
     * the firmware up; proper OPP/RPMh votes are a later power-management
     * step.
     */
    memset(&perf, 0, sizeof(perf));
    perf.num_gpu_levels = 1;
    perf.num_gmu_levels = 1;
    perf.gx_votes[0].vote = 0;
    perf.gx_votes[0].acd = 0xffffffffu;
    perf.gx_votes[0].freq = 818000; /* kHz */
    perf.cx_votes[0].vote = 0;
    perf.cx_votes[0].freq = 500000; /* kHz */
    st = hfi_send_wait(g, hfi, HFI_H2F_MSG_PERF_TABLE, (uint32_t *)&perf,
                       (uint32_t)(sizeof(perf) / 4), timeout_us);
    if (st != ADRENO_OK)
        return st;

    memset(&bw, 0, sizeof(bw));
    bw.bw_level_num = 1;
    bw.ddr_cmds_num = 0;
    bw.cnoc_cmds_num = 0;
    st = hfi_send_wait(g, hfi, HFI_H2F_MSG_BW_TABLE, (uint32_t *)&bw,
                       (uint32_t)(sizeof(bw) / 4), timeout_us);
    if (st != ADRENO_OK)
        return st;

    memset(&core, 0, sizeof(core));
    core.handle = 0;
    st = hfi_send_wait(g, hfi, HFI_H2F_MSG_CORE_FW_START, (uint32_t *)&core,
                       (uint32_t)(sizeof(core) / 4), timeout_us);
    if (st != ADRENO_OK)
        return st;

    memset(&start, 0, sizeof(start));
    st = hfi_send_wait(g, hfi, HFI_H2F_MSG_START, (uint32_t *)&start,
                       (uint32_t)(sizeof(start) / 4), timeout_us);
    if (st != ADRENO_OK)
        return st;

    log_msg(g, "HFI startup handshake complete");
    return ADRENO_OK;
}

void adreno_a730_ring_kick(adreno_a730 *g, adreno_ring *ring)
{
    if (!g || !ring)
        return;
    adreno_write(g, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_WPTR, ring->wptr);
}

enum adreno_status adreno_a730_submit(adreno_a730 *g, adreno_ring *ring,
                                      const adreno_ib *ibs, uint32_t num_ibs,
                                      uint32_t seqno)
{
    adreno_mem_window *fence;
    adreno_submit_desc desc;
    uint32_t *p;
    adreno_pm4 pm4;
    enum adreno_status st;

    if (!g || !ring || !g->cp_started)
        return ADRENO_ERR_STATE;
    fence = adreno_a730_find_window(g, "fence");
    if (!fence || fence->size < 0x100)
        return ADRENO_ERR_INVALID;

    /* A submit needs at most 2 + 2 + 2 + 4*num_ibs + ~30 dwords. */
    st = adreno_ring_reserve(ring, 48 + 4 * num_ibs, &p);
    if (st != ADRENO_OK)
        return st;
    adreno_pm4_init(&pm4, p, ring->pending_dwords);
    desc.ibs = ibs;
    desc.num_ibs = num_ibs;
    desc.seqno = seqno;
    desc.fence_iova = fence->iova;
    desc.bv_fence_iova = fence->iova + 0x40;
    st = adreno_pm4_emit_submit(&pm4, &desc);
    if (st != ADRENO_OK) {
        ring->pending = false;
        ring->pending_dwords = 0;
        return st;
    }
    adreno_ring_commit(ring, pm4.count);
    adreno_a730_ring_kick(g, ring);
    return ADRENO_OK;
}
