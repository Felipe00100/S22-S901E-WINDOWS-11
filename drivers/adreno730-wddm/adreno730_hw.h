/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_WDDM_HW_STAGED_H
#define ADRENO_WDDM_HW_STAGED_H

#include <ntddk.h>
#include <dispmprt.h>

#include "adreno/adreno_a730.h"
#include "adreno/adreno_fw.h"
#include "adreno/adreno_hfi.h"
#include "adreno/adreno_smmu.h"

typedef struct ADRENO_WDDM_HW_BLOCK {
    void *host;
    PHYSICAL_ADDRESS pa;
    uint32_t size;
} ADRENO_WDDM_HW_BLOCK;

typedef struct ADRENO_WDDM_HW {
    PDXGKRNL_INTERFACE dxgk;
    HANDLE device_handle;

    void *gpu_va;
    uint32_t gpu_size;
    bool gpu_mm;
    void *gmu_va;
    uint32_t gmu_size;
    bool gmu_mm;
    void *cx_va;
    uint32_t cx_size;
    bool cx_mm;
    void *rscc_va;
    uint32_t rscc_size;
    bool rscc_mm;
    void *smmu_va;
    uint32_t smmu_size;
    bool smmu_mm;

    adreno_a730 gpu;
    adreno_hfi hfi;

    ADRENO_WDDM_HW_BLOCK hfi_mem;
    ADRENO_WDDM_HW_BLOCK ring_mem;
    ADRENO_WDDM_HW_BLOCK fence_mem;
    ADRENO_WDDM_HW_BLOCK shadow_mem;
    ADRENO_WDDM_HW_BLOCK sqe_mem;
    ADRENO_WDDM_HW_BLOCK ib_mem;
    ADRENO_WDDM_HW_BLOCK icache_mem;
    ADRENO_WDDM_HW_BLOCK dummy_mem;
    ADRENO_WDDM_HW_BLOCK log_mem;
    ADRENO_WDDM_HW_BLOCK dcache_mem;
    ADRENO_WDDM_HW_BLOCK dbg_mem;
    ADRENO_WDDM_HW_BLOCK pt_pool;
    adreno_pt_pool pt_pool_state;

    adreno_smmu smmu;
    adreno_pt gpu_pt;
    adreno_pt gmu_pt;
    bool use_smmu;
    bool smmu_ready;
    bool has_irq;
    uint64_t next_alloc_iova;

    uint8_t *gmu_fw;
    uint32_t gmu_fw_size;
    uint8_t *sqe_fw;
    uint32_t sqe_fw_size;
    adreno_fw_image gmu_image;
    adreno_fw_sqe sqe_info;
    bool firmware_loaded;

    bool hw_ready;
    bool cp_started;
    uint32_t seqno;
} ADRENO_WDDM_HW;

/*
 * Logs to both DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, ...) and
 * \SystemRoot\Temp\adreno730_wddm.log (at PASSIVE_LEVEL).
 */
void AdrenoWddmLogLine(const char *prefix, const char *fmt, ...);

NTSTATUS AdrenoWddmHwStart(ADRENO_WDDM_HW *hw, HANDLE device_handle,
                           PDXGKRNL_INTERFACE dxgk, bool use_smmu);
void AdrenoWddmHwStop(ADRENO_WDDM_HW *hw);

bool AdrenoWddmHwSmmuReady(const ADRENO_WDDM_HW *hw);
bool AdrenoWddmHwCpStarted(const ADRENO_WDDM_HW *hw);
uint64_t AdrenoWddmHwAllocIova(ADRENO_WDDM_HW *hw, uint32_t size);
enum adreno_status AdrenoWddmHwMapIova(ADRENO_WDDM_HW *hw, uint64_t iova,
                                        uint64_t pa, uint32_t size);
void AdrenoWddmHwUnmapIova(ADRENO_WDDM_HW *hw, uint64_t iova, uint32_t size);

enum adreno_status AdrenoWddmHwSubmit(ADRENO_WDDM_HW *hw,
                                       const adreno_ib *ibs, uint32_t count,
                                       uint32_t seqno);
bool AdrenoWddmHwWaitFence(ADRENO_WDDM_HW *hw, uint32_t seqno,
                           uint32_t timeout_ms);
uint32_t AdrenoWddmHwFence(const ADRENO_WDDM_HW *hw);

uint32_t AdrenoWddmHwIrqStatus(ADRENO_WDDM_HW *hw);
void AdrenoWddmHwIrqAck(ADRENO_WDDM_HW *hw, uint32_t status);
void AdrenoWddmHwEnableInterrupts(ADRENO_WDDM_HW *hw);

#endif /* ADRENO_WDDM_HW_STAGED_H */
