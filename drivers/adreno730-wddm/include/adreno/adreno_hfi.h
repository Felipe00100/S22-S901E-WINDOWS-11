/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_HFI_H
#define ADRENO_HFI_H

#include "adreno_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADRENO_HFI_MAX_QUEUES 3
#define ADRENO_HFI_QUEUE_SIZE 4096u
#define ADRENO_HFI_QUEUE_DWORDS (ADRENO_HFI_QUEUE_SIZE / 4u)
#define ADRENO_HFI_TABLE_SIZE (16u * 1024u)

#define ADRENO_HFI_COMMAND_QUEUE 0u
#define ADRENO_HFI_RESPONSE_QUEUE 1u
#define ADRENO_HFI_DEBUG_QUEUE 2u

#define ADRENO_HFI_MSG_CMD 0u
#define ADRENO_HFI_MSG_ACK 1u

#define ADRENO_HFI_HEADER_ID(h) ((h) & 0xffu)
#define ADRENO_HFI_HEADER_SIZE(h) (((h) >> 8) & 0xffu)
#define ADRENO_HFI_HEADER_TYPE(h) (((h) >> 16) & 0xfu)
#define ADRENO_HFI_HEADER_SEQ(h) (((h) >> 20) & 0xfffu)
#define ADRENO_HFI_HEADER(id, size_dwords, type, seq)                       \
    ((((uint32_t)(seq) & 0xfffu) << 20) |                                   \
     (((uint32_t)(type) & 0xfu) << 16) |                                    \
     (((uint32_t)(size_dwords) & 0xffu) << 8) | ((uint32_t)(id) & 0xffu))

/* A6xx/A7xx message IDs (a6xx_hfi.h). */
enum adreno_hfi_msg_id {
    HFI_H2F_MSG_INIT = 0,
    HFI_H2F_MSG_FW_VERSION = 1,
    HFI_H2F_MSG_BW_TABLE = 3,
    HFI_H2F_MSG_PERF_TABLE = 4,
    HFI_H2F_MSG_TEST = 5,
    HFI_H2F_MSG_ACD = 7,
    HFI_H2F_MSG_CLX_TBL = 8,
    HFI_H2F_MSG_START = 10,
    HFI_H2F_FEATURE_CTRL = 11,
    HFI_H2F_MSG_CORE_FW_START = 14,
    HFI_H2F_MSG_TABLE = 15,
    HFI_H2F_MSG_GX_BW_PERF_VOTE = 30,
    HFI_H2F_MSG_PREPARE_SLUMBER = 33,
    HFI_F2H_MSG_ERROR = 100,
    HFI_F2H_MSG_ACK = 126,
};

enum adreno_hfi_feature {
    HFI_FEATURE_DCVS = 0,
    HFI_FEATURE_HWSCHED = 1,
    HFI_FEATURE_PREEMPTION = 2,
    HFI_FEATURE_CLOCKS_ON = 3,
    HFI_FEATURE_BUS_ON = 4,
    HFI_FEATURE_RAIL_ON = 5,
    HFI_FEATURE_HWCG = 6,
    HFI_FEATURE_IFPC = 9,
    HFI_FEATURE_NAP = 10,
    HFI_FEATURE_ACD = 12,
    HFI_FEATURE_CLX = 21,
    HFI_FEATURE_HW_FENCE = 25,
    HFI_FEATURE_AQE = 29,
};

#pragma pack(push, 4)
typedef struct adreno_hfi_queue_table_header {
    uint32_t version;
    uint32_t size;
    uint32_t qhdr0_offset;
    uint32_t qhdr_size;
    uint32_t num_queues;
    uint32_t active_queues;
} adreno_hfi_queue_table_header;

typedef struct adreno_hfi_queue_header {
    uint32_t status;
    uint32_t iova;
    uint32_t type;
    uint32_t size;
    uint32_t msg_size;
    uint32_t dropped;
    uint32_t rx_watermark;
    uint32_t tx_watermark;
    uint32_t rx_request;
    uint32_t tx_request;
    uint32_t read_index;
    uint32_t write_index;
} adreno_hfi_queue_header;

typedef struct adreno_hfi_msg_response {
    uint32_t header;
    uint32_t ret_header;
    uint32_t error;
    uint32_t payload[16];
} adreno_hfi_msg_response;

typedef struct adreno_hfi_msg_gmu_init {
    uint32_t header;
    uint32_t seg_id;
    uint32_t dbg_buffer_addr;
    uint32_t dbg_buffer_size;
    uint32_t boot_state;
} adreno_hfi_msg_gmu_init;

typedef struct adreno_hfi_msg_fw_version {
    uint32_t header;
    uint32_t supported_version;
} adreno_hfi_msg_fw_version;

typedef struct adreno_hfi_msg_feature_ctrl {
    uint32_t header;
    uint32_t feature;
    uint32_t enable;
    uint32_t data;
} adreno_hfi_msg_feature_ctrl;

typedef struct adreno_hfi_msg_start {
    uint32_t header;
} adreno_hfi_msg_start;

typedef struct adreno_hfi_msg_core_fw_start {
    uint32_t header;
    uint32_t handle;
} adreno_hfi_msg_core_fw_start;

typedef struct adreno_hfi_perf_gx {
    uint32_t vote;
    uint32_t acd;
    uint32_t freq; /* kHz */
} adreno_hfi_perf_gx;

typedef struct adreno_hfi_perf_cx {
    uint32_t vote;
    uint32_t freq; /* kHz */
} adreno_hfi_perf_cx;

typedef struct adreno_hfi_perf_table {
    uint32_t header;
    uint32_t num_gpu_levels;
    uint32_t num_gmu_levels;
    adreno_hfi_perf_gx gx_votes[16];
    adreno_hfi_perf_cx cx_votes[4];
} adreno_hfi_perf_table;

typedef struct adreno_hfi_bw_table {
    uint32_t header;
    uint32_t bw_level_num;
    uint32_t cnoc_cmds_num;
    uint32_t ddr_cmds_num;
    uint32_t cnoc_wait_bitmask;
    uint32_t ddr_wait_bitmask;
    uint32_t cnoc_cmds_addrs[6];
    uint32_t cnoc_cmds_data[2][6];
    uint32_t ddr_cmds_addrs[8];
    uint32_t ddr_cmds_data[16][8];
} adreno_hfi_bw_table;
#pragma pack(pop)

typedef struct adreno_hfi_queue {
    adreno_hfi_queue_header *header;
    uint32_t *data;
    uint32_t seqnum;
    uint32_t dropped;
} adreno_hfi_queue;

typedef struct adreno_hfi {
    uint8_t *base;
    size_t size;
    adreno_iova_t iova;
    adreno_hfi_queue_table_header *table;
    adreno_hfi_queue queues[ADRENO_HFI_MAX_QUEUES];
} adreno_hfi;

/* Lay out the shared table and three 4K queues in a 16K, 4K-aligned BO. */
enum adreno_status adreno_hfi_init(adreno_hfi *hfi, void *base, size_t size,
                                   adreno_iova_t iova);
void adreno_hfi_reset(adreno_hfi *hfi);

/* Queue primitives. Returns dwords transferred. */
enum adreno_status adreno_hfi_queue_write(adreno_hfi *hfi, uint32_t qid,
                                          const uint32_t *data,
                                          uint32_t dwords);
enum adreno_status adreno_hfi_queue_read(adreno_hfi *hfi, uint32_t qid,
                                         uint32_t *data, uint32_t capacity,
                                         uint32_t *dwords);

/* Fill in the header and enqueue a command; msg[0] is overwritten. */
enum adreno_status adreno_hfi_send(adreno_hfi *hfi, uint32_t msg_id,
                                   uint32_t *msg, uint32_t dwords,
                                   uint32_t *seq_out);

/* Convenience message builders. Each returns the number of dwords. */
uint32_t adreno_hfi_build_init(adreno_hfi_msg_gmu_init *msg, uint32_t boot_state,
                               adreno_iova_t dbg_iova, uint32_t dbg_size);
uint32_t adreno_hfi_build_fw_version(adreno_hfi_msg_fw_version *msg,
                                     uint32_t supported);
uint32_t adreno_hfi_build_feature(adreno_hfi_msg_feature_ctrl *msg,
                                  uint32_t feature, uint32_t enable,
                                  uint32_t data);
uint32_t adreno_hfi_build_start(adreno_hfi_msg_start *msg);
uint32_t adreno_hfi_build_core_fw_start(adreno_hfi_msg_core_fw_start *msg,
                                        uint32_t handle);

/* Decode a response queue message. Returns ADRENO_OK for an ACK. */
enum adreno_status adreno_hfi_decode_ack(const uint32_t *msg, uint32_t dwords,
                                         uint32_t expected_seq,
                                         uint32_t *ret_header,
                                         uint32_t *payload,
                                         uint32_t *payload_dwords);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_HFI_H */
