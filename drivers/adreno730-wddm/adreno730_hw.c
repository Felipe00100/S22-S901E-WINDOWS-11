/* SPDX-License-Identifier: MIT */
/*
 * Real SM8450 Adreno 730 bring-up for adreno730_wddm.sys (staged).
 *
 * Ports the complete hardware-verified 5-stage sequence from
 * adreno730_bringup.sys v1.0.0.18:
 *   Stage 1: GCC + GPUCC clocks + clean GX_GDSC collapse + CX_GDSC + SMMU vote
 *   Stage 2: SMMU-500 CB0 (GPU) + CB5 (GMU) with persistent pt_pool & EL0/EL1 AP split
 *   Stage 3: RSCC + PDC@0x0b290000 + GMU Cortex-M3 boot
 *   Stage 4: aop_cmd_db_mem@0x80860000 + HFI PERF_TABLE/BW_TABLE/CORE_FW_START/START/GX_BW_PERF_VOTE
 *   Stage 5: GMU_OOB_GPU_SET + A730 hw_init + UCHE/APRIV + ring_init + CP_START + HW CP_MEM_WRITE self-test
 */
#include "adreno730_hw.h"

#include <ntstrsafe.h>
#include "adreno/a730_regs.h"
#include "adreno/adreno_gpu.h"
#include "adreno/adreno_ir3_ref.h"
#include "adreno/adreno_pm4.h"
#include "adreno/adreno_ring.h"

typedef struct WDDM_SMC_ARGS {
    uint64_t a0, a1, a2, a3, a4, a5, a6, a7;
} WDDM_SMC_ARGS;

extern void S22SmcCall64(WDDM_SMC_ARGS *args);

#define WDDM_SCM_GPU_PAS_ID           13u
#define WDDM_SCM_GPU_ZAP_PHYS_BASE    0x8b91a000ull
#define WDDM_SCM_GPU_ZAP_PHYS_SIZE    0x2000u
#define WDDM_SCM_SVC_PIL              0x02u
#define WDDM_SCM_PIL_PAS_INIT_IMAGE   0x01u
#define WDDM_SCM_PIL_PAS_MEM_SETUP    0x02u
#define WDDM_SCM_PIL_PAS_AUTH_RESET   0x05u
#define WDDM_SCM_SVC_INFO             0x06u
#define WDDM_SCM_INFO_IS_CALL_AVAIL   0x01u
#define WDDM_SCM_INTERRUPTED          1ll
#define WDDM_SCM_V2_EBUSY             (-12ll)
#define WDDM_CP_SET_SECURE_MODE       0x66u

#define ADRENO_HW_TAG 'D737'
#define ADRENO_HW_MAX_FILE (4u * 1024u * 1024u)

/* ---- Dual DbgPrintEx + File Logger ------------------------------------- */

static void WddmAppendFileA(const char *text, ULONG len)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    NTSTATUS st;

    if (!text || len == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    RtlInitUnicodeString(&path, L"\\SystemRoot\\Temp\\adreno730_wddm.log");
    InitializeObjectAttributes(&attr, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    st = ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &attr, &iosb,
                      NULL, FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      FILE_OPEN_IF,
                      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                      NULL, 0);
    if (!NT_SUCCESS(st))
        return;
    (void)ZwWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)text, len, NULL, NULL);
    ZwClose(h);
}

void AdrenoWddmLogLine(const char *prefix, const char *fmt, ...)
{
    va_list args;
    char body[320];
    char line[384];
    size_t len = 0;

    va_start(args, fmt);
    RtlStringCchVPrintfA(body, sizeof(body), fmt, args);
    va_end(args);
    body[sizeof(body) - 1] = '\0';

    RtlStringCchPrintfA(line, sizeof(line), "%s: %s\r\n", prefix, body);
    line[sizeof(line) - 1] = '\0';

    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "%s: %s\n", prefix, body);

    if (NT_SUCCESS(RtlStringCchLengthA(line, sizeof(line), &len)) && len > 0)
        WddmAppendFileA(line, (ULONG)len);
}

#define HwLog(...) AdrenoWddmLogLine("adreno730_wddm_hw", __VA_ARGS__)

/* ---- MMIO callbacks ---------------------------------------------------- */

static bool HwGpuRange(const ADRENO_WDDM_HW *hw, uint32_t reg, uint32_t bytes)
{
    return reg <= hw->gpu_size / 4 && bytes <= hw->gpu_size - reg * 4;
}

static bool HwGmuByte(const ADRENO_WDDM_HW *hw, uint32_t reg, uint32_t *off)
{
    uint64_t byte = (uint64_t)reg * 4;
    if (byte < ADRENO_A730_GMU_MMIO_BIAS)
        return false;
    byte -= ADRENO_A730_GMU_MMIO_BIAS;
    if (byte + 4 > hw->gmu_size)
        return false;
    *off = (uint32_t)byte;
    return true;
}

static uint32_t HwRead32(void *ctx, adreno_space space, uint32_t reg)
{
    ADRENO_WDDM_HW *hw = (ADRENO_WDDM_HW *)ctx;
    uint32_t off;

    switch (space) {
    case ADRENO_SPACE_GPU:
        if (!hw->gpu_va || !HwGpuRange(hw, reg, 4))
            return 0xffffffffu;
        return READ_REGISTER_ULONG((PULONG)((uint8_t *)hw->gpu_va + reg * 4));
    case ADRENO_SPACE_GMU:
        if (!hw->gmu_va || !HwGmuByte(hw, reg, &off))
            return 0xffffffffu;
        return READ_REGISTER_ULONG((PULONG)((uint8_t *)hw->gmu_va + off));
    case ADRENO_SPACE_CX_MISC:
        if (!hw->cx_va || reg * 4 + 4 > hw->cx_size)
            return 0xffffffffu;
        return READ_REGISTER_ULONG((PULONG)((uint8_t *)hw->cx_va + reg * 4));
    case ADRENO_SPACE_GMU_RSCC:
        if (!hw->rscc_va || reg * 4 + 4 > hw->rscc_size)
            return 0xffffffffu;
        return READ_REGISTER_ULONG((PULONG)((uint8_t *)hw->rscc_va + reg * 4));
    case ADRENO_SPACE_SMMU:
        if (!hw->smmu_va || reg * 4 + 4 > hw->smmu_size)
            return 0xffffffffu;
        return READ_REGISTER_ULONG((PULONG)((uint8_t *)hw->smmu_va + reg * 4));
    default:
        return 0xffffffffu;
    }
}

static void HwWrite32(void *ctx, adreno_space space, uint32_t reg,
                      uint32_t value)
{
    ADRENO_WDDM_HW *hw = (ADRENO_WDDM_HW *)ctx;
    uint32_t off;

    switch (space) {
    case ADRENO_SPACE_GPU:
        if (hw->gpu_va && HwGpuRange(hw, reg, 4))
            WRITE_REGISTER_ULONG((PULONG)((uint8_t *)hw->gpu_va + reg * 4),
                                 value);
        break;
    case ADRENO_SPACE_GMU:
        if (hw->gmu_va && HwGmuByte(hw, reg, &off))
            WRITE_REGISTER_ULONG((PULONG)((uint8_t *)hw->gmu_va + off),
                                 value);
        break;
    case ADRENO_SPACE_CX_MISC:
        if (hw->cx_va && reg * 4 + 4 <= hw->cx_size)
            WRITE_REGISTER_ULONG((PULONG)((uint8_t *)hw->cx_va + reg * 4),
                                 value);
        break;
    case ADRENO_SPACE_GMU_RSCC:
        if (hw->rscc_va && reg * 4 + 4 <= hw->rscc_size)
            WRITE_REGISTER_ULONG((PULONG)((uint8_t *)hw->rscc_va + reg * 4),
                                 value);
        break;
    case ADRENO_SPACE_SMMU:
        if (hw->smmu_va && reg * 4 + 4 <= hw->smmu_size)
            WRITE_REGISTER_ULONG((PULONG)((uint8_t *)hw->smmu_va + reg * 4),
                                 value);
        break;
    default:
        break;
    }
}

static void HwWriteBulk(void *ctx, adreno_space space, uint32_t reg,
                        const void *data, size_t bytes)
{
    ADRENO_WDDM_HW *hw = (ADRENO_WDDM_HW *)ctx;
    const uint8_t *src = (const uint8_t *)data;
    size_t i;

    if (space != ADRENO_SPACE_GMU || !hw->gmu_va)
        return;
    for (i = 0; i + 4 <= bytes; i += 4) {
        uint32_t off;
        uint32_t value;
        if (!HwGmuByte(hw, reg + (uint32_t)(i / 4), &off))
            return;
        RtlCopyMemory(&value, src + i, 4);
        WRITE_REGISTER_ULONG((PULONG)((uint8_t *)hw->gmu_va + off), value);
    }
}

static void HwUdelay(void *ctx, uint32_t usec)
{
    (void)ctx;
    KeStallExecutionProcessor(usec);
}

static void HwLogCb(void *ctx, const char *message)
{
    (void)ctx;
    if (message)
        HwLog("%s", message);
}

/* ---- Registry & Firmware helpers --------------------------------------- */

static bool HwRegQueryDword(const UNICODE_STRING *key_path, PCWSTR value_name,
                            ULONG *out_value)
{
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name;
    HANDLE key = NULL;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG result;
    bool ok = false;

    if (!key_path || key_path->Length == 0 || !out_value)
        return false;
    InitializeObjectAttributes(&attr, (PUNICODE_STRING)key_path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                               NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &attr)))
        return false;
    RtlInitUnicodeString(&name, value_name);
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                                   buffer, sizeof(buffer), &result)) &&
        info->Type == REG_DWORD && info->DataLength >= sizeof(ULONG)) {
        *out_value = *(ULONG *)info->Data;
        ok = true;
    }
    ZwClose(key);
    return ok;
}

static ULONG HwRegDword(const UNICODE_STRING *dev_key, PCWSTR value_name,
                        ULONG default_value)
{
    UNICODE_STRING svc_params;
    ULONG v = default_value;

    /* Check service Parameters key first so live toggles are trivial */
    RtlInitUnicodeString(
        &svc_params,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\adreno730_wddm\\Parameters");
    if (HwRegQueryDword(&svc_params, value_name, &v))
        return v;
    if (HwRegQueryDword(dev_key, value_name, &v))
        return v;
    return default_value;
}

static void HwRegSetSvcDword(PCWSTR value_name, ULONG value)
{
    UNICODE_STRING svc_params;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    HANDLE key = NULL;
    ULONG disp = 0;

    if (!value_name || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    RtlInitUnicodeString(
        &svc_params,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\adreno730_wddm\\Parameters");
    InitializeObjectAttributes(&attr, &svc_params,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                               NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_WRITE, &attr, 0, NULL,
                                REG_OPTION_NON_VOLATILE, &disp)))
        return;
    RtlInitUnicodeString(&name, value_name);
    (void)ZwSetValueKey(key, &name, 0, REG_DWORD, &value, sizeof(value));
    (void)ZwFlushKey(key);
    ZwClose(key);
}

static NTSTATUS HwReadFile(const wchar_t *name, uint8_t **data,
                           uint32_t *size)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    HANDLE handle = NULL;
    wchar_t full[260];
    uint8_t *buf;
    uint32_t total = 0;
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;
    static const wchar_t *kSearchFmt[2] = {
        L"\\SystemRoot\\System32\\drivers\\qcom\\%s",
        L"\\SystemRoot\\System32\\drivers\\%s",
    };

    for (ULONG i = 0; i < 2; i++) {
        RtlStringCchPrintfW(full, ADRENO_ARRAY_SIZE(full), kSearchFmt[i], name);
        RtlInitUnicodeString(&path, full);
        InitializeObjectAttributes(&attr, &path,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        status = ZwCreateFile(&handle, GENERIC_READ | SYNCHRONIZE, &attr, &iosb,
                              NULL, FILE_ATTRIBUTE_NORMAL,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                              FILE_NON_DIRECTORY_FILE |
                                  FILE_SYNCHRONOUS_IO_NONALERT,
                              NULL, 0);
        if (NT_SUCCESS(status))
            break;
    }
    if (!NT_SUCCESS(status)) {
        HwLog("open %ls failed 0x%x", full, status);
        return status;
    }
    buf = (uint8_t *)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                 ADRENO_HW_MAX_FILE,
                                                 ADRENO_HW_TAG);
    if (!buf) {
        ZwClose(handle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (;;) {
        ULONG chunk = 64 * 1024;
        if (chunk > ADRENO_HW_MAX_FILE - total)
            chunk = ADRENO_HW_MAX_FILE - total;
        if (chunk == 0) {
            status = STATUS_FILE_TOO_LARGE;
            break;
        }
        status = ZwReadFile(handle, NULL, NULL, NULL, &iosb, buf + total,
                            chunk, NULL, NULL);
        if (status == STATUS_END_OF_FILE) {
            status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(status))
            break;
        if (iosb.Information == 0) {
            status = STATUS_SUCCESS;
            break;
        }
        total += (uint32_t)iosb.Information;
        if (total == ADRENO_HW_MAX_FILE) {
            status = STATUS_FILE_TOO_LARGE;
            break;
        }
    }
    ZwClose(handle);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(buf, ADRENO_HW_TAG);
        return status;
    }
    *data = buf;
    *size = total;
    HwLog("loaded %ls: %u bytes", name, total);
    return STATUS_SUCCESS;
}

static bool HwAllocBlock(ADRENO_WDDM_HW_BLOCK *block, uint32_t size,
                         const char *name)
{
    PHYSICAL_ADDRESS low, high, boundary;
    void *host;

    low.QuadPart = 0;
    high.QuadPart = 0xffffffffull;
    boundary.QuadPart = 0;
    host = MmAllocateContiguousMemorySpecifyCacheNode(
        size, low, high, boundary, MmWriteCombined, MM_ANY_NODE_OK);
    if (!host) {
        high.QuadPart = 0xffffffffffffull;
        host = MmAllocateContiguousMemorySpecifyCacheNode(
            size, low, high, boundary, MmWriteCombined, MM_ANY_NODE_OK);
    }
    if (!host) {
        HwLog("contiguous alloc failed: %s (%u bytes)", name, size);
        return false;
    }
    RtlZeroMemory(host, size);
    block->host = host;
    block->pa = MmGetPhysicalAddress(host);
    block->size = size;
    HwLog("alloc %s: %u bytes at PA 0x%llx", name, size,
          (unsigned long long)block->pa.QuadPart);
    return true;
}

static void HwFreeBlock(ADRENO_WDDM_HW_BLOCK *block)
{
    if (block->host) {
        MmFreeContiguousMemory(block->host);
        RtlZeroMemory(block, sizeof(*block));
    }
}

static bool HwMapWindow(ADRENO_WDDM_HW *hw, uint64_t pa, uint32_t size,
                        void **va, bool *via_mm, const char *name)
{
    PHYSICAL_ADDRESS address;
    NTSTATUS status;

    *va = NULL;
    *via_mm = false;
    address.QuadPart = (LONGLONG)pa;

    if (hw->dxgk && hw->dxgk->DxgkCbMapMemory) {
        status = hw->dxgk->DxgkCbMapMemory(hw->device_handle, address, size,
                                           FALSE, FALSE, MmNonCached, va);
        if (NT_SUCCESS(status) && *va) {
            HwLog("mapped %s (DxgkCb): PA 0x%llx size 0x%x", name,
                  (unsigned long long)pa, size);
            return true;
        }
    }

    *va = MmMapIoSpaceEx(address, size, PAGE_READWRITE | PAGE_NOCACHE);
    if (!*va) {
        HwLog("map %s PA 0x%llx+0x%x failed", name,
              (unsigned long long)pa, size);
        return false;
    }
    *via_mm = true;
    HwLog("mapped %s (MmMapIoSpaceEx): PA 0x%llx size 0x%x va=%p", name,
          (unsigned long long)pa, size, *va);
    return true;
}

static void HwUnmapWindow(ADRENO_WDDM_HW *hw, void *va, uint32_t size,
                          bool via_mm)
{
    if (!va)
        return;
    if (via_mm)
        MmUnmapIoSpace(va, size);
    else if (hw->dxgk && hw->device_handle)
        (void)hw->dxgk->DxgkCbUnmapMemory(hw->device_handle, va);
}

/* ---- Stage 1: SM8450 GCC + GPUCC Clocks & GDSC ------------------------- */

static void WddmClkSetBit(volatile uint8_t *base, uint32_t off, uint32_t mask)
{
    uint32_t v = READ_REGISTER_ULONG((PULONG)(base + off));
    WRITE_REGISTER_ULONG((PULONG)(base + off), v | mask);
    (void)READ_REGISTER_ULONG((PULONG)(base + off));
}

static uint32_t WddmBranchEnable(volatile uint8_t *base, uint32_t off)
{
    uint32_t i, v;
    WddmClkSetBit(base, off, 1u);
    for (i = 0; i < 500; i++) {
        v = READ_REGISTER_ULONG((PULONG)(base + off));
        if ((v & 0x80000000u) == 0)
            return v;
        KeStallExecutionProcessor(10);
    }
    return READ_REGISTER_ULONG((PULONG)(base + off));
}

static uint32_t WddmRcgConfig(volatile uint8_t *base, uint32_t cmd_off,
                              uint32_t cfg_val)
{
    uint32_t i, cmd;
    WRITE_REGISTER_ULONG((PULONG)(base + cmd_off + 4u), cfg_val);
    cmd = READ_REGISTER_ULONG((PULONG)(base + cmd_off));
    WRITE_REGISTER_ULONG((PULONG)(base + cmd_off), cmd | 1u);
    for (i = 0; i < 500; i++) {
        cmd = READ_REGISTER_ULONG((PULONG)(base + cmd_off));
        if ((cmd & 1u) == 0)
            break;
        KeStallExecutionProcessor(10);
    }
    return cmd;
}

static NTSTATUS WddmPowerOnSm8450Clocks(ADRENO_WDDM_HW *hw)
{
    PHYSICAL_ADDRESS pa;
    volatile uint8_t *gcc = NULL;
    volatile uint8_t *gpucc = NULL;
    uint32_t v, i;

    pa.QuadPart = 0x00100000ull;
    gcc = (volatile uint8_t *)MmMapIoSpaceEx(pa, 0x000A0000u,
                                             PAGE_READWRITE | PAGE_NOCACHE);
    if (!gcc) {
        HwLog("STAGE1: map GCC failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WddmClkSetBit(gcc, 0x62000, (1u << 15) | (1u << 16));
    (void)WddmBranchEnable(gcc, 0x81004); /* GCC_GPU_CFG_AHB_CLK */
    (void)WddmBranchEnable(gcc, 0x81008); /* GCC_QMIP_GPU_AHB_CLK */
    (void)WddmBranchEnable(gcc, 0x81010); /* GCC_GPU_MEMNOC_GFX_CLK */
    (void)WddmBranchEnable(gcc, 0x81018); /* GCC_GPU_SNOC_DVM_GFX_CLK */
    WddmClkSetBit(gcc, 0x81154, 1u);      /* GCC_DDRSS_GPU_AXI_CLK */

    pa.QuadPart = 0x03d90000ull;
    gpucc = (volatile uint8_t *)MmMapIoSpaceEx(pa, 0x0000A000u,
                                               PAGE_READWRITE | PAGE_NOCACHE);
    if (!gpucc) {
        MmUnmapIoSpace((PVOID)gcc, 0x000A0000u);
        HwLog("STAGE1: map GPUCC failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Clean warm collapse if GX_GDSC was left ON */
    v = READ_REGISTER_ULONG((PULONG)(gpucc + 0x905c));
    if (v & 0x80000000u) {
        if (hw->gmu_va) {
            HwWrite32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_AO_AHB_FENCE_CTRL, 0u);
            HwWrite32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_CM3_SYSRESET, 1u);
        }
        if (hw->gpu_va) {
            HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_RBBM_SW_RESET_CMD, 1u);
            (void)HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_RBBM_SW_RESET_CMD);
            KeStallExecutionProcessor(200);
            HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_RBBM_SW_RESET_CMD, 0u);
            (void)HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_RBBM_SW_RESET_CMD);
            KeStallExecutionProcessor(20);
        }
        WRITE_REGISTER_ULONG((PULONG)(gpucc + 0x905c), v | 1u);
        for (i = 0; i < 1000; i++) {
            v = READ_REGISTER_ULONG((PULONG)(gpucc + 0x905c));
            if ((v & 0x80000000u) == 0)
                break;
            KeStallExecutionProcessor(10);
        }
        HwLog("STAGE1 GX_GDSC warm collapse: 0x%x (iters=%u)", v, i);
    }

    /* Enable GPU_CX_GDSC */
    v = READ_REGISTER_ULONG((PULONG)(gpucc + 0x9108));
    v = (v & ~1u) | (1u << 11);
    WRITE_REGISTER_ULONG((PULONG)(gpucc + 0x9108), v);
    for (i = 0; i < 2000; i++) {
        v = READ_REGISTER_ULONG((PULONG)(gpucc + 0x9108));
        if (v & 0x80000000u)
            break;
        KeStallExecutionProcessor(10);
    }

    (void)WddmRcgConfig(gpucc, 0x9010, 0x00000001u);
    (void)WddmRcgConfig(gpucc, 0x93ec, 0x00000603u);
    (void)WddmRcgConfig(gpucc, 0x9318, 0x00000602u);
    (void)WddmRcgConfig(gpucc, 0x9474, 0x00000602u);

    (void)WddmBranchEnable(gpucc, 0x9004);
    (void)WddmBranchEnable(gpucc, 0x9144);
    (void)WddmBranchEnable(gpucc, 0x900c);
    (void)WddmBranchEnable(gpucc, 0x93e8);
    (void)WddmBranchEnable(gpucc, 0x9148);
    (void)WddmBranchEnable(gpucc, 0x911c);
    (void)WddmBranchEnable(gpucc, 0x912c);
    (void)WddmBranchEnable(gpucc, 0x9130);
    (void)WddmBranchEnable(gpucc, 0x913c);
    (void)WddmBranchEnable(gpucc, 0x914c);
    (void)WddmBranchEnable(gpucc, 0x9150);
    (void)WddmBranchEnable(gpucc, 0x7000); /* HLOS1_VOTE_GPU_SMMU_CLK */

    HwLog("STAGE1 CLKS OK: cx_gdsc=0x%x smmu_v=0x%x ahb=0x%x gmu=0x%x hub=0x%x",
          v,
          READ_REGISTER_ULONG((PULONG)(gpucc + 0x7000)),
          READ_REGISTER_ULONG((PULONG)(gpucc + 0x911c)),
          READ_REGISTER_ULONG((PULONG)(gpucc + 0x913c)),
          READ_REGISTER_ULONG((PULONG)(gpucc + 0x93e8)));

    MmUnmapIoSpace((PVOID)gpucc, 0x0000A000u);
    MmUnmapIoSpace((PVOID)gcc, 0x000A0000u);
    return STATUS_SUCCESS;
}

/* ---- Stage 2: Windows & SMMU-500 --------------------------------------- */

static bool HwNameEq(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t HwFixedIova(const ADRENO_WDDM_HW_BLOCK *block,
                            const char *name)
{
    if (!block || !name)
        return 0;
    if (HwNameEq(name, "icache"))
        return 0x00004000ull;
    if (HwNameEq(name, "dummy"))
        return 0x60000000ull;
    if (HwNameEq(name, "log"))
        return 0x60410000ull;
    if (HwNameEq(name, "hfi"))
        return 0x60420000ull;
    if (HwNameEq(name, "ring"))
        return 0x10000000ull;
    if (HwNameEq(name, "fence"))
        return 0x10010000ull;
    if (HwNameEq(name, "sqe"))
        return 0x10020000ull;
    if (HwNameEq(name, "ib"))
        return 0x10040000ull;
    if (HwNameEq(name, "shadow"))
        return 0x10050000ull;
    return 0;
}

static void HwSetupWindows(ADRENO_WDDM_HW *hw)
{
    ADRENO_WDDM_HW_BLOCK *blocks[] = {
        &hw->hfi_mem,    &hw->ring_mem, &hw->fence_mem, &hw->shadow_mem,
        &hw->sqe_mem,    &hw->ib_mem,   &hw->icache_mem,
        &hw->dummy_mem,  &hw->log_mem,
    };
    static const char *names[] = {
        "hfi", "ring", "fence", "shadow", "sqe",
        "ib", "icache", "dummy", "log",
    };
    uint32_t i;

    hw->gpu.num_windows = 0;
    for (i = 0; i < ADRENO_ARRAY_SIZE(blocks); i++) {
        if (blocks[i]->host) {
            uint64_t iova = (uint64_t)blocks[i]->pa.QuadPart;
            if (hw->use_smmu)
                iova = HwFixedIova(blocks[i], names[i]);
            else if (blocks[i] == &hw->icache_mem)
                iova = 0x4000;
            (void)adreno_a730_add_window(&hw->gpu, names[i], iova,
                                         blocks[i]->size, blocks[i]->host);
        }
    }
}

static enum adreno_status HwSetupSmmu(ADRENO_WDDM_HW *hw)
{
    adreno_pt_pool *pool = &hw->pt_pool_state;
    ADRENO_WDDM_HW_BLOCK *blocks[] = {
        &hw->hfi_mem,    &hw->ring_mem, &hw->fence_mem, &hw->shadow_mem,
        &hw->sqe_mem,    &hw->ib_mem,   &hw->icache_mem,
        &hw->dummy_mem,  &hw->log_mem,
    };
    static const char *names[] = {
        "hfi", "ring", "fence", "shadow", "sqe",
        "ib", "icache", "dummy", "log",
    };
    uint32_t i;
    enum adreno_status st;

    if (!hw->use_smmu)
        return ADRENO_OK;
    if (!hw->smmu_va || !hw->pt_pool.host)
        return ADRENO_ERR_STATE;

    adreno_pt_pool_init(pool, hw->pt_pool.host, hw->pt_pool.size,
                        hw->pt_pool.pa.QuadPart);
    st = adreno_pt_init(&hw->gpu_pt, pool);
    if (st != ADRENO_OK)
        return st;
    st = adreno_pt_init(&hw->gmu_pt, pool);
    if (st != ADRENO_OK)
        return st;

    for (i = 0; i < ADRENO_ARRAY_SIZE(blocks); i++) {
        uint64_t iova;
        if (!blocks[i]->host)
            continue;
        iova = HwFixedIova(blocks[i], names[i]);
        if (!iova)
            continue;
        st = adreno_pt_map(&hw->gpu_pt, iova,
                           (uint64_t)blocks[i]->pa.QuadPart,
                           blocks[i]->size, false);
        if (st != ADRENO_OK)
            return st;
        st = adreno_pt_map(&hw->gmu_pt, iova,
                           (uint64_t)blocks[i]->pa.QuadPart,
                           blocks[i]->size, false);
        if (st != ADRENO_OK)
            return st;
    }
    if (hw->dcache_mem.host)
        (void)adreno_pt_map(&hw->gmu_pt, 0x00200000ull,
                            (uint64_t)hw->dcache_mem.pa.QuadPart,
                            hw->dcache_mem.size, false);
    if (hw->dbg_mem.host)
        (void)adreno_pt_map(&hw->gmu_pt, 0x60400000ull,
                            (uint64_t)hw->dbg_mem.pa.QuadPart,
                            hw->dbg_mem.size, false);

    st = adreno_smmu_probe(&hw->smmu, &hw->gpu);
    if (st != ADRENO_OK)
        return st;
    st = adreno_smmu_setup(&hw->smmu, &hw->gpu_pt, &hw->gmu_pt, 0, 5);
    if (st != ADRENO_OK)
        return st;
    hw->smmu_ready = true;
    hw->next_alloc_iova = 0x80000000ull;
    HwLog("STAGE2 SMMU OK: %u CBs, numpage=%u sCR0=0x%x SMR0=0x%x SMR5=0x%x",
          hw->smmu.num_cbs, hw->smmu.numpage,
          HwRead32(hw, ADRENO_SPACE_SMMU, 0),
          HwRead32(hw, ADRENO_SPACE_SMMU, 0x200),
          HwRead32(hw, ADRENO_SPACE_SMMU, 0x205));
    return ADRENO_OK;
}

/* ---- Stage 3 & 4: RSCC/PDC + cmd-db + HFI ------------------------------ */

#pragma pack(push, 1)
typedef struct WDDM_CMD_DB_ENTRY_HDR {
    uint8_t id[8];
    uint32_t priority[2];
    uint32_t addr;
    uint16_t len;
    uint16_t offset;
} WDDM_CMD_DB_ENTRY_HDR;

typedef struct WDDM_CMD_DB_RSC_HDR {
    uint16_t slv_id;
    uint16_t header_offset;
    uint16_t data_offset;
    uint16_t cnt;
    uint16_t version;
    uint16_t reserved[3];
} WDDM_CMD_DB_RSC_HDR;

typedef struct WDDM_CMD_DB_HEADER {
    uint32_t version;
    uint8_t magic[4];
    WDDM_CMD_DB_RSC_HDR header[8];
    uint32_t checksum;
    uint32_t reserved;
    uint8_t data[1];
} WDDM_CMD_DB_HEADER;
#pragma pack(pop)

#pragma pack(push, 4)
typedef struct WDDM_HFI_GX_BW_PERF_VOTE {
    uint32_t header;
    uint32_t ack_type;
    uint32_t freq;
    uint32_t bw;
} WDDM_HFI_GX_BW_PERF_VOTE;
#pragma pack(pop)

static bool WddmCmdDbLookup(const uint8_t *base, uint32_t total_size,
                            const char *name, uint32_t *addr_out,
                            const uint16_t **aux_out, uint32_t *aux_count_out)
{
    const WDDM_CMD_DB_HEADER *hdr = (const WDDM_CMD_DB_HEADER *)base;
    uint8_t key[8];
    uint32_t i, j, k;
    const uint8_t *data_base;

    if (!base || total_size < sizeof(WDDM_CMD_DB_HEADER))
        return false;
    if (hdr->magic[0] != 0xdb || hdr->magic[1] != 0x30 ||
        hdr->magic[2] != 0x03 || hdr->magic[3] != 0x0c)
        return false;

    RtlZeroMemory(key, sizeof(key));
    for (i = 0; i < 8 && name[i] != '\0'; i++)
        key[i] = (uint8_t)name[i];

    data_base = hdr->data;
    for (i = 0; i < 8; i++) {
        uint16_t cnt = hdr->header[i].cnt;
        uint16_t hoff = hdr->header[i].header_offset;
        uint16_t doff = hdr->header[i].data_offset;
        if (cnt == 0)
            continue;
        for (j = 0; j < cnt; j++) {
            uint32_t eoff = (uint32_t)hoff + j * (uint32_t)sizeof(WDDM_CMD_DB_ENTRY_HDR);
            const WDDM_CMD_DB_ENTRY_HDR *ent;
            bool eq = true;
            if ((size_t)(data_base - base) + eoff + sizeof(WDDM_CMD_DB_ENTRY_HDR) > total_size)
                break;
            ent = (const WDDM_CMD_DB_ENTRY_HDR *)(data_base + eoff);
            for (k = 0; k < 8; k++) {
                if (ent->id[k] != key[k]) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                if (addr_out)
                    *addr_out = ent->addr;
                if (aux_out && aux_count_out) {
                    uint32_t aoff = (uint32_t)doff + (uint32_t)ent->offset;
                    if ((size_t)(data_base - base) + aoff + ent->len <= total_size) {
                        *aux_out = (const uint16_t *)(data_base + aoff);
                        *aux_count_out = ent->len >> 1;
                    } else {
                        *aux_out = NULL;
                        *aux_count_out = 0;
                    }
                }
                return true;
            }
        }
    }
    return false;
}

static uint32_t WddmBuildArcVote(uint32_t level, const uint16_t *pri,
                                 uint32_t pri_cnt, const uint16_t *sec,
                                 uint32_t sec_cnt)
{
    uint32_t j, pindex = 0, sindex = 0;
    for (j = 0; j < pri_cnt; j++) {
        if (pri[j] >= level) {
            pindex = j;
            break;
        }
    }
    if (j == pri_cnt && pri_cnt > 0)
        pindex = pri_cnt - 1;
    for (j = 0; j < sec_cnt; j++) {
        if (sec[j] >= level) {
            sindex = j;
            break;
        } else if (sec[j] != 0) {
            sindex = j;
        }
    }
    return (((uint32_t)pri[pindex] & 0xffffu) << 16) |
           ((sindex & 0xffu) << 8) | (pindex & 0xffu);
}

static void WddmInitGmuRpmh(ADRENO_WDDM_HW *hw)
{
    PHYSICAL_ADDRESS pa;
    volatile uint8_t *pdc;

    if (!hw->rscc_va)
        return;

    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_GPU_RSCC_RSC_STATUS0_DRV0, (1u << 24));
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_PDC_SLAVE_ID_DRV0, 1u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_HIDDEN_TCS_CMD0_DATA, 0u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_HIDDEN_TCS_CMD0_ADDR, 0u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_HIDDEN_TCS_CMD0_DATA + 2u, 0u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_HIDDEN_TCS_CMD0_ADDR + 2u, 0u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_HIDDEN_TCS_CMD0_DATA + 4u, 0x80000000u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_HIDDEN_TCS_CMD0_ADDR + 4u, 0u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_OVERRIDE_START_ADDR, 0u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_PDC_SEQ_START_ADDR, 0x4520u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_PDC_MATCH_VALUE_LO, 0x4510u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_PDC_MATCH_VALUE_HI, 0x4514u);

    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 0u, 0xeaaae5a0u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 1u, 0xe1a1ebabu);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 2u, 0xa2e0a581u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 3u, 0xecac82e2u);
    HwWrite32(hw, ADRENO_SPACE_GMU_RSCC, REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 4u, 0x0020edadu);

    pa.QuadPart = 0x0b290000ull;
    pdc = (volatile uint8_t *)MmMapIoSpaceEx(pa, 0x10000u,
                                             PAGE_READWRITE | PAGE_NOCACHE);
    if (pdc) {
        WRITE_REGISTER_ULONG((PULONG)(pdc + (0x1148u << 2)), 0u);
        WRITE_REGISTER_ULONG((PULONG)(pdc + (0x1140u << 2)), 0x80000001u);
        KeMemoryBarrier();
        HwLog("STAGE3 RSCC+PDC configured (en=0x%x)",
              READ_REGISTER_ULONG((PULONG)(pdc + (0x1140u << 2))));
        MmUnmapIoSpace((PVOID)pdc, 0x10000u);
    }
}

static enum adreno_status WddmHfiSendWait(ADRENO_WDDM_HW *hw, const char *name,
                                          uint32_t id, uint32_t *msg,
                                          uint32_t dwords, uint32_t timeout_us)
{
    uint32_t seq = 0, waited = 0, reply[64], reply_dwords;
    enum adreno_status st;

    st = adreno_hfi_send(&hw->hfi, id, msg, dwords, &seq);
    if (st != ADRENO_OK) {
        HwLog("HFI %s enqueue failed: %s", name, adreno_status_str(st));
        return st;
    }
    KeMemoryBarrier();
    HwWrite32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_HOST2GMU_INTR_SET, 1u);

    for (;;) {
        uint32_t intr = HwRead32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_GMU2HOST_INTR_INFO);
        if (intr & 1u)
            HwWrite32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_GMU2HOST_INTR_CLR, 1u);

        reply_dwords = 0;
        st = adreno_hfi_queue_read(&hw->hfi, ADRENO_HFI_RESPONSE_QUEUE, reply,
                                   ADRENO_ARRAY_SIZE(reply), &reply_dwords);
        if (st == ADRENO_OK && reply_dwords) {
            uint32_t rid = ADRENO_HFI_HEADER_ID(reply[0]);
            if (rid == HFI_F2H_MSG_ERROR) {
                HwLog("HFI %s -> GMU ERROR code=%u", name,
                      reply_dwords > 1 ? reply[1] : 0);
                continue;
            }
            st = adreno_hfi_decode_ack(reply, reply_dwords, seq, NULL, NULL, NULL);
            if (st == ADRENO_OK) {
                HwLog("HFI %s (seq=%u): ACK OK", name, seq);
                return ADRENO_OK;
            }
            if (st == ADRENO_ERR_FIRMWARE)
                return st;
        }
        if (waited >= timeout_us) {
            HwLog("HFI %s TIMEOUT (hfi_st=0x%x)", name,
                  HwRead32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_HFI_CTRL_STATUS));
            return ADRENO_ERR_TIMEOUT;
        }
        KeStallExecutionProcessor(50);
        waited += 50;
    }
}

static enum adreno_status WddmRunHfiStart(ADRENO_WDDM_HW *hw)
{
    PHYSICAL_ADDRESS pa;
    uint8_t *cmddb = NULL;
    const uint16_t *gfx_lvl = NULL, *gmxc_lvl = NULL, *cx_lvl = NULL, *mx_lvl = NULL;
    uint32_t gfx_cnt = 0, gmxc_cnt = 0, cx_cnt = 0, mx_cnt = 0;
    uint32_t addr_sh0 = 0, addr_mc0 = 0, addr_acv = 0, addr_cn0 = 0;
    adreno_hfi_perf_table perf;
    adreno_hfi_bw_table bw;
    adreno_hfi_msg_core_fw_start core;
    adreno_hfi_msg_start start;
    WDDM_HFI_GX_BW_PERF_VOTE vote;
    enum adreno_status st;
    uint32_t i;

    static const uint32_t gpu_khz[13] = {
        0, 220000, 285000, 317000, 350000, 421000, 492000,
        545000, 599000, 640000, 734000, 791000, 818000
    };
    static const uint32_t gpu_lvl[13] = {
        0, 56, 56, 64, 64, 80, 128, 144, 192, 224, 256, 320, 384
    };
    static const uint32_t gmu_khz[3] = { 0, 200000, 500000 };
    static const uint32_t gmu_lvl[3] = { 0, 64, 128 };
    static const uint32_t a730_ddr[12][3] = {
        { 0x40000000u, 0x40000000u, 0x40000000u },
        { 0x600002e8u, 0x600003d0u, 0x60000008u },
        { 0x6000068du, 0x6000089au, 0x60000008u },
        { 0x600007f2u, 0x60000a6eu, 0x60000008u },
        { 0x600009e5u, 0x60000cfdu, 0x60000008u },
        { 0x60000b29u, 0x60000ea6u, 0x60000008u },
        { 0x60001698u, 0x60001da8u, 0x60000008u },
        { 0x600018d2u, 0x60002093u, 0x60000008u },
        { 0x60001e66u, 0x600027e6u, 0x60000008u },
        { 0x600027c2u, 0x6000342fu, 0x60000008u },
        { 0x60002e71u, 0x60003cf5u, 0x60000008u },
        { 0x600030aeu, 0x60003fe5u, 0x60000008u },
    };

    pa.QuadPart = 0x80860000ull;
    cmddb = (uint8_t *)MmMapIoSpace(pa, 0x20000u, MmWriteCombined);
    if (!cmddb) {
        HwLog("STAGE4: map cmd-db@0x80860000 failed");
        return ADRENO_ERR_STATE;
    }

    (void)WddmCmdDbLookup(cmddb, 0x20000u, "gfx.lvl", NULL, &gfx_lvl, &gfx_cnt);
    (void)WddmCmdDbLookup(cmddb, 0x20000u, "gmxc.lvl", NULL, &gmxc_lvl, &gmxc_cnt);
    (void)WddmCmdDbLookup(cmddb, 0x20000u, "cx.lvl", NULL, &cx_lvl, &cx_cnt);
    (void)WddmCmdDbLookup(cmddb, 0x20000u, "mx.lvl", NULL, &mx_lvl, &mx_cnt);
    (void)WddmCmdDbLookup(cmddb, 0x20000u, "SH0", &addr_sh0, NULL, NULL);
    (void)WddmCmdDbLookup(cmddb, 0x20000u, "MC0", &addr_mc0, NULL, NULL);
    (void)WddmCmdDbLookup(cmddb, 0x20000u, "ACV", &addr_acv, NULL, NULL);
    (void)WddmCmdDbLookup(cmddb, 0x20000u, "CN0", &addr_cn0, NULL, NULL);

    if (!gfx_lvl || !cx_lvl || !mx_lvl || !addr_sh0 || !addr_mc0 || !addr_acv || !addr_cn0) {
        MmUnmapIoSpace(cmddb, 0x20000u);
        return ADRENO_ERR_STATE;
    }

    RtlZeroMemory(&perf, sizeof(perf));
    perf.num_gpu_levels = 13;
    perf.num_gmu_levels = 3;
    for (i = 0; i < 13; i++) {
        const uint16_t *sec = gmxc_lvl ? gmxc_lvl : mx_lvl;
        uint32_t sec_cnt = gmxc_lvl ? gmxc_cnt : mx_cnt;
        perf.gx_votes[i].vote = WddmBuildArcVote(gpu_lvl[i], gfx_lvl, gfx_cnt, sec, sec_cnt);
        perf.gx_votes[i].acd = 0xffffffffu;
        perf.gx_votes[i].freq = gpu_khz[i];
    }
    for (i = 0; i < 3; i++) {
        perf.cx_votes[i].vote = WddmBuildArcVote(gmu_lvl[i], cx_lvl, cx_cnt, mx_lvl, mx_cnt);
        perf.cx_votes[i].freq = gmu_khz[i];
    }

    RtlZeroMemory(&bw, sizeof(bw));
    bw.bw_level_num = 12;
    bw.ddr_cmds_num = 3;
    bw.ddr_wait_bitmask = 0x7u;
    bw.ddr_cmds_addrs[0] = addr_sh0;
    bw.ddr_cmds_addrs[1] = addr_mc0;
    bw.ddr_cmds_addrs[2] = addr_acv;
    for (i = 0; i < 12; i++) {
        bw.ddr_cmds_data[i][0] = a730_ddr[i][0];
        bw.ddr_cmds_data[i][1] = a730_ddr[i][1];
        bw.ddr_cmds_data[i][2] = a730_ddr[i][2];
    }
    bw.cnoc_cmds_num = 1;
    bw.cnoc_wait_bitmask = 0x1u;
    bw.cnoc_cmds_addrs[0] = addr_cn0;
    bw.cnoc_cmds_data[0][0] = 0x40000000u;
    bw.cnoc_cmds_data[1][0] = 0x60000001u;

    MmUnmapIoSpace(cmddb, 0x20000u);

    st = WddmHfiSendWait(hw, "PERF_TABLE", HFI_H2F_MSG_PERF_TABLE,
                         (uint32_t *)&perf, (uint32_t)(sizeof(perf) / 4), 500000);
    if (st != ADRENO_OK)
        return st;
    st = WddmHfiSendWait(hw, "BW_TABLE", HFI_H2F_MSG_BW_TABLE,
                         (uint32_t *)&bw, (uint32_t)(sizeof(bw) / 4), 500000);
    if (st != ADRENO_OK)
        return st;
    RtlZeroMemory(&core, sizeof(core));
    st = WddmHfiSendWait(hw, "CORE_FW_START", HFI_H2F_MSG_CORE_FW_START,
                         (uint32_t *)&core, (uint32_t)(sizeof(core) / 4), 500000);
    if (st != ADRENO_OK)
        return st;
    RtlZeroMemory(&start, sizeof(start));
    st = WddmHfiSendWait(hw, "START", HFI_H2F_MSG_START,
                         (uint32_t *)&start, (uint32_t)(sizeof(start) / 4), 500000);
    if (st != ADRENO_OK)
        return st;

    RtlZeroMemory(&vote, sizeof(vote));
    vote.ack_type = 1u;
    vote.freq = 1u;
    vote.bw = 1u;
    return WddmHfiSendWait(hw, "GX_BW_PERF_VOTE", HFI_H2F_MSG_GX_BW_PERF_VOTE,
                           (uint32_t *)&vote, (uint32_t)(sizeof(vote) / 4), 500000);
}

static enum adreno_status WddmGmuSetOobGpu(ADRENO_WDDM_HW *hw)
{
    uint32_t waited = 0;
    HwWrite32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_HOST2GMU_INTR_SET, (1u << 30));
    while (waited < 50000) {
        uint32_t v = HwRead32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_GMU2HOST_INTR_INFO);
        if (v & (1u << 31)) {
            HwWrite32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_GMU2HOST_INTR_CLR, (1u << 31));
            HwWrite32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_AO_AHB_FENCE_CTRL, 0u);
            HwLog("STAGE5 GMU_OOB_GPU_SET: ACK OK (sptprac=0x%x)",
                  HwRead32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS));
            return ADRENO_OK;
        }
        KeStallExecutionProcessor(50);
        waited += 50;
    }
    HwLog("STAGE5 GMU_OOB_GPU_SET: TIMEOUT");
    return ADRENO_ERR_TIMEOUT;
}

static int64_t WddmQcomScmCall(const char *name, bool fast, bool smc64,
                               uint32_t svc, uint32_t cmd, uint32_t arginfo,
                               uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3,
                               uint64_t *r0)
{
    uint64_t fnid = (((uint64_t)(svc & 0xffu)) << 8) | (uint64_t)(cmd & 0xffu);
    uint64_t call_val = (fast ? 0x80000000ull : 0ull) |
                        (smc64 ? 0x40000000ull : 0ull) |
                        (2ull << 24) | fnid;
    uint32_t busy_try;
    WDDM_SMC_ARGS args;

    for (busy_try = 0; busy_try < 20; busy_try++) {
        uint64_t a6_state = 0;
        uint64_t cur_a0 = call_val;
        uint32_t intr_try;

        for (intr_try = 0; intr_try < 1000; intr_try++) {
            RtlZeroMemory(&args, sizeof(args));
            args.a0 = cur_a0;
            args.a1 = arginfo;
            args.a2 = p0;
            args.a3 = p1;
            args.a4 = p2;
            args.a5 = p3;
            args.a6 = a6_state;
            S22SmcCall64(&args);
            a6_state = args.a6;
            if ((int64_t)args.a0 == WDDM_SCM_INTERRUPTED) {
                cur_a0 = (uint64_t)WDDM_SCM_INTERRUPTED;
                continue;
            }
            break;
        }
        if ((int64_t)args.a0 == WDDM_SCM_V2_EBUSY) {
            LARGE_INTEGER iv;
            iv.QuadPart = -300000ll;
            KeDelayExecutionThread(KernelMode, FALSE, &iv);
            continue;
        }
        break;
    }
    if (r0) *r0 = args.a1;
    HwLog("SCM %s (fn=0x%llx): a0=0x%llx a1=0x%llx", name, call_val, args.a0, args.a1);
    return (int64_t)args.a0;
}

static bool WddmZapVolatileFlagGet(void)
{
    UNICODE_STRING key_path, val_name;
    OBJECT_ATTRIBUTES oa;
    HANDLE hkey = NULL;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    ULONG ret_len = 0;
    bool authed = false;

    RtlInitUnicodeString(&key_path,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Adreno730ZapState");
    InitializeObjectAttributes(&oa, &key_path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&hkey, KEY_READ, &oa)))
        return false;
    RtlInitUnicodeString(&val_name, L"ZapAuthed");
    if (NT_SUCCESS(ZwQueryValueKey(hkey, &val_name, KeyValuePartialInformation, buf, sizeof(buf), &ret_len))) {
        PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
            authed = (*(ULONG *)info->Data != 0);
    }
    ZwClose(hkey);
    return authed;
}

static void WddmZapVolatileFlagSet(void)
{
    UNICODE_STRING key_path, val_name;
    OBJECT_ATTRIBUTES oa;
    HANDLE hkey = NULL;
    ULONG disp = 0, val = 1;

    RtlInitUnicodeString(&key_path,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Adreno730ZapState");
    InitializeObjectAttributes(&oa, &key_path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&hkey, KEY_WRITE, &oa, 0, NULL, REG_OPTION_VOLATILE, &disp)))
        return;
    RtlInitUnicodeString(&val_name, L"ZapAuthed");
    (void)ZwSetValueKey(hkey, &val_name, 0, REG_DWORD, &val, sizeof(val));
    ZwClose(hkey);
}

static enum adreno_status WddmEmitSetSecureMode0(ADRENO_WDDM_HW *hw)
{
    uint32_t *p = NULL;
    adreno_pm4 pm4;
    enum adreno_status st;
    uint32_t wptr;

    st = adreno_ring_reserve(&hw->gpu.ring, 8, &p);
    if (st != ADRENO_OK)
        return st;
    adreno_pm4_init(&pm4, p, 8);
    if (!adreno_pm4_emit_pkt7(&pm4, WDDM_CP_SET_SECURE_MODE, 1) ||
        !adreno_pm4_emit(&pm4, 0x00000000u)) {
        hw->gpu.ring.pending = false;
        return ADRENO_ERR_OVERFLOW;
    }
    adreno_ring_commit(&hw->gpu.ring, pm4.count);
    KeMemoryBarrier();
    adreno_a730_ring_kick(&hw->gpu, &hw->gpu.ring);

    wptr = hw->gpu.ring.wptr;
    st = adreno_hw_poll(&hw->gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_RPTR,
                        0xffffffffu, wptr, 500000);
    HwLog("STAGE5d CP_SET_SECURE_MODE(0): %s (RPTR=0x%x WPTR=0x%x TSB=0x%x_%08x)",
          adreno_status_str(st),
          HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_RPTR),
          HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_WPTR),
          HwRead32(hw, ADRENO_SPACE_GPU, 0xfc01u),
          HwRead32(hw, ADRENO_SPACE_GPU, 0xfc00u));
    return st;
}

static enum adreno_status WddmZapAuthAndUnsecure(ADRENO_WDDM_HW *hw)
{
    uint64_t r0 = 0;
    int64_t ret;
    uint8_t *mdt_buf = NULL, *b01_buf = NULL;
    uint32_t mdt_size = 0, b01_size = 0;
    void *mdata_dma = NULL, *zap_va = NULL;
    PHYSICAL_ADDRESS low_pa, high_pa, boundary_pa, mdata_pa, zap_pa;
    uint64_t aligned_sz;

    if (WddmZapVolatileFlagGet()) {
        HwLog("STAGE5d ZAP already authed this boot; emitting CP_SET_SECURE_MODE(0)");
        return WddmEmitSetSecureMode0(hw);
    }

    ret = WddmQcomScmCall("IS_CALL_AVAIL_64", true, true,
                          WDDM_SCM_SVC_INFO, WDDM_SCM_INFO_IS_CALL_AVAIL, 1u,
                          (2ull << 24) | ((uint64_t)WDDM_SCM_SVC_INFO << 8) | WDDM_SCM_INFO_IS_CALL_AVAIL,
                          0, 0, 0, &r0);
    if (ret != 0 || r0 != 1)
        return ADRENO_ERR_STATE;

    if (!NT_SUCCESS(HwReadFile(L"a730_zap.mdt", &mdt_buf, &mdt_size)) ||
        mdt_size < 0x94u || mdt_size > 4096u ||
        !NT_SUCCESS(HwReadFile(L"a730_zap.b01", &b01_buf, &b01_size)) ||
        b01_size == 0 || b01_size > WDDM_SCM_GPU_ZAP_PHYS_SIZE) {
        if (mdt_buf) ExFreePoolWithTag(mdt_buf, ADRENO_HW_TAG);
        if (b01_buf) ExFreePoolWithTag(b01_buf, ADRENO_HW_TAG);
        return ADRENO_ERR_FIRMWARE;
    }

    low_pa.QuadPart = 0;
    high_pa.QuadPart = 0xffffffffull;
    boundary_pa.QuadPart = 0;
    mdata_dma = MmAllocateContiguousMemorySpecifyCacheNode(
        4096, low_pa, high_pa, boundary_pa, MmWriteCombined, MM_ANY_NODE_OK);
    if (!mdata_dma) {
        ExFreePoolWithTag(mdt_buf, ADRENO_HW_TAG);
        ExFreePoolWithTag(b01_buf, ADRENO_HW_TAG);
        return ADRENO_ERR_NOMEM;
    }
    {
        volatile uint8_t *dst = (volatile uint8_t *)mdata_dma;
        uint32_t i;
        for (i = 0; i < 4096u; i++) dst[i] = 0;
        for (i = 0; i < mdt_size; i++) dst[i] = mdt_buf[i];
    }
    KeMemoryBarrier();
    mdata_pa = MmGetPhysicalAddress(mdata_dma);

    ret = WddmQcomScmCall("PAS_INIT_IMAGE", false, true,
                          WDDM_SCM_SVC_PIL, WDDM_SCM_PIL_PAS_INIT_IMAGE, 0x82u,
                          WDDM_SCM_GPU_PAS_ID, (uint64_t)mdata_pa.QuadPart, 0, 0, &r0);
    if (ret != 0 || r0 != 0)
        goto fail_zap;

    aligned_sz = ((uint64_t)b01_size + 0xfffu) & ~0xfffu;
    ret = WddmQcomScmCall("PAS_MEM_SETUP", false, true,
                          WDDM_SCM_SVC_PIL, WDDM_SCM_PIL_PAS_MEM_SETUP, 0x03u,
                          WDDM_SCM_GPU_PAS_ID, WDDM_SCM_GPU_ZAP_PHYS_BASE, aligned_sz, 0, &r0);
    if (ret != 0 || r0 != 0)
        goto fail_zap;

    zap_pa.QuadPart = (LONGLONG)WDDM_SCM_GPU_ZAP_PHYS_BASE;
    zap_va = MmMapIoSpace(zap_pa, (SIZE_T)aligned_sz, MmWriteCombined);
    if (!zap_va)
        goto fail_zap;
    {
        volatile uint8_t *dst = (volatile uint8_t *)zap_va;
        uint32_t i;
        for (i = 0; i < (uint32_t)aligned_sz; i++) dst[i] = 0;
        for (i = 0; i < b01_size; i++) dst[i] = b01_buf[i];
    }
    KeMemoryBarrier();
    MmUnmapIoSpace(zap_va, (SIZE_T)aligned_sz);

    ret = WddmQcomScmCall("PAS_AUTH_AND_RESET", false, true,
                          WDDM_SCM_SVC_PIL, WDDM_SCM_PIL_PAS_AUTH_RESET, 0x01u,
                          WDDM_SCM_GPU_PAS_ID, 0, 0, 0, &r0);
    MmFreeContiguousMemory(mdata_dma);
    ExFreePoolWithTag(mdt_buf, ADRENO_HW_TAG);
    ExFreePoolWithTag(b01_buf, ADRENO_HW_TAG);
    if (ret != 0 || r0 != 0)
        return ADRENO_ERR_FIRMWARE;

    WddmZapVolatileFlagSet();
    return WddmEmitSetSecureMode0(hw);

fail_zap:
    MmFreeContiguousMemory(mdata_dma);
    ExFreePoolWithTag(mdt_buf, ADRENO_HW_TAG);
    ExFreePoolWithTag(b01_buf, ADRENO_HW_TAG);
    return ADRENO_ERR_FIRMWARE;
}

/* ---- Resource parsing -------------------------------------------------- */

static void HwParseResources(ADRENO_WDDM_HW *hw, PDXGK_DEVICE_INFO info,
                             uint64_t *gpu_pa, uint64_t *gmu_pa, uint64_t *cx_pa,
                             uint64_t *rscc_pa, uint64_t *smmu_pa,
                             uint32_t *gpu_size, uint32_t *gmu_size,
                             uint32_t *cx_size, uint32_t *rscc_size,
                             uint32_t *smmu_size)
{
    PCM_RESOURCE_LIST list = info ? info->TranslatedResourceList : NULL;
    ULONG mem_index = 0;
    ULONG i, j;

    if (!list)
        return;
    for (i = 0; i < list->Count; i++) {
        PCM_FULL_RESOURCE_DESCRIPTOR full = &list->List[i];
        for (j = 0; j < full->PartialResourceList.Count; j++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR prd =
                &full->PartialResourceList.PartialDescriptors[j];
            if (prd->Type == CmResourceTypeInterrupt) {
                hw->has_irq = true;
                continue;
            }
            if (prd->Type != CmResourceTypeMemory)
                continue;
            if (mem_index == 0 && !*gpu_pa) {
                *gpu_pa = prd->u.Memory.Start.QuadPart;
                *gpu_size = prd->u.Memory.Length;
            } else if (mem_index == 1 && !*gmu_pa) {
                *gmu_pa = prd->u.Memory.Start.QuadPart;
                *gmu_size = prd->u.Memory.Length;
            } else if (mem_index == 2 && !*cx_pa) {
                *cx_pa = prd->u.Memory.Start.QuadPart;
                *cx_size = prd->u.Memory.Length;
            } else if (mem_index == 3 && !*rscc_pa) {
                *rscc_pa = prd->u.Memory.Start.QuadPart;
                *rscc_size = prd->u.Memory.Length;
            } else if (mem_index == 4 && !*smmu_pa) {
                *smmu_pa = prd->u.Memory.Start.QuadPart;
                *smmu_size = prd->u.Memory.Length;
            }
            mem_index++;
        }
    }
}

/* ---- Main Hardware Start / Stop ---------------------------------------- */

NTSTATUS AdrenoWddmHwStart(ADRENO_WDDM_HW *hw, HANDLE device_handle,
                           PDXGKRNL_INTERFACE dxgk, bool use_smmu)
{
    DXGK_DEVICE_INFO info;
    NTSTATUS status;
    enum adreno_status st;
    uint64_t gpu_pa = 0, gmu_pa = 0, cx_pa = 0, rscc_pa = 0, smmu_pa = 0;
    uint32_t gpu_size = ADRENO_A730_GPU_REG_SIZE;
    uint32_t gmu_size = ADRENO_A730_GMU_SIZE;
    uint32_t cx_size = ADRENO_A730_CX_MEM_SIZE;
    uint32_t rscc_size = ADRENO_A730_GMU_RSCC_SIZE;
    uint32_t smmu_size = ADRENO_A730_SMMU_SIZE;
    adreno_hw_ops ops;
    adreno_mem_window *pwrup;
    adreno_mem_window *hfiw;
    uint32_t pwrup_iova = 0;
    ULONG start_gpu = 0;

    if (!hw || !dxgk || !device_handle)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(hw, sizeof(*hw));
    hw->dxgk = dxgk;
    hw->device_handle = device_handle;
    hw->use_smmu = use_smmu;

    RtlZeroMemory(&info, sizeof(info));
    status = dxgk->DxgkCbGetDeviceInformation(device_handle, &info);
    if (!NT_SUCCESS(status))
        HwLog("GetDeviceInformation returned 0x%x", status);

    /* Safe boot gate: StartGpu + anti-bootloop BootInProgress watchdog */
    start_gpu = HwRegDword(&info.DeviceRegistryPath, L"StartGpu", 0);
    HwLog("config: StartGpu=%u (reg_len=%u)", start_gpu,
          info.DeviceRegistryPath.Length);
    if (!start_gpu) {
        HwLog("StartGpu=0; skipping hardware boot (safe reference CP mode)");
        return STATUS_DEVICE_NOT_READY;
    }
    if (HwRegDword(&info.DeviceRegistryPath, L"BootInProgress", 0) != 0) {
        HwLog("WATCHDOG: previous GPU boot did not complete; skipping HW boot for safety!");
        HwRegSetSvcDword(L"BootInProgress", 0);
        return STATUS_DEVICE_NOT_READY;
    }
    HwRegSetSvcDword(L"BootInProgress", 1);

    HwParseResources(hw, &info, &gpu_pa, &gmu_pa, &cx_pa, &rscc_pa, &smmu_pa,
                     &gpu_size, &gmu_size, &cx_size, &rscc_size, &smmu_size);

    {
        ULONG v;
        v = HwRegDword(&info.DeviceRegistryPath, L"UseSmmu",
                       hw->use_smmu ? 1 : 0);
        hw->use_smmu = v != 0;
        v = HwRegDword(&info.DeviceRegistryPath, L"UseDefaultAddresses", 1);
        if (v) {
            gpu_pa = 0;
            gmu_pa = 0;
            cx_pa = 0;
            rscc_pa = 0;
            smmu_pa = 0;
        }
        v = HwRegDword(&info.DeviceRegistryPath, L"GpuPa", 0);
        if (v) gpu_pa = v;
        v = HwRegDword(&info.DeviceRegistryPath, L"GmuPa", 0);
        if (v) gmu_pa = v;
        v = HwRegDword(&info.DeviceRegistryPath, L"CxMiscPa", 0);
        if (v) cx_pa = v;
        v = HwRegDword(&info.DeviceRegistryPath, L"RsccPa", 0);
        if (v) rscc_pa = v;
        v = HwRegDword(&info.DeviceRegistryPath, L"SmmuPa", 0);
        if (v) smmu_pa = v;
    }

    if (!gpu_pa) {
        HwLog("using SM8450 physical MMIO defaults");
        gpu_pa = ADRENO_A730_GPU_REG_BASE;
        gmu_pa = ADRENO_A730_GMU_BASE;
        cx_pa = ADRENO_A730_CX_MEM_BASE;
        rscc_pa = ADRENO_A730_GMU_RSCC_BASE;
        smmu_pa = ADRENO_A730_SMMU_BASE;
    }

    hw->gpu_size = gpu_size;
    hw->gmu_size = gmu_size;
    hw->cx_size = cx_size;
    hw->rscc_size = rscc_size;
    hw->smmu_size = smmu_size;

    if (!HwMapWindow(hw, gpu_pa, gpu_size, &hw->gpu_va, &hw->gpu_mm, "gpu") ||
        !HwMapWindow(hw, gmu_pa, gmu_size, &hw->gmu_va, &hw->gmu_mm, "gmu") ||
        !HwMapWindow(hw, cx_pa, cx_size, &hw->cx_va, &hw->cx_mm, "cx") ||
        !HwMapWindow(hw, rscc_pa, rscc_size, &hw->rscc_va, &hw->rscc_mm, "rscc") ||
        !HwMapWindow(hw, smmu_pa, smmu_size, &hw->smmu_va, &hw->smmu_mm, "smmu")) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }

    RtlZeroMemory(&ops, sizeof(ops));
    ops.ctx = hw;
    ops.read32 = HwRead32;
    ops.write32 = HwWrite32;
    ops.write_bulk = HwWriteBulk;
    ops.udelay = HwUdelay;
    ops.log = HwLogCb;
    adreno_a730_init(&hw->gpu, &ops, ADRENO_CHIP_ID_A730);

    /* Stage 1: Enable GCC + GPUCC clocks & CX GDSC BEFORE touching GPU/GMU/SMMU */
    status = WddmPowerOnSm8450Clocks(hw);
    if (!NT_SUCCESS(status))
        goto fail;

    /* Load firmware blobs */
    status = HwReadFile(L"gmu_gen70000.bin", &hw->gmu_fw, &hw->gmu_fw_size);
    if (!NT_SUCCESS(status))
        goto fail;
    status = HwReadFile(L"a730_sqe.fw", &hw->sqe_fw, &hw->sqe_fw_size);
    if (!NT_SUCCESS(status))
        goto fail;
    st = adreno_fw_parse_gmu(hw->gmu_fw, hw->gmu_fw_size, &hw->gmu_image);
    if (st != ADRENO_OK) {
        HwLog("parse GMU fw failed: %s", adreno_status_str(st));
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto fail;
    }
    st = adreno_fw_parse_sqe(hw->sqe_fw, hw->sqe_fw_size, &hw->sqe_info);
    if (st != ADRENO_OK) {
        HwLog("parse SQE fw failed: %s", adreno_status_str(st));
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto fail;
    }
    hw->firmware_loaded = true;

    /* Allocate all DMA blocks (MmWriteCombined + MM_ANY_NODE_OK) */
    if (!HwAllocBlock(&hw->hfi_mem, ADRENO_HFI_TABLE_SIZE, "hfi") ||
        !HwAllocBlock(&hw->ring_mem, 65536, "ring") ||
        !HwAllocBlock(&hw->fence_mem, 4096, "fence") ||
        !HwAllocBlock(&hw->shadow_mem, 4096, "shadow") ||
        !HwAllocBlock(&hw->sqe_mem,
                      hw->sqe_fw_size ? (hw->sqe_fw_size + 4095) & ~4095u
                                      : 65536, "sqe") ||
        !HwAllocBlock(&hw->ib_mem, 65536, "ib") ||
        !HwAllocBlock(&hw->icache_mem, 1024 * 1024, "icache") ||
        !HwAllocBlock(&hw->dummy_mem, 16384, "dummy") ||
        !HwAllocBlock(&hw->log_mem, 16384, "log") ||
        !HwAllocBlock(&hw->dcache_mem, 256 * 1024, "dcache") ||
        !HwAllocBlock(&hw->dbg_mem, 28 * 1024, "debug") ||
        (hw->use_smmu && !HwAllocBlock(&hw->pt_pool, 2 * 1024 * 1024, "pt"))) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    /* Skip 4-byte SQE header word (matches Linux a6xx_ucode_load_buf) */
    if (hw->sqe_fw_size > 4)
        RtlCopyMemory(hw->sqe_mem.host, hw->sqe_fw + 4, hw->sqe_fw_size - 4);

    /* Stage 2: Windows & SMMU-500 */
    HwSetupWindows(hw);
    st = HwSetupSmmu(hw);
    if (st != ADRENO_OK) {
        HwLog("STAGE2 SMMU failed: %s", adreno_status_str(st));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }

    /* Stage 3: RSCC + PDC + GMU Cortex-M3 boot */
    WddmInitGmuRpmh(hw);
    hfiw = adreno_a730_find_window(&hw->gpu, "hfi");
    if (!hfiw ||
        adreno_hfi_init(&hw->hfi, hw->hfi_mem.host, hw->hfi_mem.size,
                        hfiw->iova) != ADRENO_OK) {
        status = STATUS_INTERNAL_ERROR;
        goto fail;
    }
    KeMemoryBarrier();
    st = adreno_a730_gmu_boot(&hw->gpu, &hw->gmu_image, 1, 100000);
    HwLog("STAGE3 GMU_BOOT: %s (fw_init=0x%x hfi_st=0x%x ver=0x%x)",
          adreno_status_str(st),
          HwRead32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_CM3_FW_INIT_RESULT),
          HwRead32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_HFI_CTRL_STATUS),
          HwRead32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_CORE_FW_VERSION));
    if (st != ADRENO_OK) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }

    /* Stage 4: cmd-db + HFI startup + GX OPP #1 power-on */
    st = WddmRunHfiStart(hw);
    HwLog("STAGE4 HFI_START: %s (sptprac=0x%x RBBM=0x%x)",
          adreno_status_str(st),
          HwRead32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS),
          HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_RBBM_STATUS));
    if (st != ADRENO_OK) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }

    /* Stage 5: GMU_OOB_GPU_SET + A730 hw_init + UCHE/APRIV + ring + CP_START */
    st = WddmGmuSetOobGpu(hw);
    if (st != ADRENO_OK) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }
    st = adreno_a730_hw_init(&hw->gpu, 0);
    if (st != ADRENO_OK) {
        HwLog("STAGE5a HW_INIT failed: %s", adreno_status_str(st));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }
    HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_UCHE_CACHE_WAYS, 0x00840004u);
    HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_UCHE_CMDQ_CONFIG, 0x0006690eu);
    HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_CP_APRIV_CNTL, 0x0000006fu);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x0ad0u /* A7XX_CP_BV_APRIV_CNTL */, 0x0000000fu);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x0b31u /* A7XX_CP_LPAC_APRIV_CNTL */, 0x0000000fu);
    HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_UCHE_TRAP_BASE, 0xfffff000u);
    HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_UCHE_TRAP_BASE + 1u, 0x000001ffu);
    HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_UCHE_WRITE_THRU_BASE, 0xfffff000u);
    HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_UCHE_WRITE_THRU_BASE + 1u, 0x000001ffu);
    /* SM8450 / A730 UBWC 4.0 & non-UBWC ordering config (Linux a6xx_set_ubwc_config) */
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x8e08u /* RB_NC_MODE_CNTL */, 0x00000806u);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0xb604u /* TPL1_NC_MODE_CNTL */, 0x00000006u);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0xae02u /* SP_NC_MODE_CNTL */, 0x00000026u);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x0a00u /* CP_APERTURE_CNTL_HOST */, 1u << 12);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x8602u /* GRAS_NC_MODE_CNTL (BR) */, 0x00000060u);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x0a00u /* CP_APERTURE_CNTL_HOST */, 2u << 12);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x8602u /* GRAS_NC_MODE_CNTL (BV) */, 0x00000060u);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x0a00u /* CP_APERTURE_CNTL_HOST */, 0u);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x0e01u /* UCHE_MODE_CNTL */, 0x00600000u);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x0534u /* RBBM_NC_MODE_CNTL */, 0x00000001u);
    HwWrite32(hw, ADRENO_SPACE_GPU, 0x8e28u /* RB_CMP_DBG_ECO_CNTL */,
              HwRead32(hw, ADRENO_SPACE_GPU, 0x8e28u) | (1u << 11));
    HwLog("STAGE5a HW_INIT OK (RBBM=0x%x)",
          HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_RBBM_STATUS));

    st = adreno_a730_ring_init(&hw->gpu);
    if (st != ADRENO_OK) {
        HwLog("STAGE5b RING_INIT failed: %s", adreno_status_str(st));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }

    pwrup = adreno_a730_find_window(&hw->gpu, "shadow");
    if (pwrup) {
        RtlZeroMemory(pwrup->host, pwrup->size);
        pwrup_iova = (uint32_t)pwrup->iova;
    }
    KeMemoryBarrier();
    st = adreno_a730_cp_start(&hw->gpu, pwrup_iova, 500000);
    {
        uint32_t cb0_base = (hw->smmu.numpage ? hw->smmu.numpage : 32u) * 0x1000u;
        HwLog("STAGE5c CP_START: %s (RBBM=0x%x RPTR=0x%x WPTR=0x%x CB0_FSR=0x%x FAR=0x%x)",
              adreno_status_str(st),
              HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_RBBM_STATUS),
              HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_RPTR),
              HwRead32(hw, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_WPTR),
              HwRead32(hw, ADRENO_SPACE_SMMU, (cb0_base + 0x58u) >> 2),
              HwRead32(hw, ADRENO_SPACE_SMMU, (cb0_base + 0x60u) >> 2));
    }
    if (st != ADRENO_OK) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }

    st = WddmZapAuthAndUnsecure(hw);
    if (st != ADRENO_OK)
        HwLog("STAGE5d ZAP_UNSECURE warning: %s (continuing)", adreno_status_str(st));

    hw->cp_started = true;
    hw->hw_ready = true;
    hw->seqno = 0;
    if (hw->has_irq)
        AdrenoWddmHwEnableInterrupts(hw);

    /* Live WDDM hardware CP_MEM_WRITE + IR3 COMPUTE SHADER self-tests */
    {
        adreno_mem_window *ibw = adreno_a730_find_window(&hw->gpu, "ib");
        if (ibw && ibw->host && ibw->size >= 0x4000) {
            adreno_pm4 s;
            uint32_t *cmds = (uint32_t *)ibw->host;
            volatile uint32_t *dst = (volatile uint32_t *)((uint8_t *)ibw->host + 0x1000);
            uint64_t dst_iova = ibw->iova + 0x1000ull;
            adreno_ib ib;
            bool done;

            *dst = 0x11223344u;
            adreno_pm4_init(&s, cmds, 128);
            adreno_pm4_mem_write(&s, dst_iova, 0xcafebabeu);
            KeMemoryBarrier();

            RtlZeroMemory(&ib, sizeof(ib));
            ib.iova = ibw->iova;
            ib.size_dwords = s.count;
            st = AdrenoWddmHwSubmit(hw, &ib, 1, 1);
            done = (st == ADRENO_OK) && AdrenoWddmHwWaitFence(hw, 1, 500);
            HwLog("HW self-test CP_MEM_WRITE: st=%s done=%u fence=%u readback=0x%08x (%s)",
                  adreno_status_str(st), done ? 1u : 0u, AdrenoWddmHwFence(hw),
                  *dst, (done && *dst == 0xcafebabeu) ? "PASS" : "FAIL");

            /* Real A730 IR3 compute shader dispatch writing 0xdeadbeef via SP->UCHE->DDR */
            if (done && *dst == 0xcafebabeu) {
                uint8_t *sh_host = (uint8_t *)ibw->host + 0x2000;
                uint64_t sh_iova = ibw->iova + 0x2000ull;
                volatile uint32_t *out_host = (volatile uint32_t *)((uint8_t *)ibw->host + 0x3000);
                uint64_t out_iova = ibw->iova + 0x3000ull;
                uint32_t consts[8];

                RtlCopyMemory(sh_host, adreno_ir3_ssbo_test_shader,
                              ADRENO_IR3_SSBO_TEST_SHADER_BYTES);
                *out_host = 0x55555555u;
                consts[0] = (uint32_t)out_iova;
                consts[1] = (uint32_t)(out_iova >> 32);
                consts[2] = 0xdeadbeefu;
                consts[3] = 0u;
                consts[4] = 0xdeadbeefu;
                consts[5] = 0u;
                consts[6] = 0u;
                consts[7] = 0u;

                adreno_pm4_init(&s, cmds, 256);
                adreno_pm4_reg_write(&s, REG_A6XX_SP_MODE_CNTL, 0x5u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_UPDATE_CNTL, 0xbfu);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_CONST_CONFIG, 0x00000102u);
                adreno_pm4_reg_write(&s, REG_A6XX_SP_CS_CONFIG, 0x100u);
                adreno_pm4_reg_write(&s, REG_A6XX_SP_CS_INSTR_SIZE, 1u);
                adreno_pm4_reg_write(&s, REG_A6XX_SP_CS_CNTL_0, (4u << 7) | (1u << 31));
                adreno_pm4_reg_write(&s, REG_A7XX_SP_PS_WAVE_CNTL, 0u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_REG_PROG_ID_0, 0xfcfcfcfcu);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_REG_PROG_ID_1, 0xfcfcfcfcu);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_REG_PROG_ID_2, 0xfcfcfcfcu);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_REG_PROG_ID_3, 0x0000fc00u);
                adreno_pm4_reg_write(&s, REG_A6XX_SP_CS_CNTL_1, 0u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_WGE_CNTL,
                                     0x00fcu | (1u << 11) | (4u << 20) | (17u << 26));
                adreno_pm4_reg_write(&s, REG_A6XX_SP_CS_WIE_CNTL_0, 0xfcfcfcfcu);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_WIE_CNTL_1, 0x00fcu);
                adreno_pm4_reg_write64(&s, REG_A6XX_SP_CS_BASE, sh_iova);
                adreno_pm4_reg_write(&s, REG_A6XX_SP_CS_INSTR_SIZE, 1u);
                adreno_pm4_load_state6_shader(&s, ADRENO_SB6_CS_SHADER, sh_iova, 1u);
                adreno_pm4_load_state6_const(&s, ADRENO_SB6_CS_SHADER, 0u, consts, 2u);
                adreno_pm4_set_marker(&s, 8u /* RM6_COMPUTE */);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_NDRANGE_0, 1u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_NDRANGE_7, 0u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_NDRANGE_1, 1u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_NDRANGE_2, 0u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_NDRANGE_3, 1u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_NDRANGE_4, 0u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_NDRANGE_5, 1u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_NDRANGE_6, 0u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_KERNEL_GROUP_X, 1u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_KERNEL_GROUP_Y, 1u);
                adreno_pm4_reg_write(&s, REG_A7XX_SP_CS_KERNEL_GROUP_Z, 1u);
                adreno_pm4_exec_cs(&s, 1u, 1u, 1u);
                adreno_pm4_wait_for_idle(&s);
                adreno_pm4_cache_flush_ts(&s);
                adreno_pm4_wait_for_idle(&s);
                KeMemoryBarrier();

                ib.iova = ibw->iova;
                ib.size_dwords = s.count;
                st = AdrenoWddmHwSubmit(hw, &ib, 1, 2);
                done = (st == ADRENO_OK) && AdrenoWddmHwWaitFence(hw, 2, 500);
                HwLog("HW self-test IR3 COMPUTE: st=%s done=%u fence=%u readback=0x%08x TSB=0x%x_%08x (%s)",
                      adreno_status_str(st), done ? 1u : 0u, AdrenoWddmHwFence(hw),
                      *out_host,
                      HwRead32(hw, ADRENO_SPACE_GPU, 0xfc01u),
                      HwRead32(hw, ADRENO_SPACE_GPU, 0xfc00u),
                      (done && *out_host == 0xdeadbeefu) ? "PASS" : "FAIL");
            }
        }
    }

    HwLog("hardware ready: chip 0x%08x, SMMU=%u, CP started, has_irq=%u",
          hw->gpu.chip_id, hw->smmu_ready ? 1u : 0u, hw->has_irq ? 1u : 0u);
    HwRegSetSvcDword(L"BootInProgress", 0);
    return STATUS_SUCCESS;

fail:
    HwRegSetSvcDword(L"BootInProgress", 0);
    AdrenoWddmHwStop(hw);
    return status;
}

void AdrenoWddmHwStop(ADRENO_WDDM_HW *hw)
{
    if (!hw)
        return;
    /* Halt CP, GMU, and SMMU BEFORE freeing DMA buffers! */
    if (hw->gpu_va && hw->cp_started) {
        HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_RBBM_INT_0_MASK, 0u);
        HwWrite32(hw, ADRENO_SPACE_GPU, REG_A6XX_CP_SQE_CNTL, 0u);
    }
    if (hw->gmu_va && hw->gpu.gmu_booted) {
        HwWrite32(hw, ADRENO_SPACE_GMU, REG_A6XX_GMU_CM3_SYSRESET, 1u);
    }
    if (hw->smmu_va && hw->smmu_ready) {
        uint32_t scr0 = HwRead32(hw, ADRENO_SPACE_SMMU, 0);
        HwWrite32(hw, ADRENO_SPACE_SMMU, 0, scr0 | 1u);
    }
    hw->hw_ready = false;
    hw->cp_started = false;
    HwFreeBlock(&hw->hfi_mem);
    HwFreeBlock(&hw->ring_mem);
    HwFreeBlock(&hw->fence_mem);
    HwFreeBlock(&hw->shadow_mem);
    HwFreeBlock(&hw->sqe_mem);
    HwFreeBlock(&hw->ib_mem);
    HwFreeBlock(&hw->icache_mem);
    HwFreeBlock(&hw->dummy_mem);
    HwFreeBlock(&hw->log_mem);
    HwFreeBlock(&hw->dcache_mem);
    HwFreeBlock(&hw->dbg_mem);
    HwFreeBlock(&hw->pt_pool);
    if (hw->gmu_fw) {
        ExFreePoolWithTag(hw->gmu_fw, ADRENO_HW_TAG);
        hw->gmu_fw = NULL;
    }
    if (hw->sqe_fw) {
        ExFreePoolWithTag(hw->sqe_fw, ADRENO_HW_TAG);
        hw->sqe_fw = NULL;
    }
    HwUnmapWindow(hw, hw->gpu_va, hw->gpu_size, hw->gpu_mm);
    HwUnmapWindow(hw, hw->gmu_va, hw->gmu_size, hw->gmu_mm);
    HwUnmapWindow(hw, hw->cx_va, hw->cx_size, hw->cx_mm);
    HwUnmapWindow(hw, hw->rscc_va, hw->rscc_size, hw->rscc_mm);
    HwUnmapWindow(hw, hw->smmu_va, hw->smmu_size, hw->smmu_mm);
    hw->gpu_va = NULL;
    hw->gmu_va = NULL;
    hw->cx_va = NULL;
    hw->rscc_va = NULL;
    hw->smmu_va = NULL;
    hw->smmu_ready = false;
    hw->firmware_loaded = false;
}

bool AdrenoWddmHwSmmuReady(const ADRENO_WDDM_HW *hw)
{
    return hw && hw->smmu_ready;
}

bool AdrenoWddmHwCpStarted(const ADRENO_WDDM_HW *hw)
{
    return hw && hw->cp_started;
}

uint64_t AdrenoWddmHwAllocIova(ADRENO_WDDM_HW *hw, uint32_t size)
{
    uint64_t iova;
    if (!hw || !hw->smmu_ready || size == 0)
        return 0;
    iova = ADRENO_ALIGN_UP(hw->next_alloc_iova, 0x10000);
    hw->next_alloc_iova = iova + ADRENO_ALIGN_UP(size, 0x1000);
    return iova;
}

enum adreno_status AdrenoWddmHwMapIova(ADRENO_WDDM_HW *hw, uint64_t iova,
                                        uint64_t pa, uint32_t size)
{
    enum adreno_status st;
    if (!hw || !hw->smmu_ready || !iova || !pa || !size)
        return ADRENO_ERR_INVALID;
    st = adreno_pt_map(&hw->gpu_pt, iova, pa, size, false);
    if (st != ADRENO_OK)
        return st;
    st = adreno_pt_map(&hw->gmu_pt, iova, pa, size, false);
    if (st != ADRENO_OK) {
        (void)adreno_pt_unmap(&hw->gpu_pt, iova, size);
        return st;
    }
    adreno_smmu_tlb_invalidate(&hw->smmu);
    return ADRENO_OK;
}

void AdrenoWddmHwUnmapIova(ADRENO_WDDM_HW *hw, uint64_t iova, uint32_t size)
{
    if (!hw || !hw->smmu_ready || !iova || !size)
        return;
    (void)adreno_pt_unmap(&hw->gpu_pt, iova, size);
    (void)adreno_pt_unmap(&hw->gmu_pt, iova, size);
    adreno_smmu_tlb_invalidate(&hw->smmu);
}

enum adreno_status AdrenoWddmHwSubmit(ADRENO_WDDM_HW *hw,
                                       const adreno_ib *ibs, uint32_t count,
                                       uint32_t seqno)
{
    if (!hw || !hw->cp_started)
        return ADRENO_ERR_STATE;
    adreno_ring_set_rptr(&hw->gpu.ring,
                         adreno_read(&hw->gpu, ADRENO_SPACE_GPU, REG_A6XX_CP_RB_RPTR));
    if (seqno == 0)
        seqno = ++hw->seqno;
    else if (seqno > hw->seqno)
        hw->seqno = seqno;
    KeMemoryBarrier();
    return adreno_a730_submit(&hw->gpu, &hw->gpu.ring, ibs, count, seqno);
}

bool AdrenoWddmHwWaitFence(ADRENO_WDDM_HW *hw, uint32_t seqno,
                           uint32_t timeout_ms)
{
    LARGE_INTEGER now;
    LONGLONG start, limit;
    volatile uint32_t *fence;

    if (!hw || !hw->fence_mem.host)
        return false;
    fence = (volatile uint32_t *)hw->fence_mem.host;
    if (timeout_ms > 10000)
        timeout_ms = 10000;
    KeQuerySystemTime(&now);
    start = now.QuadPart;
    limit = (LONGLONG)timeout_ms * 10000;
    for (;;) {
        if (*fence >= seqno)
            return true;
        KeQuerySystemTime(&now);
        if (now.QuadPart - start >= limit)
            return false;
        KeStallExecutionProcessor(100);
    }
}

uint32_t AdrenoWddmHwFence(const ADRENO_WDDM_HW *hw)
{
    if (!hw || !hw->fence_mem.host)
        return 0;
    return *(volatile uint32_t *)hw->fence_mem.host;
}

uint32_t AdrenoWddmHwIrqStatus(ADRENO_WDDM_HW *hw)
{
    uint32_t status;
    if (!hw || !hw->hw_ready)
        return 0;
    status = adreno_read(&hw->gpu, ADRENO_SPACE_GPU,
                         REG_A6XX_RBBM_INT_0_STATUS);
    if (status)
        adreno_write(&hw->gpu, ADRENO_SPACE_GPU,
                     REG_A6XX_RBBM_INT_CLEAR_CMD, status);
    return status;
}

void AdrenoWddmHwIrqAck(ADRENO_WDDM_HW *hw, uint32_t status)
{
    if (!hw || !hw->hw_ready || !status)
        return;
    adreno_write(&hw->gpu, ADRENO_SPACE_GPU, REG_A6XX_RBBM_INT_CLEAR_CMD,
                 status);
}

void AdrenoWddmHwEnableInterrupts(ADRENO_WDDM_HW *hw)
{
    if (!hw || !hw->hw_ready)
        return;
    adreno_write(&hw->gpu, ADRENO_SPACE_GPU, REG_A6XX_RBBM_INT_0_MASK,
                 0x00100000u);
}
