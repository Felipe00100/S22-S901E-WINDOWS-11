/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_ABI_H
#define ADRENO_ABI_H

/*
 * Private user/kernel ABI for the bring-up driver and the Vulkan ICD.
 * This is not a stable ABI; bump ADRENO_ABI_VERSION on any layout change.
 *
 * Device interface:
 *   {b7c3c2a0-5a7e-4c6e-9f21-7c0d73000001}
 *
 * All register offsets use the same spaces as include/adreno/adreno_a730.h.
 */

#if defined(__KERNEL__) || defined(_KERNEL_MODE)
#include <ntddk.h>
typedef unsigned int adreno_u32;
typedef unsigned long long adreno_u64;
#else
#include <stdint.h>
typedef uint32_t adreno_u32;
typedef uint64_t adreno_u64;
#endif

#define ADRENO_ABI_VERSION 1u
#define ADRENO_ABI_MAGIC 0x41373330u /* "A730" */

#define ADRENO_DEVICE_INTERFACE_GUID                                      \
    {0xb7c3c2a0, 0x5a7e, 0x4c6e,                                         \
     {0x9f, 0x21, 0x7c, 0x0d, 0x73, 0x00, 0x00, 0x01}}

#define ADRENO_IOCTL_BASE 0x800u

#ifndef FILE_DEVICE_GRAPHICS
#define FILE_DEVICE_GRAPHICS 0x0000002bu
#endif

#if defined(__KERNEL__) || defined(_KERNEL_MODE)
#define ADRENO_IOCTL(code)                                                 \
    CTL_CODE(FILE_DEVICE_GRAPHICS, ADRENO_IOCTL_BASE + (code),             \
             METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#endif
#define ADRENO_IOCTL_GET_INFO 0x00u
#define ADRENO_IOCTL_READ_REG 0x01u
#define ADRENO_IOCTL_WRITE_REG 0x02u
#define ADRENO_IOCTL_SUBMIT 0x03u
#define ADRENO_IOCTL_WAIT_FENCE 0x04u
#define ADRENO_IOCTL_GET_FENCE 0x05u
#define ADRENO_IOCTL_RESET 0x06u
#define ADRENO_IOCTL_GET_LOG 0x07u
#define ADRENO_IOCTL_RELOAD_FW 0x08u
#define ADRENO_IOCTL_ALLOC 0x09u
#define ADRENO_IOCTL_FREE 0x0au
#define ADRENO_IOCTL_SUBMIT_IB 0x0bu
#define ADRENO_IOCTL_SUBMIT_IB_LIST 0x0cu

#pragma pack(push, 8)

typedef struct adreno_abi_info {
    adreno_u32 magic;
    adreno_u32 abi_version;
    adreno_u32 chip_id;
    adreno_u32 info_size;

    adreno_u32 rbbm_status;
    adreno_u32 gpu_control_status;
    adreno_u32 gmu_booted;
    adreno_u32 cp_started;
    adreno_u32 fw_gmu_loaded;
    adreno_u32 fw_sqe_loaded;
    adreno_u32 ring_dwords;
    adreno_u32 fence_value;
    adreno_u32 num_windows;
    adreno_u32 reserved0;

    adreno_u64 ring_iova;
    adreno_u64 fence_iova;
    adreno_u64 hfi_iova;
    adreno_u64 sqe_iova;
    adreno_u64 hfi_host_va;
    adreno_u64 reserved1;

    adreno_u32 fw_gmu_crc32;
    adreno_u32 fw_sqe_crc32;
    adreno_u32 fw_gmu_bytes;
    adreno_u32 fw_sqe_bytes;
    adreno_u32 last_error;
    adreno_u32 pad[3];
} adreno_abi_info;

typedef struct adreno_abi_reg {
    adreno_u32 space;
    adreno_u32 reg;
    adreno_u32 value;
    adreno_u32 status;
} adreno_abi_reg;

typedef struct adreno_abi_submit {
    adreno_u32 magic;
    adreno_u32 seqno;
    adreno_u32 cmd_dwords;
    adreno_u32 flags;
    adreno_u32 status;
    adreno_u32 reserved;
    /* cmd_dwords follow immediately, 4-byte aligned. */
    adreno_u32 cmds[];
} adreno_abi_submit;

typedef struct adreno_abi_fence {
    adreno_u32 seqno;
    adreno_u32 timeout_ms;
    adreno_u32 completed;
    adreno_u32 fence_value;
    adreno_u32 status;
    adreno_u32 reserved;
} adreno_abi_fence;

typedef struct adreno_abi_log {
    adreno_u32 first;
    adreno_u32 count;
    adreno_u32 capacity;
    adreno_u32 status;
    /* message bytes follow */
} adreno_abi_log;

typedef struct adreno_abi_alloc {
    adreno_u32 magic;
    adreno_u32 size;
    adreno_u32 flags;
    adreno_u32 handle;
    adreno_u32 status;
    adreno_u32 reserved0;
    adreno_u64 iova;
    adreno_u64 user_va;
    adreno_u64 reserved1;
} adreno_abi_alloc;

typedef struct adreno_abi_free {
    adreno_u32 magic;
    adreno_u32 handle;
    adreno_u32 status;
    adreno_u32 reserved;
} adreno_abi_free;

typedef struct adreno_abi_submit_ib {
    adreno_u32 magic;
    adreno_u32 handle;
    adreno_u32 offset_dwords;
    adreno_u32 size_dwords;
    adreno_u32 seqno;
    adreno_u32 status;
    adreno_u64 reserved;
} adreno_abi_submit_ib;

typedef struct adreno_abi_ib {
    adreno_u32 handle;
    adreno_u32 offset_dwords;
    adreno_u32 size_dwords;
    adreno_u32 flags;
} adreno_abi_ib;

typedef struct adreno_abi_submit_list {
    adreno_u32 magic;
    adreno_u32 count;
    adreno_u32 seqno;
    adreno_u32 status;
    adreno_abi_ib ibs[];
} adreno_abi_submit_list;

#pragma pack(pop)

#endif /* ADRENO_ABI_H */
