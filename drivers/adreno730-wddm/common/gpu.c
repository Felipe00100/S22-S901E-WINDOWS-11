/* SPDX-License-Identifier: MIT */
#include "adreno/adreno_gpu.h"

#include <string.h>

/*
 * Values are taken from the upstream adreno catalog:
 *   drivers/gpu/drm/msm/adreno/a7xx_catalog.c
 *   drivers/gpu/drm/msm/adreno/adreno_gpu.h (adreno_is_a730)
 * and from arch/arm64/boot/dts/qcom/sm8450.dtsi.
 */
const adreno_gpu_info adreno_a730_info = {
    .chip_id = ADRENO_CHIP_ID_A730,
    .name = "Adreno 730",
    .family = ADRENO_FAMILY_A7XX_GEN1,
    .gmem_size = ADRENO_A730_GMEM_SIZE,
    .num_rings = ADRENO_A730_NUM_RINGS,
    .preempt_record_size = ADRENO_A730_PREEMPT_RECORD_SIZE,
    .fw_sqe = ADRENO_A730_FW_SQE,
    .fw_gmu = ADRENO_A730_FW_GMU,
    .fw_zap = ADRENO_A730_FW_ZAP,
    .quirks = ADRENO_QUIRK_CACHED_COHERENT |
              ADRENO_QUIRK_HW_APRIV |
              ADRENO_QUIRK_PREEMPTION,
};

const adreno_gpu_info *adreno_gpu_info_from_chip_id(uint32_t chip_id)
{
    if (chip_id == ADRENO_CHIP_ID_A730)
        return &adreno_a730_info;
    return NULL;
}

bool adreno_is_a7xx(const adreno_gpu_info *info)
{
    return info && info->family >= ADRENO_FAMILY_A7XX_GEN1 &&
           info->family <= ADRENO_FAMILY_A7XX_GEN4;
}

const char *adreno_family_name(adreno_family family)
{
    switch (family) {
    case ADRENO_FAMILY_UNKNOWN: return "unknown";
    case ADRENO_FAMILY_A2XX: return "a2xx";
    case ADRENO_FAMILY_A3XX: return "a3xx";
    case ADRENO_FAMILY_A4XX: return "a4xx";
    case ADRENO_FAMILY_A5XX: return "a5xx";
    case ADRENO_FAMILY_A6XX_GEN1: return "a6xx-gen1";
    case ADRENO_FAMILY_A6XX_GEN2: return "a6xx-gen2";
    case ADRENO_FAMILY_A6XX_GEN3: return "a6xx-gen3";
    case ADRENO_FAMILY_A6XX_GEN4: return "a6xx-gen4";
    case ADRENO_FAMILY_A7XX_GEN1: return "a7xx-gen1";
    case ADRENO_FAMILY_A7XX_GEN2: return "a7xx-gen2";
    case ADRENO_FAMILY_A7XX_GEN3: return "a7xx-gen3";
    case ADRENO_FAMILY_A7XX_GEN4: return "a7xx-gen4";
    case ADRENO_FAMILY_A8XX: return "a8xx";
    }
    return "unknown";
}

/*
 * Linux historically looked for firmware under qcom/ first. linux-firmware
 * installs the signed variants next to the plain names. The Windows build
 * ships the blobs in System32\drivers and accepts either spelling.
 */
const char *adreno_firmware_signed_name(const char *name)
{
    static char buf[128];
    size_t len;

    if (!name)
        return NULL;
    if (strncmp(name, "qcom/", 5) == 0)
        return name;
    len = strlen(name);
    if (len + 6 > sizeof(buf))
        return name;
    memcpy(buf, "qcom/", 5);
    memcpy(buf + 5, name, len + 1);
    return buf;
}

const char *adreno_firmware_legacy_name(const char *name)
{
    return name && strncmp(name, "qcom/", 5) == 0 ? name + 5 : name;
}
