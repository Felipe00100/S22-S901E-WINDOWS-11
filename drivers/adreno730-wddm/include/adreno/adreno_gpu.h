/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_GPU_H
#define ADRENO_GPU_H

#include "adreno_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Chip IDs as used by the upstream drm/msm adreno catalog and ACPI/DT. */
#define ADRENO_CHIP_ID_A730 0x07030001u

#define ADRENO_A730_GMEM_SIZE (2u * 1024u * 1024u)
#define ADRENO_A730_NUM_RINGS 4u
#define ADRENO_A730_PREEMPT_RECORD_SIZE (2860u * 1024u)

#define ADRENO_A730_FW_SQE "a730_sqe.fw"
#define ADRENO_A730_FW_GMU "gmu_gen70000.bin"
#define ADRENO_A730_FW_ZAP "a730_zap.mdt"

/* Register/MMIO windows from the SM8450 device tree. All sizes in bytes. */
#define ADRENO_A730_GPU_REG_BASE 0x03d00000u
#define ADRENO_A730_GPU_REG_SIZE 0x00040000u
#define ADRENO_A730_CX_MEM_BASE 0x03d9e000u
#define ADRENO_A730_CX_MEM_SIZE 0x00001000u
#define ADRENO_A730_CX_DBGC_BASE 0x03d61000u
#define ADRENO_A730_CX_DBGC_SIZE 0x00000800u

#define ADRENO_A730_GMU_BASE 0x03d6a000u
#define ADRENO_A730_GMU_SIZE 0x00035000u
#define ADRENO_A730_GMU_RSCC_BASE 0x03d50000u
#define ADRENO_A730_GMU_RSCC_SIZE 0x00010000u
#define ADRENO_A730_GMU_PDC_BASE 0x0b290000u
#define ADRENO_A730_GMU_PDC_SIZE 0x00010000u

#define ADRENO_A730_SMMU_BASE 0x03da0000u
#define ADRENO_A730_SMMU_SIZE 0x00040000u

/*
 * GMU register offsets in the freedreno database are absolute GPU-window
 * dword offsets. The GMU's own 0x35000-byte MMIO window starts at 0x3d6a000,
 * i.e. 0x6a000 bytes into the GPU window. A GMU-space access at register R is
 * therefore at byte offset (R * 4) - ADRENO_A730_GMU_MMIO_BIAS inside the GMU
 * mapping. The CX_MISC mapping at 0x3d9e000 uses plain R * 4.
 */
#define ADRENO_A730_GMU_MMIO_BIAS 0x0006a000u

/* IRQ numbers from the SM8450 device tree (GIC SPI, not Windows IRQ). */
#define ADRENO_A730_IRQ_GPU 300
#define ADRENO_A730_IRQ_GMU_HFI 304
#define ADRENO_A730_IRQ_GMU 305

typedef enum adreno_family {
    ADRENO_FAMILY_UNKNOWN = 0,
    ADRENO_FAMILY_A2XX,
    ADRENO_FAMILY_A3XX,
    ADRENO_FAMILY_A4XX,
    ADRENO_FAMILY_A5XX,
    ADRENO_FAMILY_A6XX_GEN1,
    ADRENO_FAMILY_A6XX_GEN2,
    ADRENO_FAMILY_A6XX_GEN3,
    ADRENO_FAMILY_A6XX_GEN4,
    ADRENO_FAMILY_A7XX_GEN1,
    ADRENO_FAMILY_A7XX_GEN2,
    ADRENO_FAMILY_A7XX_GEN3,
    ADRENO_FAMILY_A7XX_GEN4,
    ADRENO_FAMILY_A8XX,
} adreno_family;

typedef enum adreno_quirk {
    ADRENO_QUIRK_NONE = 0,
    ADRENO_QUIRK_CACHED_COHERENT = ADRENO_BIT(0),
    ADRENO_QUIRK_HW_APRIV = ADRENO_BIT(1),
    ADRENO_QUIRK_PREEMPTION = ADRENO_BIT(2),
} adreno_quirk;

typedef struct adreno_gpu_info {
    uint32_t chip_id;
    const char *name;
    adreno_family family;
    uint32_t gmem_size;
    uint32_t num_rings;
    uint32_t preempt_record_size;
    const char *fw_sqe;
    const char *fw_gmu;
    const char *fw_zap;
    uint32_t quirks;
} adreno_gpu_info;

extern const adreno_gpu_info adreno_a730_info;

const adreno_gpu_info *adreno_gpu_info_from_chip_id(uint32_t chip_id);
bool adreno_is_a7xx(const adreno_gpu_info *info);
const char *adreno_family_name(adreno_family family);

/* The firmware blobs are distributed by linux-firmware under qcom/. */
const char *adreno_firmware_signed_name(const char *name);
const char *adreno_firmware_legacy_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_GPU_H */
