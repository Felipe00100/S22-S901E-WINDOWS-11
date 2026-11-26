/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_D3D12_H
#define ADRENO_D3D12_H

#include "adreno_types.h"
#include "adreno_pm4.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal kernel interface used by the D3D12 backend. The Windows UMD binds
 * these to the runtime callbacks; the host simulator and the bring-up tool
 * bind them to the KMD ABI. Keeping this portable is what lets the same clear
 * path be tested without a WDDM adapter.
 */
typedef struct adreno_kmd_ops {
    void *ctx;
    enum adreno_status (*alloc)(void *ctx, uint32_t size, uint32_t *handle,
                                uint64_t *iova, void **host);
    enum adreno_status (*free)(void *ctx, uint32_t handle);
    enum adreno_status (*submit)(void *ctx, const adreno_ib *ibs,
                                 uint32_t count, uint32_t seqno);
    enum adreno_status (*wait)(void *ctx, uint32_t seqno,
                               uint32_t timeout_ms);
    /* Optional; used to read back through the KMD instead of a host pointer. */
    enum adreno_status (*read)(void *ctx, uint32_t handle, uint32_t offset,
                               void *dst, uint32_t size);
} adreno_kmd_ops;

typedef struct adreno_d3d12_device {
    adreno_kmd_ops kmd;
    uint32_t next_seqno;
} adreno_d3d12_device;

typedef struct adreno_d3d12_resource {
    adreno_d3d12_device *device;
    uint32_t handle;
    uint64_t iova;
    void *host;
    uint32_t size;
} adreno_d3d12_resource;

typedef struct adreno_d3d12_cmdlist {
    adreno_d3d12_device *device;
    adreno_d3d12_resource cmd_bo;
    adreno_pm4 pm4;
} adreno_d3d12_cmdlist;

enum adreno_status adreno_d3d12_device_init(adreno_d3d12_device *dev,
                                             const adreno_kmd_ops *ops);
void adreno_d3d12_device_finish(adreno_d3d12_device *dev);

enum adreno_status adreno_d3d12_resource_create(adreno_d3d12_device *dev,
                                                 uint32_t size,
                                                 adreno_d3d12_resource *res);
void adreno_d3d12_resource_destroy(adreno_d3d12_resource *res);

enum adreno_status adreno_d3d12_cmdlist_create(adreno_d3d12_device *dev,
                                                uint32_t capacity_dwords,
                                                adreno_d3d12_cmdlist *cmd);
void adreno_d3d12_cmdlist_destroy(adreno_d3d12_cmdlist *cmd);
void adreno_d3d12_cmdlist_reset(adreno_d3d12_cmdlist *cmd);

/*
 * Grow the command BO so at least extra_dwords more can be recorded. Existing
 * packets are preserved. This lets a command list start small (or be reused)
 * and scale up to a large clear without a fixed upper bound at creation.
 */
enum adreno_status adreno_d3d12_cmdlist_reserve(adreno_d3d12_cmdlist *cmd,
                                                 uint32_t extra_dwords);

/* Clear a range of 32-bit words in a resource to a constant color. */
enum adreno_status adreno_d3d12_cmd_clear_u32(adreno_d3d12_cmdlist *cmd,
                                               adreno_d3d12_resource *res,
                                               uint32_t offset_bytes,
                                               uint32_t dwords,
                                               uint32_t color);

/* Copy dwords between two resources (CP_MEMCPY). */
enum adreno_status adreno_d3d12_cmd_copy_u32(adreno_d3d12_cmdlist *cmd,
                                              adreno_d3d12_resource *dst,
                                              uint32_t dst_offset,
                                              adreno_d3d12_resource *src,
                                              uint32_t src_offset,
                                              uint32_t dwords);

/*
 * Fixed-function state for a precompiled A730 IR3 compute kernel. The UMD
 * fills this from the shader/PSO once a DXIL->IR3 compiler exists; until then
 * the diagnostic kernel in adreno_ir3_ref.h uses the same path.
 */
typedef struct adreno_d3d12_ir3_meta {
    uint32_t instrlen_units; /* 128-byte instruction groups */
    uint32_t constlen;       /* vec4 constant registers */
    uint32_t fullregfootprint;
    uint32_t halfregfootprint;
    uint32_t branchstack;
    uint32_t threadsize; /* 0 = THREAD64, 1 = THREAD128 */
    uint32_t shared_size;
    uint32_t constantrammode;
    uint32_t mergedregs;
    uint32_t localid_regid;
    uint32_t wgid_constid;
    uint32_t local_size[3];
} adreno_d3d12_ir3_meta;

/*
 * Encode a real A730 compute dispatch:
 *   SP_* fixed state, CP_LOAD_STATE6 (shader + constants), CP_EXEC_CS,
 *   CP_WAIT_FOR_IDLE and CACHE_FLUSH_TS.
 * shader must point at the IR3 bytes (iova/size are used); constants holds
 * const_vec4s vec4s starting at c0. Returns ADRENO_ERR_UNSUPPORTED for
 * malformed/oversized state instead of silently dropping the dispatch.
 */
enum adreno_status adreno_d3d12_cmd_dispatch_ir3(
    adreno_d3d12_cmdlist *cmd, const adreno_d3d12_resource *shader,
    const adreno_d3d12_ir3_meta *meta, const uint32_t *constants,
    uint32_t const_vec4s, const uint32_t grid[3]);

/* Metadata for the precompiled diagnostic kernel in adreno_ir3_ref.h. */
extern const adreno_d3d12_ir3_meta adreno_d3d12_a730_test_meta;

/*
 * UMD -> WDDM KMD allocation request. The D3D12 allocate callback has no
 * explicit size field; the UMD carries it in pPrivateDriverData so the KMD
 * can create the matching allocation. The fake-runtime test uses the same
 * structure.
 */
#define ADRENO_WDDM_ALLOC_PRIVATE_MAGIC 0x57444D41u /* "WDMA" */
typedef struct adreno_wddm_alloc_private {
    uint32_t magic;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t format;
    uint32_t texture2d;
    uint32_t reserved;
} adreno_wddm_alloc_private;

/*
 * UMD-private shader container used by the diagnostic compute test. The D3D12
 * runtime never produces this; it lets a test hand the UMD a precompiled IR3
 * blob until the DXIL->IR3 compiler exists. Header fields mirror
 * adreno_d3d12_ir3_meta, and shader_bytes are stored immediately after it.
 */
#define ADRENO_IR3_BLOB_MAGIC 0x49335241u /* "IR3A" */
#define ADRENO_IR3_BLOB_VERSION 1u
typedef struct adreno_ir3_blob {
    uint32_t magic;
    uint32_t version;
    uint32_t shader_bytes;
    uint32_t instrlen_units;
    uint32_t constlen;
    uint32_t fullregfootprint;
    uint32_t halfregfootprint;
    uint32_t branchstack;
    uint32_t threadsize;
    uint32_t shared_size;
    uint32_t constantrammode;
    uint32_t mergedregs;
    uint32_t localid_regid;
    uint32_t wgid_constid;
    uint32_t local_size[3];
    uint32_t reserved;
} adreno_ir3_blob;

enum adreno_status adreno_d3d12_submit(adreno_d3d12_device *dev,
                                       adreno_d3d12_cmdlist *cmd,
                                       uint32_t seqno);
/*
 * Submit several closed command lists as one KMD submission (one
 * SUBMIT_IB_LIST with N IBs, one fence). This is what D3D12
 * ExecuteCommandLists maps to.
 */
enum adreno_status adreno_d3d12_submit_lists(adreno_d3d12_device *dev,
                                              adreno_d3d12_cmdlist *const *cmds,
                                              uint32_t count, uint32_t seqno);
enum adreno_status adreno_d3d12_wait(adreno_d3d12_device *dev,
                                     uint32_t seqno, uint32_t timeout_ms);

/* Readback helper: host pointer when available, otherwise KMD read callback. */
enum adreno_status adreno_d3d12_resource_read(adreno_d3d12_resource *res,
                                               uint32_t offset, void *dst,
                                               uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_D3D12_H */
