/* SPDX-License-Identifier: MIT */
/* Staged v9 HFI: fixes table->size in BYTES (168, not /4!) and adds ARM64 DMB barriers + volatile indices */
#include "adreno/adreno_hfi.h"

#include <string.h>
#ifdef _M_ARM64
#include <intrin.h>
#define HFI_MB() __dmb(_ARM64_BARRIER_SY)
#else
#define HFI_MB() do {} while (0)
#endif

#define HFI_COOKIE 0xfafafafau

static uint32_t queue_used(const adreno_hfi_queue *q)
{
    uint32_t w = *(volatile uint32_t *)&q->header->write_index;
    uint32_t r = *(volatile uint32_t *)&q->header->read_index;
    return (w - r) & (q->header->size - 1);
}

static uint32_t queue_space(const adreno_hfi_queue *q)
{
    return q->header->size - 1 - queue_used(q);
}

enum adreno_status adreno_hfi_init(adreno_hfi *hfi, void *base, size_t size,
                                   adreno_iova_t iova)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)base;

    if (!hfi || !base || size < ADRENO_HFI_TABLE_SIZE ||
        (iova & 0xfff) != 0)
        return ADRENO_ERR_INVALID;

    memset(hfi, 0, sizeof(*hfi));
    memset(base, 0, size);
    hfi->base = p;
    hfi->size = size;
    hfi->iova = iova;
    hfi->table = (adreno_hfi_queue_table_header *)p;
    hfi->table->version = 0;
    /* Linux a6xx_hfi.c: table->size is in BYTES (sizeof(*table) + num_queues * sizeof(queue_header) = 168) */
    hfi->table->size =
        (uint32_t)(sizeof(*hfi->table) +
                   ADRENO_HFI_MAX_QUEUES * sizeof(adreno_hfi_queue_header));
    hfi->table->qhdr0_offset = (uint32_t)(sizeof(*hfi->table) / 4);
    hfi->table->qhdr_size =
        (uint32_t)(sizeof(adreno_hfi_queue_header) / 4);
    hfi->table->num_queues = ADRENO_HFI_MAX_QUEUES;
    hfi->table->active_queues = ADRENO_HFI_MAX_QUEUES;

    for (i = 0; i < ADRENO_HFI_MAX_QUEUES; i++) {
        adreno_hfi_queue *q = &hfi->queues[i];
        adreno_hfi_queue_header *qh =
            (adreno_hfi_queue_header *)(p + sizeof(*hfi->table) +
                                        i * sizeof(*qh));
        q->header = qh;
        q->data = (uint32_t *)(p + ADRENO_HFI_QUEUE_SIZE * (i + 1));
        q->seqnum = 0;
        q->dropped = 0;
        qh->status = 1;
        qh->iova = (uint32_t)(iova + ADRENO_HFI_QUEUE_SIZE * (i + 1));
        qh->type = (10u << 8) | i;
        qh->size = ADRENO_HFI_QUEUE_DWORDS;
        qh->msg_size = 0;
        qh->dropped = 0;
        qh->rx_watermark = 1;
        qh->tx_watermark = 1;
        qh->rx_request = 1;
        qh->tx_request = 0;
        qh->read_index = 0;
        qh->write_index = 0;
    }
    HFI_MB();
    return ADRENO_OK;
}

void adreno_hfi_reset(adreno_hfi *hfi)
{
    uint32_t i;
    for (i = 0; hfi && i < ADRENO_HFI_MAX_QUEUES; i++) {
        *(volatile uint32_t *)&hfi->queues[i].header->read_index = 0;
        *(volatile uint32_t *)&hfi->queues[i].header->write_index = 0;
        hfi->queues[i].header->dropped = 0;
        hfi->queues[i].seqnum = 0;
        hfi->queues[i].dropped = 0;
    }
    HFI_MB();
}

enum adreno_status adreno_hfi_queue_write(adreno_hfi *hfi, uint32_t qid,
                                          const uint32_t *data,
                                          uint32_t dwords)
{
    adreno_hfi_queue *q;
    adreno_hfi_queue_header *qh;
    uint32_t i, index;

    if (!hfi || !data || qid >= ADRENO_HFI_MAX_QUEUES || dwords == 0 ||
        dwords > 0xffu)
        return ADRENO_ERR_INVALID;
    q = &hfi->queues[qid];
    qh = q->header;
    if (queue_space(q) < dwords) {
        qh->dropped++;
        q->dropped++;
        return ADRENO_ERR_NOMEM;
    }

    index = *(volatile uint32_t *)&qh->write_index;
    for (i = 0; i < dwords; i++) {
        ((volatile uint32_t *)q->data)[index] = data[i];
        index = (index + 1) & (qh->size - 1);
    }
    /* Cookify unused tail so the firmware never parses stale messages. */
    while (index & 3) {
        ((volatile uint32_t *)q->data)[index] = HFI_COOKIE;
        index = (index + 1) & (qh->size - 1);
    }
    HFI_MB();
    *(volatile uint32_t *)&qh->write_index = index;
    HFI_MB();
    return ADRENO_OK;
}

enum adreno_status adreno_hfi_queue_read(adreno_hfi *hfi, uint32_t qid,
                                         uint32_t *data, uint32_t capacity,
                                         uint32_t *dwords)
{
    adreno_hfi_queue *q;
    adreno_hfi_queue_header *qh;
    uint32_t hdr, size, index, i, widx;

    if (!hfi || !data || !dwords || qid >= ADRENO_HFI_MAX_QUEUES)
        return ADRENO_ERR_INVALID;
    q = &hfi->queues[qid];
    qh = q->header;
    *dwords = 0;
    index = *(volatile uint32_t *)&qh->read_index;
    widx = *(volatile uint32_t *)&qh->write_index;
    if (index == widx) {
        *(volatile uint32_t *)&qh->rx_request = 1;
        return ADRENO_OK;
    }
    HFI_MB();

    hdr = ((volatile uint32_t *)q->data)[index];
    size = ADRENO_HFI_HEADER_SIZE(hdr);
    if (size == 0 || size > capacity || size > qh->size)
        return ADRENO_ERR_FIRMWARE;

    for (i = 0; i < size; i++) {
        data[i] = ((volatile uint32_t *)q->data)[index];
        index = (index + 1) & (qh->size - 1);
    }
    index = (index + 3u) & ~3u;
    HFI_MB();
    *(volatile uint32_t *)&qh->read_index = index & (qh->size - 1);
    *(volatile uint32_t *)&qh->rx_request = 1;
    HFI_MB();
    *dwords = size;
    return ADRENO_OK;
}

enum adreno_status adreno_hfi_send(adreno_hfi *hfi, uint32_t msg_id,
                                   uint32_t *msg, uint32_t dwords,
                                   uint32_t *seq_out)
{
    adreno_hfi_queue *q;
    uint32_t seq;
    enum adreno_status st;

    if (!hfi || !msg || dwords == 0 || dwords > 0xffu)
        return ADRENO_ERR_INVALID;
    q = &hfi->queues[ADRENO_HFI_COMMAND_QUEUE];
    seq = (q->seqnum + 1) % 0xfff;
    if (seq == 0)
        seq = 1;
    msg[0] = ADRENO_HFI_HEADER(msg_id, dwords, ADRENO_HFI_MSG_CMD, seq);

    st = adreno_hfi_queue_write(hfi, ADRENO_HFI_COMMAND_QUEUE, msg, dwords);
    if (st != ADRENO_OK)
        return st;
    q->seqnum = seq;
    if (seq_out)
        *seq_out = seq;
    return ADRENO_OK;
}

uint32_t adreno_hfi_build_init(adreno_hfi_msg_gmu_init *msg, uint32_t boot_state,
                               adreno_iova_t dbg_iova, uint32_t dbg_size)
{
    memset(msg, 0, sizeof(*msg));
    msg->dbg_buffer_addr = (uint32_t)dbg_iova;
    msg->dbg_buffer_size = dbg_size;
    msg->boot_state = boot_state;
    return (uint32_t)(sizeof(*msg) / 4);
}

uint32_t adreno_hfi_build_fw_version(adreno_hfi_msg_fw_version *msg,
                                     uint32_t supported)
{
    memset(msg, 0, sizeof(*msg));
    msg->supported_version = supported;
    return (uint32_t)(sizeof(*msg) / 4);
}

uint32_t adreno_hfi_build_feature(adreno_hfi_msg_feature_ctrl *msg,
                                  uint32_t feature, uint32_t enable,
                                  uint32_t data)
{
    memset(msg, 0, sizeof(*msg));
    msg->feature = feature;
    msg->enable = enable;
    msg->data = data;
    return (uint32_t)(sizeof(*msg) / 4);
}

uint32_t adreno_hfi_build_start(adreno_hfi_msg_start *msg)
{
    memset(msg, 0, sizeof(*msg));
    return (uint32_t)(sizeof(*msg) / 4);
}

uint32_t adreno_hfi_build_core_fw_start(adreno_hfi_msg_core_fw_start *msg,
                                        uint32_t handle)
{
    memset(msg, 0, sizeof(*msg));
    msg->handle = handle;
    return (uint32_t)(sizeof(*msg) / 4);
}

enum adreno_status adreno_hfi_decode_ack(const uint32_t *msg, uint32_t dwords,
                                         uint32_t expected_seq,
                                         uint32_t *ret_header,
                                         uint32_t *payload,
                                         uint32_t *payload_dwords)
{
    const adreno_hfi_msg_response *resp =
        (const adreno_hfi_msg_response *)msg;
    uint32_t n;

    if (!msg || dwords < 3)
        return ADRENO_ERR_INVALID;
    if (ADRENO_HFI_HEADER_ID(resp->header) == HFI_F2H_MSG_ERROR)
        return ADRENO_ERR_FIRMWARE;
    if (ADRENO_HFI_HEADER_SEQ(resp->ret_header) !=
        (expected_seq & 0xfffu))
        return ADRENO_ERR_STATE;
    if (resp->error)
        return ADRENO_ERR_FIRMWARE;

    n = ADRENO_HFI_HEADER_SIZE(resp->header);
    if (n > dwords)
        n = dwords;
    if (n > 3) {
        n -= 3;
        if (n > 16)
            n = 16;
    } else {
        n = 0;
    }
    if (ret_header)
        *ret_header = resp->ret_header;
    if (payload && n)
        memcpy(payload, resp->payload, n * sizeof(uint32_t));
    if (payload_dwords)
        *payload_dwords = n;
    return ADRENO_OK;
}
