/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_pm4.h"

#include <string.h>

/* Fields from adreno_pm4.xml / a6xx_catalog.c. */
#define CP_THREAD_CONTROL_SYNC_THREADS 0x80000000u
#define CP_THREAD_CONTROL_CONCURRENT_BIN_DISABLE 0x08000000u
#define CP_EVENT_WRITE_IRQ 0x80000000u
#define CP_EVENT_WRITE_TIMESTAMP 0x40000000u
#define CP_EVENT_WRITE_CACHE_CLEAN_BV 0x08000000u
#define CP_EVENT_WRITE_CACHE_FLUSH_TS 4u

void adreno_pm4_init(adreno_pm4 *pm4, uint32_t *data, uint32_t capacity_dwords)
{
    pm4->data = data;
    pm4->capacity = data ? capacity_dwords : 0;
    pm4->count = 0;
    pm4->overflow = false;
}

void adreno_pm4_reset(adreno_pm4 *pm4)
{
    pm4->count = 0;
    pm4->overflow = false;
}

bool adreno_pm4_emit(adreno_pm4 *pm4, uint32_t value)
{
    if (pm4->overflow || pm4->count >= pm4->capacity) {
        pm4->overflow = true;
        return false;
    }
    pm4->data[pm4->count++] = value;
    return true;
}

bool adreno_pm4_emit_pkt4(adreno_pm4 *pm4, uint32_t reg, uint32_t count)
{
    if (count > 0xfffu) {
        pm4->overflow = true;
        return false;
    }
    return adreno_pm4_emit(pm4, ADRENO_PM4_TYPE4_HEADER(reg, count));
}

bool adreno_pm4_emit_pkt7(adreno_pm4 *pm4, uint32_t opcode, uint32_t count)
{
    if (count > 0xfffu) {
        pm4->overflow = true;
        return false;
    }
    return adreno_pm4_emit(pm4, ADRENO_PM4_TYPE7_HEADER(opcode, count));
}

bool adreno_pm4_reg_write(adreno_pm4 *pm4, uint32_t reg, uint32_t value)
{
    return adreno_pm4_emit_pkt4(pm4, reg, 1) && adreno_pm4_emit(pm4, value);
}

bool adreno_pm4_reg_write64(adreno_pm4 *pm4, uint32_t reg, uint64_t value)
{
    return adreno_pm4_emit_pkt4(pm4, reg, 2) &&
           adreno_pm4_emit(pm4, (uint32_t)value) &&
           adreno_pm4_emit(pm4, (uint32_t)(value >> 32));
}

bool adreno_pm4_indirect_buffer(adreno_pm4 *pm4, adreno_iova_t iova,
                                uint32_t size_dwords)
{
    return adreno_pm4_emit_pkt7(pm4, CP_INDIRECT_BUFFER, 3) &&
           adreno_pm4_emit(pm4, (uint32_t)iova) &&
           adreno_pm4_emit(pm4, (uint32_t)(iova >> 32)) &&
           adreno_pm4_emit(pm4, size_dwords & 0x000fffffu);
}

bool adreno_pm4_event_write(adreno_pm4 *pm4, uint32_t event, uint32_t flags,
                            adreno_iova_t iova, uint32_t value)
{
    return adreno_pm4_emit_pkt7(pm4, CP_EVENT_WRITE, 4) &&
           adreno_pm4_emit(pm4, event | flags) &&
           adreno_pm4_emit(pm4, (uint32_t)iova) &&
           adreno_pm4_emit(pm4, (uint32_t)(iova >> 32)) &&
           adreno_pm4_emit(pm4, value);
}

bool adreno_pm4_event_write_1(adreno_pm4 *pm4, uint32_t value)
{
    return adreno_pm4_emit_pkt7(pm4, CP_EVENT_WRITE, 1) &&
           adreno_pm4_emit(pm4, value);
}

bool adreno_pm4_mem_write(adreno_pm4 *pm4, adreno_iova_t iova, uint32_t value)
{
    return adreno_pm4_emit_pkt7(pm4, CP_MEM_WRITE, 3) &&
           adreno_pm4_emit(pm4, (uint32_t)iova) &&
           adreno_pm4_emit(pm4, (uint32_t)(iova >> 32)) &&
           adreno_pm4_emit(pm4, value);
}

bool adreno_pm4_memcpy(adreno_pm4 *pm4, adreno_iova_t dst, adreno_iova_t src,
                       uint32_t dwords)
{
    if (dwords == 0)
        return true;
    return adreno_pm4_emit_pkt7(pm4, CP_MEMCPY, 5) &&
           adreno_pm4_emit(pm4, dwords) &&
           adreno_pm4_emit(pm4, (uint32_t)src) &&
           adreno_pm4_emit(pm4, (uint32_t)(src >> 32)) &&
           adreno_pm4_emit(pm4, (uint32_t)dst) &&
           adreno_pm4_emit(pm4, (uint32_t)(dst >> 32));
}

bool adreno_pm4_load_state6_shader(adreno_pm4 *pm4, uint32_t state_block,
                                   adreno_iova_t iova, uint32_t units)
{
    uint32_t ctrl;

    if (!pm4 || units == 0 || units > 0x3ffu ||
        state_block > 0xfu) {
        if (pm4)
            pm4->overflow = true;
        return false;
    }
    ctrl = ((uint32_t)ADRENO_ST6_SHADER << 14) |
           ((uint32_t)ADRENO_SS6_INDIRECT << 16) |
           (state_block << 18) | (units << 22);
    return adreno_pm4_emit_pkt7(pm4,
                                (state_block == ADRENO_SB6_CS_SHADER)
                                    ? CP_LOAD_STATE6
                                    : CP_LOAD_STATE6_FRAG,
                                3) &&
           adreno_pm4_emit(pm4, ctrl) &&
           adreno_pm4_emit(pm4, (uint32_t)iova) &&
           adreno_pm4_emit(pm4, (uint32_t)(iova >> 32));
}

bool adreno_pm4_load_state6_const(adreno_pm4 *pm4, uint32_t state_block,
                                  uint32_t dst_vec4, const uint32_t *data,
                                  uint32_t vec4s)
{
    uint32_t ctrl;
    uint32_t i;

    if (!pm4 || !data || vec4s == 0 || vec4s > 0x3ffu ||
        dst_vec4 > 0x3fffu || state_block > 0xfu) {
        if (pm4)
            pm4->overflow = true;
        return false;
    }
    ctrl = dst_vec4 | ((uint32_t)ADRENO_ST6_CONSTANTS << 14) |
           ((uint32_t)ADRENO_SS6_DIRECT << 16) | (state_block << 18) |
           (vec4s << 22);
    if (!adreno_pm4_emit_pkt7(pm4,
                              (state_block == ADRENO_SB6_CS_SHADER)
                                  ? CP_LOAD_STATE6
                                  : CP_LOAD_STATE6_FRAG,
                              3 + vec4s * 4) ||
        !adreno_pm4_emit(pm4, ctrl) || !adreno_pm4_emit(pm4, 0) ||
        !adreno_pm4_emit(pm4, 0))
        return false;
    for (i = 0; i < vec4s * 4; i++) {
        if (!adreno_pm4_emit(pm4, data[i]))
            return false;
    }
    return true;
}

bool adreno_pm4_exec_cs(adreno_pm4 *pm4, uint32_t groups_x,
                        uint32_t groups_y, uint32_t groups_z)
{
    return adreno_pm4_emit_pkt7(pm4, CP_EXEC_CS, 4) &&
           adreno_pm4_emit(pm4, 0) &&
           adreno_pm4_emit(pm4, groups_x) &&
           adreno_pm4_emit(pm4, groups_y) &&
           adreno_pm4_emit(pm4, groups_z);
}

bool adreno_pm4_wait_for_idle(adreno_pm4 *pm4)
{
    return adreno_pm4_emit_pkt7(pm4, CP_WAIT_FOR_IDLE, 0);
}

bool adreno_pm4_cache_flush_ts(adreno_pm4 *pm4)
{
    return adreno_pm4_emit_pkt7(pm4, CP_EVENT_WRITE, 1) &&
           adreno_pm4_emit(pm4, CACHE_CLEAN);
}

bool adreno_pm4_thread_control(adreno_pm4 *pm4, uint32_t thread_value)
{
    return adreno_pm4_emit_pkt7(pm4, CP_THREAD_CONTROL, 1) &&
           adreno_pm4_emit(pm4, thread_value);
}

bool adreno_pm4_set_marker(adreno_pm4 *pm4, uint32_t marker)
{
    return adreno_pm4_emit_pkt7(pm4, CP_SET_MARKER, 1) &&
           adreno_pm4_emit(pm4, marker);
}

bool adreno_pm4_wait_timestamp(adreno_pm4 *pm4, adreno_iova_t iova,
                               uint32_t seqno)
{
    return adreno_pm4_emit_pkt7(pm4, CP_WAIT_TIMESTAMP, 4) &&
           adreno_pm4_emit(pm4, 0) &&
           adreno_pm4_emit(pm4, (uint32_t)iova) &&
           adreno_pm4_emit(pm4, (uint32_t)(iova >> 32)) &&
           adreno_pm4_emit(pm4, seqno);
}

bool adreno_pm4_me_init(adreno_pm4 *pm4, adreno_iova_t pwrup_reglist_iova)
{
    const uint32_t mask = ADRENO_BIT(0) | ADRENO_BIT(1) | ADRENO_BIT(3) |
                          ADRENO_BIT(6) | ADRENO_BIT(8);
    return adreno_pm4_emit_pkt7(pm4, CP_ME_INIT, 7) &&
           adreno_pm4_emit(pm4, mask) &&
           adreno_pm4_emit(pm4, 0x00000003u) &&
           adreno_pm4_emit(pm4, 0x20000000u) &&
           adreno_pm4_emit(pm4, 0x00000002u) &&
           adreno_pm4_emit(pm4, (uint32_t)pwrup_reglist_iova) &&
           adreno_pm4_emit(pm4, (uint32_t)(pwrup_reglist_iova >> 32)) &&
           adreno_pm4_emit(pm4, ADRENO_BIT(31));
}

enum adreno_status adreno_pm4_emit_submit(adreno_pm4 *pm4,
                                          const adreno_submit_desc *desc)
{
    uint32_t i;

    if (!pm4 || !desc || (!desc->ibs && desc->num_ibs) ||
        !desc->fence_iova || (desc->fence_iova & 7) != 0)
        return ADRENO_ERR_INVALID;
    if (desc->num_ibs > 4096)
        return ADRENO_ERR_RANGE;

    /*
     * Full Linux 7.2 a7xx_submit() BR+BV sequence so Turnip A730 concurrent
     * binning / CP_THREAD_CONTROL packets execute cleanly on both pipes.
     */
    if (!adreno_pm4_thread_control(pm4,
                                   CP_THREAD_CONTROL_SYNC_THREADS |
                                       CP_SET_THREAD_BOTH))
        return ADRENO_ERR_OVERFLOW;
    if (!adreno_pm4_emit_pkt7(pm4, 0x1fu /* CP_RESET_CONTEXT_STATE */, 1) ||
        !adreno_pm4_emit(pm4, 0x0fu))
        return ADRENO_ERR_OVERFLOW;
    if (!adreno_pm4_thread_control(pm4,
                                   CP_THREAD_CONTROL_SYNC_THREADS |
                                       CP_SET_THREAD_BOTH))
        return ADRENO_ERR_OVERFLOW;

    for (i = 0; i < desc->num_ibs; i++) {
        if (desc->ibs[i].size_dwords == 0)
            continue;
        if (!adreno_pm4_indirect_buffer(pm4, desc->ibs[i].iova,
                                        desc->ibs[i].size_dwords))
            return ADRENO_ERR_OVERFLOW;
    }

    if (!adreno_pm4_thread_control(pm4, CP_SET_THREAD_BR))
        return ADRENO_ERR_OVERFLOW;
    if (!adreno_pm4_event_write_1(pm4, CCU_INVALIDATE_DEPTH))
        return ADRENO_ERR_OVERFLOW;
    if (!adreno_pm4_event_write_1(pm4, CCU_INVALIDATE_COLOR))
        return ADRENO_ERR_OVERFLOW;

    if (desc->bv_fence_iova) {
        if (!adreno_pm4_thread_control(pm4, CP_SET_THREAD_BV))
            return ADRENO_ERR_OVERFLOW;
        if (!adreno_pm4_event_write(pm4, CACHE_CLEAN,
                                    CP_EVENT_WRITE_CACHE_CLEAN_BV,
                                    desc->bv_fence_iova, desc->seqno))
            return ADRENO_ERR_OVERFLOW;
        if (!adreno_pm4_thread_control(pm4, CP_SET_THREAD_BR))
            return ADRENO_ERR_OVERFLOW;
        if (!adreno_pm4_wait_timestamp(pm4, desc->bv_fence_iova, desc->seqno))
            return ADRENO_ERR_OVERFLOW;
    }

    /* Flush UCHE and write the fence timestamp on the BR pipe. */
    if (!adreno_pm4_event_write(pm4, CACHE_CLEAN,
                                CP_EVENT_WRITE_CACHE_CLEAN_BV |
                                    CP_EVENT_WRITE_IRQ,
                                desc->fence_iova, desc->seqno))
        return ADRENO_ERR_OVERFLOW;
    if (!adreno_pm4_thread_control(pm4, CP_SET_THREAD_BOTH))
        return ADRENO_ERR_OVERFLOW;

    return pm4->overflow ? ADRENO_ERR_OVERFLOW : ADRENO_OK;
}

enum adreno_status adreno_pm4_validate(const uint32_t *data, uint32_t dwords)
{
    uint32_t i = 0;

    if (!data && dwords)
        return ADRENO_ERR_INVALID;

    while (i < dwords) {
        uint32_t header = data[i];
        uint32_t type = header >> 28;
        uint32_t count;

        if (type == 4)
            count = header & 0x7fu;
        else if (type == 7)
            count = header & 0x7fffu;
        else
            return ADRENO_ERR_INVALID;
        if (count > dwords - i - 1)
            return ADRENO_ERR_RANGE;
        i += 1 + count;
    }
    return ADRENO_OK;
}

uint32_t adreno_crc32(const void *data, size_t size, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~seed;
    size_t i;
    unsigned int bit;

    for (i = 0; i < size; i++) {
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}
