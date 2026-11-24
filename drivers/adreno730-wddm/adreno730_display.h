/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 *
 * Adreno 730 Full WDDM 2.0 Display / VidPN / 60Hz VSync / Scanout module
 * for Samsung Galaxy S22 (SM-S901E / SM8450).
 */
#ifndef ADRENO730_DISPLAY_H
#define ADRENO730_DISPLAY_H

#ifndef DXGKDDI_INTERFACE_VERSION
#define DXGKDDI_INTERFACE_VERSION 0x5023 /* DXGKDDI_INTERFACE_VERSION_WDDM2_0 */
#endif

#include <ntddk.h>
#include <windef.h>
#include <d3dkmddi.h>
#include <dispmprt.h>
#include <stdint.h>
#include <stdbool.h>

#define ADRENO_DISP_CHILD_UID        0x00000100u
#define ADRENO_DISP_DEFAULT_WIDTH    1080u
#define ADRENO_DISP_DEFAULT_HEIGHT   2340u
#define ADRENO_DISP_DEFAULT_REFRESH  60u
#define ADRENO_DISP_DEFAULT_FB_PA    0xB8000000ull

typedef struct ADRENO_WDDM_DISPLAY {
    BOOLEAN enabled;
    BOOLEAN vsync_enabled;
    BOOLEAN source_visible;
    BOOLEAN vidpn_committed;

    DXGKRNL_INTERFACE dxgk;
    HANDLE device_handle;

    UINT width;
    UINT height;
    UINT pitch;
    D3DDDIFORMAT format;

    PHYSICAL_ADDRESS fb_pa;
    SIZE_T fb_size;
    PVOID fb_va;

    PHYSICAL_ADDRESS current_primary_pa;
    HANDLE current_primary_alloc;

    KTIMER vsync_timer;
    KDPC vsync_dpc;
    volatile LONG vsync_active;
    volatile LONG vsync_count;
} ADRENO_WDDM_DISPLAY, *PADRENO_WDDM_DISPLAY;

BOOLEAN AdrenoDispReadEnableFlag(void);

NTSTATUS AdrenoDispStart(PADRENO_WDDM_DISPLAY disp, HANDLE device_handle,
                         const DXGKRNL_INTERFACE *dxgk);
VOID AdrenoDispStop(PADRENO_WDDM_DISPLAY disp);

NTSTATUS AdrenoDispQueryChildRelations(PADRENO_WDDM_DISPLAY disp,
                                       PDXGK_CHILD_DESCRIPTOR relations,
                                       ULONG size);
NTSTATUS AdrenoDispQueryChildStatus(PADRENO_WDDM_DISPLAY disp,
                                    PDXGK_CHILD_STATUS child_status,
                                    BOOLEAN non_destructive);
NTSTATUS AdrenoDispQueryDeviceDescriptor(PADRENO_WDDM_DISPLAY disp,
                                         ULONG child_uid,
                                         PDXGK_DEVICE_DESCRIPTOR desc);

NTSTATUS AdrenoDispIsSupportedVidPn(PADRENO_WDDM_DISPLAY disp,
                                    DXGKARG_ISSUPPORTEDVIDPN *arg);
NTSTATUS AdrenoDispRecommendFunctionalVidPn(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *arg);
NTSTATUS AdrenoDispEnumVidPnCofuncModality(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *arg);
NTSTATUS AdrenoDispRecommendMonitorModes(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_RECOMMENDMONITORMODES *arg);
NTSTATUS AdrenoDispRecommendVidPnTopology(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_RECOMMENDVIDPNTOPOLOGY *arg);
NTSTATUS AdrenoDispSetVidPnSourceVisibility(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_SETVIDPNSOURCEVISIBILITY *arg);
NTSTATUS AdrenoDispCommitVidPn(PADRENO_WDDM_DISPLAY disp,
                               const DXGKARG_COMMITVIDPN *arg);
NTSTATUS AdrenoDispUpdateActiveVidPnPresentPath(
    PADRENO_WDDM_DISPLAY disp,
    const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *arg);
NTSTATUS AdrenoDispSetVidPnSourceAddress(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_SETVIDPNSOURCEADDRESS *arg);
NTSTATUS AdrenoDispGetScanLine(PADRENO_WDDM_DISPLAY disp,
                               DXGKARG_GETSCANLINE *arg);
NTSTATUS AdrenoDispControlVsync(PADRENO_WDDM_DISPLAY disp, BOOLEAN enable);
NTSTATUS AdrenoDispQueryVidPnHWCapability(PADRENO_WDDM_DISPLAY disp,
                                          DXGKARG_QUERYVIDPNHWCAPABILITY *arg);
NTSTATUS AdrenoDispStopAndReleasePostDisplay(PADRENO_WDDM_DISPLAY disp,
                                             D3DDDI_VIDEO_PRESENT_TARGET_ID target_id,
                                             PDXGK_DISPLAY_INFORMATION info);
NTSTATUS AdrenoDispSystemDisplayEnable(PADRENO_WDDM_DISPLAY disp,
                                       D3DDDI_VIDEO_PRESENT_TARGET_ID target_id,
                                       PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS flags,
                                       UINT *width, UINT *height,
                                       D3DDDIFORMAT *format);
VOID AdrenoDispSystemDisplayWrite(PADRENO_WDDM_DISPLAY disp,
                                  PVOID source, UINT src_w, UINT src_h,
                                  UINT src_stride, UINT pos_x, UINT pos_y);
NTSTATUS AdrenoDispGetChildContainerId(PADRENO_WDDM_DISPLAY disp,
                                       ULONG child_uid,
                                       PDXGK_CHILD_CONTAINER_ID cid);

#endif /* ADRENO730_DISPLAY_H */
