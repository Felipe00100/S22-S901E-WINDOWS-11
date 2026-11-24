/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_d3d12.h"
#include "adreno/adreno_ir3_ref.h"

#include <string.h>

const adreno_d3d12_ir3_meta adreno_d3d12_a730_test_meta = {
    ADRENO_IR3_TEST_SHADER_UNITS, ADRENO_IR3_TEST_CONSTLEN,
    ADRENO_IR3_TEST_FULLREG,     ADRENO_IR3_TEST_HALFREG,
    ADRENO_IR3_TEST_BRANCHSTACK, ADRENO_IR3_TEST_THREADSIZE,
    ADRENO_IR3_TEST_SHARED_SIZE, ADRENO_IR3_TEST_CONST_RAM_MODE,
    1u /* mergedregs */,         0u /* localid_regid r0.x */,
    0x00fcu /* wgid_constid INVALID_REG */,
    {1u, 1u, 1u},
};

enum adreno_status adreno_d3d12_device_init(adreno_d3d12_device *dev,
                                             const adreno_kmd_ops *ops)
{
    if (!dev || !ops || !ops->alloc || !ops->free || !ops->submit ||
        !ops->wait)
        return ADRENO_ERR_INVALID;
    memset(dev, 0, sizeof(*dev));
    dev->kmd = *ops;
    return ADRENO_OK;
}

void adreno_d3d12_device_finish(adreno_d3d12_device *dev)
{
    if (dev)
        memset(dev, 0, sizeof(*dev));
}

enum adreno_status adreno_d3d12_resource_create(adreno_d3d12_device *dev,
                                                 uint32_t size,
                                                 adreno_d3d12_resource *res)
{
    uint32_t handle = 0;
    uint64_t iova = 0;
    void *host = NULL;
    enum adreno_status st;

    if (!dev || !res || size == 0 || size > 256u * 1024u * 1024u)
        return ADRENO_ERR_INVALID;
    size = (uint32_t)ADRENO_ALIGN_UP(size, 4096);
    st = dev->kmd.alloc(dev->kmd.ctx, size, &handle, &iova, &host);
    if (st != ADRENO_OK)
        return st;
    memset(res, 0, sizeof(*res));
    res->device = dev;
    res->handle = handle;
    res->iova = iova;
    res->host = host;
    res->size = size;
    return ADRENO_OK;
}

void adreno_d3d12_resource_destroy(adreno_d3d12_resource *res)
{
    if (!res || !res->device)
        return;
    if (res->handle)
        (void)res->device->kmd.free(res->device->kmd.ctx, res->handle);
    memset(res, 0, sizeof(*res));
}

enum adreno_status adreno_d3d12_cmdlist_create(adreno_d3d12_device *dev,
                                                uint32_t capacity_dwords,
                                                adreno_d3d12_cmdlist *cmd)
{
    enum adreno_status st;

    if (!dev || !cmd || capacity_dwords < 64)
        return ADRENO_ERR_INVALID;
    memset(cmd, 0, sizeof(*cmd));
    st = adreno_d3d12_resource_create(dev, capacity_dwords * 4u, &cmd->cmd_bo);
    if (st != ADRENO_OK)
        return st;
    cmd->device = dev;
    adreno_pm4_init(&cmd->pm4, (uint32_t *)cmd->cmd_bo.host,
                    capacity_dwords);
    return ADRENO_OK;
}

void adreno_d3d12_cmdlist_destroy(adreno_d3d12_cmdlist *cmd)
{
    if (!cmd)
        return;
    adreno_d3d12_resource_destroy(&cmd->cmd_bo);
    memset(cmd, 0, sizeof(*cmd));
}

void adreno_d3d12_cmdlist_reset(adreno_d3d12_cmdlist *cmd)
{
    if (cmd)
        adreno_pm4_reset(&cmd->pm4);
}

enum adreno_status adreno_d3d12_cmdlist_reserve(adreno_d3d12_cmdlist *cmd,
                                                 uint32_t extra_dwords)
{
    adreno_d3d12_cmdlist grown;
    uint64_t needed;
    uint32_t capacity;
    enum adreno_status st;

    if (!cmd || !cmd->device)
        return ADRENO_ERR_INVALID;
    if (cmd->pm4.overflow)
        return ADRENO_ERR_OVERFLOW;
    needed = (uint64_t)cmd->pm4.count + extra_dwords;
    if (needed <= cmd->pm4.capacity)
        return ADRENO_OK;
    if (needed > 16u * 1024u * 1024u)
        return ADRENO_ERR_RANGE;

    capacity = cmd->pm4.capacity ? cmd->pm4.capacity : 1024;
    while (capacity < needed) {
        if (capacity > 8u * 1024u * 1024u)
            return ADRENO_ERR_NOMEM;
        capacity *= 2;
    }

    st = adreno_d3d12_cmdlist_create(cmd->device, capacity, &grown);
    if (st != ADRENO_OK)
        return st;
    if (cmd->pm4.count) {
        memcpy(grown.cmd_bo.host, cmd->cmd_bo.host, cmd->pm4.count * 4u);
        grown.pm4.count = cmd->pm4.count;
    }
    adreno_d3d12_cmdlist_destroy(cmd);
    *cmd = grown;
    return ADRENO_OK;
}

enum adreno_status adreno_d3d12_cmd_clear_u32(adreno_d3d12_cmdlist *cmd,
                                               adreno_d3d12_resource *res,
                                               uint32_t offset_bytes,
                                               uint32_t dwords,
                                               uint32_t color)
{
    uint32_t i;
    enum adreno_status st;

    if (!cmd || !res || !res->device || dwords == 0 ||
        (offset_bytes & 3) != 0)
        return ADRENO_ERR_INVALID;
    if ((uint64_t)offset_bytes + (uint64_t)dwords * 4 > res->size)
        return ADRENO_ERR_RANGE;

    st = adreno_d3d12_cmdlist_reserve(cmd, dwords * 4u + 8u);
    if (st != ADRENO_OK)
        return st;

    /*
     * CP_MEM_WRITE is the smallest reliable clear primitive available without
     * a shader. A future UMD can replace this with CP_BLIT/fill or a compute
     * dispatch; the submission/fence path stays the same.
     */
    for (i = 0; i < dwords; i++) {
        if (!adreno_pm4_mem_write(&cmd->pm4,
                                  res->iova + offset_bytes + (uint64_t)i * 4,
                                  color))
            return ADRENO_ERR_OVERFLOW;
    }
    return ADRENO_OK;
}

enum adreno_status adreno_d3d12_cmd_copy_u32(adreno_d3d12_cmdlist *cmd,
                                              adreno_d3d12_resource *dst,
                                              uint32_t dst_offset,
                                              adreno_d3d12_resource *src,
                                              uint32_t src_offset,
                                              uint32_t dwords)
{
    if (!cmd || !dst || !src || dwords == 0 ||
        (dst_offset & 3) || (src_offset & 3))
        return ADRENO_ERR_INVALID;
    if ((uint64_t)dst_offset + (uint64_t)dwords * 4 > dst->size ||
        (uint64_t)src_offset + (uint64_t)dwords * 4 > src->size)
        return ADRENO_ERR_RANGE;
    if (adreno_d3d12_cmdlist_reserve(cmd, 8) != ADRENO_OK)
        return ADRENO_ERR_OVERFLOW;
    if (!adreno_pm4_memcpy(&cmd->pm4, dst->iova + dst_offset,
                           src->iova + src_offset, dwords))
        return ADRENO_ERR_OVERFLOW;
    return ADRENO_OK;
}

enum adreno_status adreno_d3d12_submit(adreno_d3d12_device *dev,
                                       adreno_d3d12_cmdlist *cmd,
                                       uint32_t seqno)
{
    adreno_d3d12_cmdlist *lists[1];

    if (!cmd)
        return ADRENO_ERR_INVALID;
    lists[0] = cmd;
    return adreno_d3d12_submit_lists(dev, lists, 1, seqno);
}

enum adreno_status adreno_d3d12_submit_lists(adreno_d3d12_device *dev,
                                              adreno_d3d12_cmdlist *const *cmds,
                                              uint32_t count, uint32_t seqno)
{
    adreno_ib ibs[32];
    uint32_t i, n = 0;
    enum adreno_status st;

    if (!dev || !cmds || count == 0 || count > 32)
        return ADRENO_ERR_INVALID;
    for (i = 0; i < count; i++) {
        adreno_d3d12_cmdlist *cmd = cmds[i];
        if (!cmd || !cmd->cmd_bo.handle)
            return ADRENO_ERR_INVALID;
        if (cmd->pm4.overflow)
            return ADRENO_ERR_OVERFLOW;
        if (cmd->pm4.count == 0)
            continue;
        st = adreno_pm4_validate((const uint32_t *)cmd->cmd_bo.host,
                                 cmd->pm4.count);
        if (st != ADRENO_OK)
            return st;
        ibs[n].iova = cmd->cmd_bo.iova;
        ibs[n].size_dwords = cmd->pm4.count;
        ibs[n].handle = cmd->cmd_bo.handle;
        n++;
    }
    if (seqno == 0)
        seqno = ++dev->next_seqno;
    else if (seqno > dev->next_seqno)
        dev->next_seqno = seqno;
    if (n == 0)
        return ADRENO_OK;
    st = dev->kmd.submit(dev->kmd.ctx, ibs, n, seqno);
    if (st != ADRENO_OK)
        return st;
    dev->next_seqno = seqno;
    return ADRENO_OK;
}

enum adreno_status adreno_d3d12_wait(adreno_d3d12_device *dev,
                                     uint32_t seqno, uint32_t timeout_ms)
{
    if (!dev || !dev->kmd.wait || seqno == 0)
        return ADRENO_ERR_INVALID;
    return dev->kmd.wait(dev->kmd.ctx, seqno, timeout_ms);
}

enum adreno_status adreno_d3d12_resource_read(adreno_d3d12_resource *res,
                                               uint32_t offset, void *dst,
                                               uint32_t size)
{
    if (!res || !dst || (uint64_t)offset + size > res->size)
        return ADRENO_ERR_RANGE;
    if (res->host) {
        memcpy(dst, (const uint8_t *)res->host + offset, size);
        return ADRENO_OK;
    }
    if (res->device && res->device->kmd.read)
        return res->device->kmd.read(res->device->kmd.ctx, res->handle,
                                     offset, dst, size);
    return ADRENO_ERR_UNSUPPORTED;
}

enum adreno_status adreno_d3d12_cmd_dispatch_ir3(
    adreno_d3d12_cmdlist *cmd, const adreno_d3d12_resource *shader,
    const adreno_d3d12_ir3_meta *meta, const uint32_t *constants,
    uint32_t const_vec4s, const uint32_t grid[3])
{
    adreno_pm4 *pm4;
    uint32_t cntl0, cntl1, wge, wie0, wie1, nd0, nd7;
    uint32_t tile_h, work_dim;
    uint64_t gx, gy, gz;
    enum adreno_status st;

    if (!cmd || !cmd->device || !shader || !meta || !constants || !grid)
        return ADRENO_ERR_INVALID;
    if (meta->instrlen_units == 0 || meta->instrlen_units > 0x3ffu ||
        meta->constlen == 0 || meta->constlen > 256u ||
        const_vec4s == 0 || const_vec4s > 256u)
        return ADRENO_ERR_RANGE;
    if ((uint64_t)meta->instrlen_units * ADRENO_SHADER_UNIT_BYTES >
        shader->size)
        return ADRENO_ERR_RANGE;
    if (meta->fullregfootprint > 31u || meta->halfregfootprint > 63u ||
        meta->branchstack > 127u || meta->threadsize > 1u ||
        meta->shared_size > 31u || meta->constantrammode > 3u)
        return ADRENO_ERR_RANGE;
    if (meta->local_size[0] == 0 || meta->local_size[0] > 1024u ||
        meta->local_size[1] == 0 || meta->local_size[1] > 1024u ||
        meta->local_size[2] == 0 || meta->local_size[2] > 1024u ||
        grid[0] == 0 || grid[1] == 0 || grid[2] == 0)
        return ADRENO_ERR_INVALID;
    if ((uint64_t)meta->local_size[0] * meta->local_size[1] *
            meta->local_size[2] >
        1024u)
        return ADRENO_ERR_RANGE;
    gx = (uint64_t)meta->local_size[0] * grid[0];
    gy = (uint64_t)meta->local_size[1] * grid[1];
    gz = (uint64_t)meta->local_size[2] * grid[2];
    if (gx > 0xffffffffu || gy > 0xffffffffu || gz > 0xffffffffu)
        return ADRENO_ERR_RANGE;

    st = adreno_d3d12_cmdlist_reserve(cmd, 512u + const_vec4s * 4u);
    if (st != ADRENO_OK)
        return st;
    pm4 = &cmd->pm4;

    /*
     * Silicon-proven A730 compute dispatch sequence (avoids privileged
     * kernel-only chicken/UCHE registers from user-mode IBs).
     */
    cntl0 = ((meta->halfregfootprint & 0x3fu) << 1) |
            ((meta->fullregfootprint & 0x1fu) << 7) |
            ((meta->branchstack & 0x7fu) << 13) | ((meta->threadsize & 1u)
                                                   << 20) |
            ((meta->mergedregs & 1u) << 31);
    cntl1 = (meta->shared_size & 0x1fu) |
            ((meta->constantrammode & 3u) << 5);
    tile_h = (meta->local_size[1] % 8u == 0)   ? 3u
             : (meta->local_size[1] % 4u == 0) ? 5u
             : (meta->local_size[1] % 2u == 0) ? 9u
                                               : 17u;
    wge = 0x00fcu | ((meta->threadsize & 1u) << 9) | (1u << 11) |
          (4u << 20) | (tile_h << 26);
    wie0 = (meta->wgid_constid & 0xffu) | (0x00fcu << 8) | (0x00fcu << 16) |
           ((meta->localid_regid & 0xffu) << 24);
    wie1 = 0x00fcu | ((meta->threadsize & 1u) << 8);

    if (!adreno_pm4_wait_for_idle(pm4) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_MODE_CNTL, 0x5u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_UPDATE_CNTL, 0xbfu) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_CONST_CONFIG,
                              ((meta->constlen >> 2) & 0xffu) | 0x100u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_CONFIG, 0x100u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_BOOLEAN_CF_MASK, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_TSIZE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_USIZE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_PROGRAM_COUNTER_OFFSET, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_VGS_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_PVT_MEM_PARAM, 0u) ||
        !adreno_pm4_reg_write64(pm4, REG_A6XX_SP_CS_PVT_MEM_BASE, 0ull) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_PVT_MEM_SIZE, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_PVT_MEM_STACK_OFFSET, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_INSTR_SIZE,
                              meta->instrlen_units) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_CNTL_0, cntl0) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_PS_WAVE_CNTL, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_REG_PROG_ID_0, 0xfcfcfcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_REG_PROG_ID_1, 0xfcfcfcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_REG_PROG_ID_2, 0xfcfcfcfcu) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_REG_PROG_ID_3, 0x0000fc00u) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_CNTL_1, cntl1) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_WGE_CNTL, wge) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_WIE_CNTL_0, wie0) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_WIE_CNTL_1, wie1))
        return ADRENO_ERR_OVERFLOW;
    if (!adreno_pm4_reg_write64(pm4, REG_A6XX_SP_CS_BASE, shader->iova) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_INSTR_SIZE,
                              meta->instrlen_units) ||
        !adreno_pm4_reg_write(pm4, REG_A6XX_SP_CS_HYSTERESIS, 0u))
        return ADRENO_ERR_OVERFLOW;

    if (!adreno_pm4_load_state6_shader(pm4, ADRENO_SB6_CS_SHADER,
                                       shader->iova,
                                       meta->instrlen_units) ||
        !adreno_pm4_load_state6_const(pm4, ADRENO_SB6_CS_SHADER, 0,
                                      constants, const_vec4s))
        return ADRENO_ERR_OVERFLOW;

    if (!adreno_pm4_set_marker(pm4, 8u)) /* RM6_COMPUTE */
        return ADRENO_ERR_OVERFLOW;

    work_dim = 3u;
    nd0 = work_dim | ((meta->local_size[0] - 1u) << 2) |
          ((meta->local_size[1] - 1u) << 12) |
          ((meta->local_size[2] - 1u) << 22);
    nd7 = ((meta->local_size[0] - 1u) << 2) |
          ((meta->local_size[1] - 1u) << 12) |
          ((meta->local_size[2] - 1u) << 22);

    if (!adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_NDRANGE_0, nd0) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_NDRANGE_7, nd7) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_NDRANGE_1,
                              (uint32_t)gx) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_NDRANGE_2, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_NDRANGE_3,
                              (uint32_t)gy) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_NDRANGE_4, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_NDRANGE_5,
                              (uint32_t)gz) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_NDRANGE_6, 0u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_KERNEL_GROUP_X, 1u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_KERNEL_GROUP_Y, 1u) ||
        !adreno_pm4_reg_write(pm4, REG_A7XX_SP_CS_KERNEL_GROUP_Z, 1u))
        return ADRENO_ERR_OVERFLOW;

    if (!adreno_pm4_exec_cs(pm4, grid[0], grid[1], grid[2]) ||
        !adreno_pm4_wait_for_idle(pm4) ||
        !adreno_pm4_event_write_1(pm4, CACHE_CLEAN) ||
        !adreno_pm4_event_write_1(pm4, CACHE_INVALIDATE7) ||
        !adreno_pm4_emit_pkt7(pm4, CP_WAIT_MEM_WRITES, 0) ||
        !adreno_pm4_wait_for_idle(pm4))
        return ADRENO_ERR_OVERFLOW;

    return pm4->overflow ? ADRENO_ERR_OVERFLOW : ADRENO_OK;
}
