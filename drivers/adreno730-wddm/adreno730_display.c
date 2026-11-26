/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
 *
 * Adreno 730 Full WDDM 2.0 Display / VidPN / 60Hz VSync / Scanout module
 * for Samsung Galaxy S22 (SM-S901E / SM8450).
 *
 * Stage 2A: Exposes a complete WDDM 2.0 VidPN display pipeline (1 source,
 * 1 internal 1080x2340@60Hz OLED target, valid EDID, 60Hz VSync timer DPC,
 * and scanout framebuffer mapping) gated by Services\adreno730_wddm\Parameters\EnableDisplay.
 */
#include "adreno730_display.h"
#include "adreno730_hw.h"

#define DLog(...) AdrenoWddmLogLine("adreno730_disp", __VA_ARGS__)

/* ---- 128-byte EDID for Samsung Galaxy S22 OLED (1080x2340 @ 60Hz) ------ */

static void BuildS22Edid(uint8_t edid[128], UINT width, UINT height)
{
    uint32_t hactive = width;
    uint32_t vactive = height;
    uint32_t hblank = 120;
    uint32_t vblank = 40;
    uint32_t pclk_10khz = ((hactive + hblank) * (vactive + vblank) * 60u) / 10000u;
    uint32_t sum = 0;

    RtlZeroMemory(edid, 128);

    /* EDID 1.4 Header */
    edid[0] = 0x00; edid[1] = 0xFF; edid[2] = 0xFF; edid[3] = 0xFF;
    edid[4] = 0xFF; edid[5] = 0xFF; edid[6] = 0xFF; edid[7] = 0x00;

    /* Manufacturer ID: "S22" -> S(19), M(13), S(19) -> "SMS" */
    edid[8] = 0x4D;
    edid[9] = 0xB3;
    edid[10] = 0x01; edid[11] = 0x09; /* Product code 0x0901 (SM-S901E) */
    edid[16] = 1;    /* Week 1 */
    edid[17] = 36;   /* Year 2026 */
    edid[18] = 1;    /* EDID 1.4 */
    edid[19] = 4;

    /* Basic display parameters: Digital, 8-bit, DisplayPort/DSI internal */
    edid[20] = 0xA5;
    edid[21] = 7;    /* 7 cm wide */
    edid[22] = 14;   /* 14 cm tall */
    edid[23] = 120;  /* Gamma 2.2 */
    edid[24] = 0x0A; /* sRGB + preferred timing */

    /* Chromaticity (standard sRGB) */
    edid[25] = 0xEE; edid[26] = 0x91; edid[27] = 0xA3; edid[28] = 0x54;
    edid[29] = 0x4C; edid[30] = 0x99; edid[31] = 0x26; edid[32] = 0x0F;
    edid[33] = 0x50; edid[34] = 0x54;

    /* Descriptor 1 (offset 54): Detailed Timing Descriptor */
    edid[54] = (uint8_t)(pclk_10khz & 0xFFu);
    edid[55] = (uint8_t)((pclk_10khz >> 8) & 0xFFu);
    edid[56] = (uint8_t)(hactive & 0xFFu);
    edid[57] = (uint8_t)(hblank & 0xFFu);
    edid[58] = (uint8_t)((((hactive >> 8) & 0x0Fu) << 4) | ((hblank >> 8) & 0x0Fu));
    edid[59] = (uint8_t)(vactive & 0xFFu);
    edid[60] = (uint8_t)(vblank & 0xFFu);
    edid[61] = (uint8_t)((((vactive >> 8) & 0x0Fu) << 4) | ((vblank >> 8) & 0x0Fu));
    edid[62] = 32;  /* H sync offset */
    edid[63] = 32;  /* H sync width */
    edid[64] = (uint8_t)((8u << 4) | 4u); /* V sync offset=8, width=4 */
    edid[65] = 0x00;
    edid[66] = 68;  /* 68 mm width */
    edid[67] = 146; /* 146 mm height */
    edid[68] = 0x00;
    edid[71] = 0x1E; /* Non-interlaced, digital separate sync */

    /* Descriptor 2 (offset 72): Display Product Name "S22 OLED" */
    edid[72] = 0x00; edid[73] = 0x00; edid[74] = 0x00; edid[75] = 0xFC; edid[76] = 0x00;
    edid[77] = 'S'; edid[78] = '2'; edid[79] = '2'; edid[80] = ' ';
    edid[81] = 'O'; edid[82] = 'L'; edid[83] = 'E'; edid[84] = 'D';
    edid[85] = '\n'; edid[86] = ' '; edid[87] = ' '; edid[88] = ' '; edid[89] = ' ';

    /* Descriptor 3 & 4: Dummy descriptors */
    edid[93]  = 0x10;
    edid[111] = 0x10;

    for (ULONG i = 0; i < 127; i++)
        sum += edid[i];
    edid[127] = (uint8_t)((256u - (sum & 0xFFu)) & 0xFFu);
}

/* ---- Registry helper --------------------------------------------------- */

BOOLEAN AdrenoDispReadEnableFlag(void)
{
    UNICODE_STRING key_path, val_name;
    OBJECT_ATTRIBUTES attr;
    HANDLE key = NULL;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG res = 0;
    BOOLEAN enabled = FALSE;

    RtlInitUnicodeString(
        &key_path,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\adreno730_wddm\\Parameters");
    InitializeObjectAttributes(&attr, &key_path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &attr)))
        return FALSE;

    RtlInitUnicodeString(&val_name, L"EnableDisplay");
    if (NT_SUCCESS(ZwQueryValueKey(key, &val_name, KeyValuePartialInformation,
                                   buf, sizeof(buf), &res)) &&
        info->Type == REG_DWORD && info->DataLength >= sizeof(ULONG)) {
        enabled = (*(ULONG *)info->Data != 0) ? TRUE : FALSE;
    }
    ZwClose(key);
    return enabled;
}

/* ---- PASSIVE_LEVEL Primary Scanout Blit Thread (0xB8000000) ------------ */

static VOID AdrenoDispScanoutThread(PVOID ctx)
{
    PADRENO_WDDM_DISPLAY disp = (PADRENO_WDDM_DISPLAY)ctx;

    while (disp && InterlockedCompareExchange(&disp->scanout_stop, 0, 0) == 0) {
        KeWaitForSingleObject(&disp->scanout_event, Executive, KernelMode, FALSE, NULL);
        if (InterlockedCompareExchange(&disp->scanout_stop, 0, 0) != 0)
            break;
        if (!disp->enabled || !disp->fb_va || !disp->source_visible || disp->fb_size == 0)
            continue;

        {
            UINT seg = disp->current_primary_seg;
            uint64_t addr = (uint64_t)disp->current_primary_pa.QuadPart;
            SIZE_T total_bytes = disp->fb_size;
            SIZE_T total_pages = (total_bytes + 4095u) >> 12;
            SIZE_T p;
            SIZE_T copied_total = 0;

            if (addr == 0 || addr == (uint64_t)disp->fb_pa.QuadPart)
                continue;

            if (seg == 1 && disp->aperture_pa && disp->aperture_pages > 0) {
                uint64_t base_page = addr >> 12;
                if (base_page + total_pages <= disp->aperture_pages) {
                    for (p = 0; p < total_pages; p++) {
                        uint64_t pa = disp->aperture_pa[base_page + p];
                        MM_COPY_ADDRESS src_addr;
                        SIZE_T chunk, copied = 0;
                        if (pa == 0 || pa == (uint64_t)disp->fb_pa.QuadPart)
                            break;
                        chunk = ((p + 1) * 4096u <= total_bytes) ? 4096u : (total_bytes - p * 4096u);
                        src_addr.PhysicalAddress.QuadPart = (LONGLONG)pa;
                        if (NT_SUCCESS(MmCopyMemory((uint8_t *)disp->fb_va + p * 4096u,
                                                    src_addr, chunk,
                                                    MM_COPY_MEMORY_PHYSICAL, &copied))) {
                            copied_total += copied;
                        }
                    }
                }
            } else if (seg == 0 && addr >= 0x80000000ull) {
                for (p = 0; p < total_pages; p++) {
                    MM_COPY_ADDRESS src_addr;
                    SIZE_T chunk, copied = 0;
                    chunk = ((p + 1) * 4096u <= total_bytes) ? 4096u : (total_bytes - p * 4096u);
                    src_addr.PhysicalAddress.QuadPart = (LONGLONG)(addr + (uint64_t)p * 4096ull);
                    if (NT_SUCCESS(MmCopyMemory((uint8_t *)disp->fb_va + p * 4096u,
                                                src_addr, chunk,
                                                MM_COPY_MEMORY_PHYSICAL, &copied))) {
                        copied_total += copied;
                    }
                }
            }

            if (copied_total > 0) {
                LONG n = InterlockedIncrement(&disp->scanout_blits);
                if (n <= 4) {
                    DLog("Scanout blit #%ld -> fb 0x%llx (seg=%u addr=0x%llx bytes=%llu)",
                         n, (uint64_t)disp->fb_pa.QuadPart, seg, addr, (uint64_t)copied_total);
                }
            }
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- 60 Hz VSync Timer DPC --------------------------------------------- */

static VOID AdrenoDispVsyncDpc(PKDPC dpc, PVOID ctx, PVOID arg1, PVOID arg2)
{
    PADRENO_WDDM_DISPLAY disp = (PADRENO_WDDM_DISPLAY)ctx;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data;

    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(arg1);
    UNREFERENCED_PARAMETER(arg2);

    if (!disp || !disp->enabled ||
        InterlockedCompareExchange(&disp->vsync_active, 0, 0) == 0)
        return;

    InterlockedIncrement(&disp->vsync_count);

    if (InterlockedExchange(&disp->scanout_dirty, 0) != 0 && disp->scanout_thread)
        KeSetEvent(&disp->scanout_event, IO_NO_INCREMENT, FALSE);

    RtlZeroMemory(&data, sizeof(data));
    data.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    data.CrtcVsync.VidPnTargetId = ADRENO_DISP_CHILD_UID;
    data.CrtcVsync.PhysicalAddress =
        disp->current_primary_pa.QuadPart ? disp->current_primary_pa : disp->fb_pa;
    data.CrtcVsync.PhysicalAdapterMask = 1;

    disp->dxgk.DxgkCbNotifyInterrupt(disp->device_handle, &data);
    disp->dxgk.DxgkCbNotifyDpc(disp->device_handle);
}

/* ---- Start / Stop ------------------------------------------------------ */

NTSTATUS AdrenoDispStart(PADRENO_WDDM_DISPLAY disp, HANDLE device_handle,
                         const DXGKRNL_INTERFACE *dxgk)
{
    DXGK_DISPLAY_INFORMATION post;
    NTSTATUS st;

    if (!disp || !dxgk)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(disp, sizeof(*disp));
    disp->device_handle = device_handle;
    RtlCopyMemory(&disp->dxgk, dxgk, sizeof(disp->dxgk));

    disp->width  = ADRENO_DISP_DEFAULT_WIDTH;
    disp->height = ADRENO_DISP_DEFAULT_HEIGHT;
    disp->pitch  = disp->width * 4u;
    disp->format = D3DDDIFMT_A8R8G8B8;
    disp->fb_pa.QuadPart = ADRENO_DISP_DEFAULT_FB_PA;

    RtlZeroMemory(&post, sizeof(post));
    if (dxgk->DxgkCbAcquirePostDisplayOwnership) {
        st = dxgk->DxgkCbAcquirePostDisplayOwnership(device_handle, &post);
        DLog("AcquirePostDisplayOwnership -> 0x%08x (%ux%u pitch=%u fmt=%u pa=0x%llx)",
             st, post.Width, post.Height, post.Pitch, (UINT)post.ColorFormat,
             (uint64_t)post.PhysicAddress.QuadPart);
        if (NT_SUCCESS(st) && post.Width > 0 && post.Height > 0 &&
            post.PhysicAddress.QuadPart != 0) {
            disp->width  = post.Width;
            disp->height = post.Height;
            disp->pitch  = post.Pitch ? post.Pitch : (post.Width * 4u);
            disp->format = post.ColorFormat ? post.ColorFormat : D3DDDIFMT_A8R8G8B8;
            disp->fb_pa  = post.PhysicAddress;
        }
    }

    disp->fb_size = (SIZE_T)disp->pitch * (SIZE_T)disp->height;
    disp->fb_va = MmMapIoSpaceEx(disp->fb_pa, disp->fb_size,
                                 PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!disp->fb_va) {
        DLog("WARNING: MmMapIoSpaceEx(fb=0x%llx, %llu bytes) failed; scanout blit disabled",
             (uint64_t)disp->fb_pa.QuadPart, (uint64_t)disp->fb_size);
    }

    KeInitializeEvent(&disp->scanout_event, SynchronizationEvent, FALSE);
    disp->scanout_stop = 0;
    disp->scanout_dirty = 0;
    disp->scanout_blits = 0;
    {
        HANDLE hThread = NULL;
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                                  AdrenoDispScanoutThread, disp);
        if (NT_SUCCESS(st) && hThread) {
            (void)ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS,
                                            *PsThreadType, KernelMode,
                                            (PVOID *)&disp->scanout_thread, NULL);
            ZwClose(hThread);
        }
    }

    KeInitializeTimerEx(&disp->vsync_timer, NotificationTimer);
    KeInitializeDpc(&disp->vsync_dpc, AdrenoDispVsyncDpc, disp);
    disp->enabled = TRUE;
    disp->source_visible = TRUE;

    DLog("Full WDDM Display initialized: %ux%u@60Hz pitch=%u fb_pa=0x%llx va=%p scanout_thr=%p",
         disp->width, disp->height, disp->pitch,
         (uint64_t)disp->fb_pa.QuadPart, disp->fb_va, disp->scanout_thread);
    return STATUS_SUCCESS;
}

VOID AdrenoDispSetAperture(PADRENO_WDDM_DISPLAY disp,
                           const uint64_t *aperture_pa,
                           uint32_t aperture_pages)
{
    if (!disp)
        return;
    disp->aperture_pa = aperture_pa;
    disp->aperture_pages = aperture_pages;
}

VOID AdrenoDispStop(PADRENO_WDDM_DISPLAY disp)
{
    if (!disp || !disp->enabled)
        return;

    InterlockedExchange(&disp->vsync_active, 0);
    KeCancelTimer(&disp->vsync_timer);
    KeFlushQueuedDpcs();

    if (disp->scanout_thread) {
        InterlockedExchange(&disp->scanout_stop, 1);
        KeSetEvent(&disp->scanout_event, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(disp->scanout_thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(disp->scanout_thread);
        disp->scanout_thread = NULL;
    }

    if (disp->fb_va) {
        MmUnmapIoSpace(disp->fb_va, disp->fb_size);
        disp->fb_va = NULL;
    }
    disp->enabled = FALSE;
    DLog("Display stopped (vsyncs=%ld blits=%ld)", disp->vsync_count, disp->scanout_blits);
}

/* ---- Child Relations / Status / EDID ----------------------------------- */

NTSTATUS AdrenoDispQueryChildRelations(PADRENO_WDDM_DISPLAY disp,
                                       PDXGK_CHILD_DESCRIPTOR relations,
                                       ULONG size)
{
    if (!disp || !disp->enabled)
        return STATUS_SUCCESS;
    if (!relations || size < sizeof(DXGK_CHILD_DESCRIPTOR) * 2)
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(relations, size);
    relations[0].ChildDeviceType = TypeVideoOutput;
    relations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
        D3DKMDT_VOT_OTHER;
    relations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness =
        D3DKMDT_MOA_NONE;
    relations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    relations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    relations[0].AcpiUid = 0;
    relations[0].ChildUid = ADRENO_DISP_CHILD_UID;
    DLog("QueryChildRelations(size=%u, desc_sz=%u) -> 1 output (VOT_OTHER, uid=0x%x)",
         size, (UINT)sizeof(DXGK_CHILD_DESCRIPTOR), ADRENO_DISP_CHILD_UID);
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispQueryChildStatus(PADRENO_WDDM_DISPLAY disp,
                                    PDXGK_CHILD_STATUS child_status,
                                    BOOLEAN non_destructive)
{
    UNREFERENCED_PARAMETER(non_destructive);
    if (!disp || !disp->enabled || !child_status)
        return STATUS_NOT_SUPPORTED;
    DLog("QueryChildStatus(uid=0x%x type=%u)", child_status->ChildUid, (UINT)child_status->Type);
    if (child_status->ChildUid != ADRENO_DISP_CHILD_UID)
        return STATUS_INVALID_PARAMETER;

    switch (child_status->Type) {
    case StatusConnection:
        child_status->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        child_status->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS AdrenoDispQueryDeviceDescriptor(PADRENO_WDDM_DISPLAY disp,
                                         ULONG child_uid,
                                         PDXGK_DEVICE_DESCRIPTOR desc)
{
    uint8_t edid[128];

    if (!disp || !disp->enabled || !desc)
        return STATUS_NOT_SUPPORTED;
    DLog("QueryDeviceDescriptor(uid=0x%x off=%u len=%u)", child_uid, desc->DescriptorOffset, desc->DescriptorLength);
    if (child_uid != ADRENO_DISP_CHILD_UID)
        return STATUS_INVALID_PARAMETER;
    if (desc->DescriptorOffset >= sizeof(edid) || !desc->DescriptorBuffer)
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;

    BuildS22Edid(edid, disp->width, disp->height);
    {
        ULONG avail = sizeof(edid) - desc->DescriptorOffset;
        ULONG copy = (desc->DescriptorLength < avail) ? desc->DescriptorLength : avail;
        RtlCopyMemory(desc->DescriptorBuffer, edid + desc->DescriptorOffset, copy);
        desc->DescriptorLength = copy;
    }
    return STATUS_SUCCESS;
}

/* ---- VidPN Helpers ----------------------------------------------------- */

static void FillVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *sig,
                                UINT width, UINT height)
{
    UINT htotal = width + 120u;
    UINT vtotal = height + 40u;

    RtlZeroMemory(sig, sizeof(*sig));
    sig->VideoStandard = D3DKMDT_VSS_OTHER;
    sig->TotalSize.cx  = htotal;
    sig->TotalSize.cy  = vtotal;
    sig->ActiveSize.cx = width;
    sig->ActiveSize.cy = height;
    sig->VSyncFreq.Numerator   = ADRENO_DISP_DEFAULT_REFRESH;
    sig->VSyncFreq.Denominator = 1;
    sig->HSyncFreq.Numerator   = ADRENO_DISP_DEFAULT_REFRESH * vtotal;
    sig->HSyncFreq.Denominator = 1;
    sig->PixelRate = (SIZE_T)htotal * (SIZE_T)vtotal * ADRENO_DISP_DEFAULT_REFRESH;
    sig->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS AddAndPinSourceMode(PADRENO_WDDM_DISPLAY disp,
                                    const DXGK_VIDPN_INTERFACE *vpi,
                                    D3DKMDT_HVIDPN hVidPn,
                                    D3DDDI_VIDEO_PRESENT_SOURCE_ID src_id,
                                    BOOLEAN do_pin)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE *mode = NULL;
    NTSTATUS st;

    st = vpi->pfnCreateNewSourceModeSet(hVidPn, src_id, &hSet, &setIf);
    if (!NT_SUCCESS(st))
        return st;

    st = setIf->pfnCreateNewModeInfo(hSet, &mode);
    if (!NT_SUCCESS(st)) {
        vpi->pfnReleaseSourceModeSet(hVidPn, hSet);
        return st;
    }

    mode->Type = D3DKMDT_RMT_GRAPHICS;
    mode->Format.Graphics.PrimSurfSize.cx = disp->width;
    mode->Format.Graphics.PrimSurfSize.cy = disp->height;
    mode->Format.Graphics.VisibleRegionSize.cx = disp->width;
    mode->Format.Graphics.VisibleRegionSize.cy = disp->height;
    mode->Format.Graphics.Stride = disp->pitch;
    mode->Format.Graphics.PixelFormat = disp->format;
    mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

    {
        D3DKMDT_VIDEO_PRESENT_SOURCE_MODE_ID id = mode->Id;
        st = setIf->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(st)) {
            setIf->pfnReleaseModeInfo(hSet, mode);
            vpi->pfnReleaseSourceModeSet(hVidPn, hSet);
            return st;
        }
        if (do_pin) {
            st = setIf->pfnPinMode(hSet, id);
            if (!NT_SUCCESS(st)) {
                vpi->pfnReleaseSourceModeSet(hVidPn, hSet);
                return st;
            }
        }
    }

    st = vpi->pfnAssignSourceModeSet(hVidPn, src_id, hSet);
    if (!NT_SUCCESS(st))
        vpi->pfnReleaseSourceModeSet(hVidPn, hSet);
    return st;
}

static NTSTATUS AddAndPinTargetMode(PADRENO_WDDM_DISPLAY disp,
                                    const DXGK_VIDPN_INTERFACE *vpi,
                                    D3DKMDT_HVIDPN hVidPn,
                                    D3DDDI_VIDEO_PRESENT_TARGET_ID tgt_id,
                                    BOOLEAN do_pin)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *setIf = NULL;
    D3DKMDT_VIDPN_TARGET_MODE *mode = NULL;
    NTSTATUS st;

    st = vpi->pfnCreateNewTargetModeSet(hVidPn, tgt_id, &hSet, &setIf);
    if (!NT_SUCCESS(st))
        return st;

    st = setIf->pfnCreateNewModeInfo(hSet, &mode);
    if (!NT_SUCCESS(st)) {
        vpi->pfnReleaseTargetModeSet(hVidPn, hSet);
        return st;
    }

    FillVideoSignalInfo(&mode->VideoSignalInfo, disp->width, disp->height);
    mode->Preference = D3DKMDT_MP_PREFERRED;

    {
        D3DKMDT_VIDEO_PRESENT_TARGET_MODE_ID id = mode->Id;
        st = setIf->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(st)) {
            setIf->pfnReleaseModeInfo(hSet, mode);
            vpi->pfnReleaseTargetModeSet(hVidPn, hSet);
            return st;
        }
        if (do_pin) {
            st = setIf->pfnPinMode(hSet, id);
            if (!NT_SUCCESS(st)) {
                vpi->pfnReleaseTargetModeSet(hVidPn, hSet);
                return st;
            }
        }
    }

    st = vpi->pfnAssignTargetModeSet(hVidPn, tgt_id, hSet);
    if (!NT_SUCCESS(st))
        vpi->pfnReleaseTargetModeSet(hVidPn, hSet);
    return st;
}

/* ---- VidPN DDIs -------------------------------------------------------- */

NTSTATUS AdrenoDispIsSupportedVidPn(PADRENO_WDDM_DISPLAY disp,
                                    DXGKARG_ISSUPPORTEDVIDPN *arg)
{
    if (!disp || !disp->enabled || !arg)
        return STATUS_INVALID_PARAMETER;

    arg->IsVidPnSupported = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispRecommendFunctionalVidPn(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *arg)
{
    const DXGK_VIDPN_INTERFACE *vpi = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopo = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf = NULL;
    D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    NTSTATUS st;

    if (!disp || !disp->enabled || !arg)
        return STATUS_INVALID_PARAMETER;

    st = disp->dxgk.DxgkCbQueryVidPnInterface(
        arg->hRecommendedFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vpi);
    if (!NT_SUCCESS(st))
        return st;

    st = vpi->pfnGetTopology(arg->hRecommendedFunctionalVidPn, &hTopo, &topoIf);
    if (!NT_SUCCESS(st))
        return st;

    st = topoIf->pfnCreateNewPathInfo(hTopo, &path);
    if (!NT_SUCCESS(st))
        return st;

    path->VidPnSourceId = 0;
    path->VidPnTargetId = ADRENO_DISP_CHILD_UID;
    path->ImportanceOrdinal = D3DKMDT_VPPI_PRIMARY;
    path->ContentTransformation.Scaling = D3DKMDT_VPPS_IDENTITY;
    path->ContentTransformation.ScalingSupport.Identity = 1;
    path->ContentTransformation.Rotation = D3DKMDT_VPPR_IDENTITY;
    path->ContentTransformation.RotationSupport.Identity = 1;
    path->VisibleFromActiveTLOffset.cx = 0;
    path->VisibleFromActiveTLOffset.cy = 0;
    path->VisibleFromActiveBROffset.cx = 0;
    path->VisibleFromActiveBROffset.cy = 0;
    path->VidPnTargetColorBasis = D3DKMDT_CB_SRGB;
    path->VidPnTargetColorCoeffDynamicRanges.FirstChannel = 8;
    path->VidPnTargetColorCoeffDynamicRanges.SecondChannel = 8;
    path->VidPnTargetColorCoeffDynamicRanges.ThirdChannel = 8;
    path->VidPnTargetColorCoeffDynamicRanges.FourthChannel = 0;
    path->Content = D3DKMDT_VPPC_GRAPHICS;
    path->CopyProtection.CopyProtectionType = D3DKMDT_VPPMT_NOPROTECTION;
    path->GammaRamp.Type = D3DDDI_GAMMARAMP_DEFAULT;

    st = topoIf->pfnAddPath(hTopo, path);
    if (!NT_SUCCESS(st)) {
        topoIf->pfnReleasePathInfo(hTopo, path);
        return st;
    }

    st = AddAndPinSourceMode(disp, vpi, arg->hRecommendedFunctionalVidPn, 0, TRUE);
    if (!NT_SUCCESS(st))
        return st;
    st = AddAndPinTargetMode(disp, vpi, arg->hRecommendedFunctionalVidPn,
                             ADRENO_DISP_CHILD_UID, TRUE);
    DLog("RecommendFunctionalVidPn -> 0x%08x (%ux%u@60Hz)", st, disp->width, disp->height);
    return st;
}

NTSTATUS AdrenoDispEnumVidPnCofuncModality(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *arg)
{
    const DXGK_VIDPN_INTERFACE *vpi = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopo = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    NTSTATUS st;

    if (!disp || !disp->enabled || !arg)
        return STATUS_INVALID_PARAMETER;

    st = disp->dxgk.DxgkCbQueryVidPnInterface(
        arg->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vpi);
    if (!NT_SUCCESS(st))
        return st;

    st = vpi->pfnGetTopology(arg->hConstrainingVidPn, &hTopo, &topoIf);
    if (!NT_SUCCESS(st))
        return st;

    st = topoIf->pfnAcquireFirstPathInfo(hTopo, &path);
    while (NT_SUCCESS(st) && path) {
        const D3DKMDT_VIDPN_PRESENT_PATH *next = NULL;

        /* Update source mode set if not pivot */
        if (arg->EnumPivotType != D3DKMDT_EPT_VIDPNSOURCE ||
            arg->EnumPivot.VidPnSourceId != path->VidPnSourceId) {
            D3DKMDT_HVIDPNSOURCEMODESET hSrc = NULL;
            const DXGK_VIDPNSOURCEMODESET_INTERFACE *srcIf = NULL;
            const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
            if (NT_SUCCESS(vpi->pfnAcquireSourceModeSet(
                    arg->hConstrainingVidPn, path->VidPnSourceId, &hSrc, &srcIf))) {
                NTSTATUS pst = srcIf->pfnAcquirePinnedModeInfo(hSrc, &pinned);
                if (NT_SUCCESS(pst) && pinned) {
                    srcIf->pfnReleaseModeInfo(hSrc, pinned);
                } else {
                    vpi->pfnReleaseSourceModeSet(arg->hConstrainingVidPn, hSrc);
                    hSrc = NULL;
                    (void)AddAndPinSourceMode(disp, vpi, arg->hConstrainingVidPn,
                                              path->VidPnSourceId, FALSE);
                }
                if (hSrc)
                    vpi->pfnReleaseSourceModeSet(arg->hConstrainingVidPn, hSrc);
            }
        }

        /* Update target mode set if not pivot */
        if (arg->EnumPivotType != D3DKMDT_EPT_VIDPNTARGET ||
            arg->EnumPivot.VidPnTargetId != path->VidPnTargetId) {
            D3DKMDT_HVIDPNTARGETMODESET hTgt = NULL;
            const DXGK_VIDPNTARGETMODESET_INTERFACE *tgtIf = NULL;
            const D3DKMDT_VIDPN_TARGET_MODE *pinned = NULL;
            if (NT_SUCCESS(vpi->pfnAcquireTargetModeSet(
                    arg->hConstrainingVidPn, path->VidPnTargetId, &hTgt, &tgtIf))) {
                NTSTATUS pst = tgtIf->pfnAcquirePinnedModeInfo(hTgt, &pinned);
                if (NT_SUCCESS(pst) && pinned) {
                    tgtIf->pfnReleaseModeInfo(hTgt, pinned);
                } else {
                    vpi->pfnReleaseTargetModeSet(arg->hConstrainingVidPn, hTgt);
                    hTgt = NULL;
                    (void)AddAndPinTargetMode(disp, vpi, arg->hConstrainingVidPn,
                                              path->VidPnTargetId, FALSE);
                }
                if (hTgt)
                    vpi->pfnReleaseTargetModeSet(arg->hConstrainingVidPn, hTgt);
            }
        }

        /* Advertise identity scaling & rotation support */
        if (arg->EnumPivotType != D3DKMDT_EPT_SCALING &&
            arg->EnumPivotType != D3DKMDT_EPT_ROTATION) {
            D3DKMDT_VIDPN_PRESENT_PATH upd = *path;
            upd.ContentTransformation.ScalingSupport.Identity = 1;
            upd.ContentTransformation.RotationSupport.Identity = 1;
            (void)topoIf->pfnUpdatePathSupportInfo(hTopo, &upd);
        }

        st = topoIf->pfnAcquireNextPathInfo(hTopo, path, &next);
        topoIf->pfnReleasePathInfo(hTopo, path);
        path = next;
    }

    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispRecommendMonitorModes(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_RECOMMENDMONITORMODES *arg)
{
    D3DKMDT_MONITOR_SOURCE_MODE *mode = NULL;
    NTSTATUS st;

    if (!disp || !disp->enabled || !arg || !arg->pMonitorSourceModeSetInterface)
        return STATUS_INVALID_PARAMETER;

    st = arg->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(
        arg->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(st))
        return st;

    FillVideoSignalInfo(&mode->VideoSignalInfo, disp->width, disp->height);
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges.FirstChannel = 8;
    mode->ColorCoeffDynamicRanges.SecondChannel = 8;
    mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    mode->ColorCoeffDynamicRanges.FourthChannel = 0;
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;

    st = arg->pMonitorSourceModeSetInterface->pfnAddMode(
        arg->hMonitorSourceModeSet, mode);
    if (!NT_SUCCESS(st))
        arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(
            arg->hMonitorSourceModeSet, mode);
    return st;
}

NTSTATUS AdrenoDispRecommendVidPnTopology(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_RECOMMENDVIDPNTOPOLOGY *arg)
{
    const DXGK_VIDPN_INTERFACE *vpi = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopo = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf = NULL;
    D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    NTSTATUS st;

    if (!disp || !disp->enabled || !arg)
        return STATUS_INVALID_PARAMETER;

    st = disp->dxgk.DxgkCbQueryVidPnInterface(
        arg->hVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vpi);
    if (!NT_SUCCESS(st))
        return st;
    st = vpi->pfnGetTopology(arg->hVidPn, &hTopo, &topoIf);
    if (!NT_SUCCESS(st))
        return st;
    st = topoIf->pfnCreateNewPathInfo(hTopo, &path);
    if (!NT_SUCCESS(st))
        return st;

    path->VidPnSourceId = 0;
    path->VidPnTargetId = ADRENO_DISP_CHILD_UID;
    path->ImportanceOrdinal = D3DKMDT_VPPI_PRIMARY;
    path->ContentTransformation.Scaling = D3DKMDT_VPPS_IDENTITY;
    path->ContentTransformation.Rotation = D3DKMDT_VPPR_IDENTITY;
    path->Content = D3DKMDT_VPPC_GRAPHICS;
    path->CopyProtection.CopyProtectionType = D3DKMDT_VPPMT_NOPROTECTION;
    path->GammaRamp.Type = D3DDDI_GAMMARAMP_DEFAULT;

    st = topoIf->pfnAddPath(hTopo, path);
    if (!NT_SUCCESS(st))
        topoIf->pfnReleasePathInfo(hTopo, path);
    return st;
}

NTSTATUS AdrenoDispSetVidPnSourceVisibility(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_SETVIDPNSOURCEVISIBILITY *arg)
{
    if (!disp || !arg)
        return STATUS_INVALID_PARAMETER;
    disp->source_visible = arg->Visible;
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispCommitVidPn(PADRENO_WDDM_DISPLAY disp,
                               const DXGKARG_COMMITVIDPN *arg)
{
    if (!disp || !disp->enabled || !arg)
        return STATUS_INVALID_PARAMETER;
    disp->vidpn_committed = TRUE;
    disp->current_primary_alloc = arg->hPrimaryAllocation;
    DLog("CommitVidPn(src=%u primary=%p) -> OK",
         arg->AffectedVidPnSourceId, arg->hPrimaryAllocation);
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispUpdateActiveVidPnPresentPath(
    PADRENO_WDDM_DISPLAY disp,
    const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *arg)
{
    UNREFERENCED_PARAMETER(disp);
    UNREFERENCED_PARAMETER(arg);
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispSetVidPnSourceAddress(
    PADRENO_WDDM_DISPLAY disp, const DXGKARG_SETVIDPNSOURCEADDRESS *arg)
{
    if (!disp || !disp->enabled || !arg)
        return STATUS_INVALID_PARAMETER;

    /* IRQL-safe (may execute at DIRQL): record primary surface and let
     * 60 Hz VSync DPC wake our PASSIVE_LEVEL scanout blit thread. */
    disp->current_primary_seg   = arg->PrimarySegment;
    disp->current_primary_pa    = arg->PrimaryAddress;
    disp->current_primary_alloc = arg->hAllocation;
    InterlockedExchange(&disp->scanout_dirty, 1);
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispGetScanLine(PADRENO_WDDM_DISPLAY disp,
                               DXGKARG_GETSCANLINE *arg)
{
    LARGE_INTEGER freq, now;
    ULONGLONG ticks_per_frame, pos;

    if (!disp || !disp->enabled || !arg)
        return STATUS_INVALID_PARAMETER;

    now = KeQueryPerformanceCounter(&freq);
    ticks_per_frame = (freq.QuadPart > 0) ? (ULONGLONG)(freq.QuadPart / ADRENO_DISP_DEFAULT_REFRESH) : 16666ull;
    if (ticks_per_frame == 0)
        ticks_per_frame = 1;
    pos = (ULONGLONG)now.QuadPart % ticks_per_frame;
    arg->ScanLine = (UINT)((pos * (disp->height + 40u)) / ticks_per_frame);
    arg->InVerticalBlank = (arg->ScanLine >= disp->height) ? TRUE : FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispControlVsync(PADRENO_WDDM_DISPLAY disp, BOOLEAN enable)
{
    if (!disp || !disp->enabled)
        return STATUS_SUCCESS;

    if (enable) {
        if (InterlockedExchange(&disp->vsync_active, 1) == 0) {
            LARGE_INTEGER due;
            due.QuadPart = -166667ll; /* ~16.67 ms (60 Hz) */
            KeSetTimerEx(&disp->vsync_timer, due, 16, &disp->vsync_dpc);
            disp->vsync_enabled = TRUE;
            DLog("VSync 60Hz ENABLED");
        }
    } else {
        if (InterlockedExchange(&disp->vsync_active, 0) != 0) {
            KeCancelTimer(&disp->vsync_timer);
            disp->vsync_enabled = FALSE;
            DLog("VSync 60Hz DISABLED");
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispQueryVidPnHWCapability(PADRENO_WDDM_DISPLAY disp,
                                          DXGKARG_QUERYVIDPNHWCAPABILITY *arg)
{
    if (!disp || !disp->enabled || !arg)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&arg->VidPnHWCaps, sizeof(arg->VidPnHWCaps));
    arg->VidPnHWCaps.DriverRotation = 0;
    arg->VidPnHWCaps.DriverScaling = 0;
    arg->VidPnHWCaps.DriverCloning = 0;
    arg->VidPnHWCaps.DriverColorConvert = 0;
    arg->VidPnHWCaps.DriverLinkedAdapaterOutput = 0;
    arg->VidPnHWCaps.DriverRemoteDisplay = 0;
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispStopAndReleasePostDisplay(PADRENO_WDDM_DISPLAY disp,
                                             D3DDDI_VIDEO_PRESENT_TARGET_ID target_id,
                                             PDXGK_DISPLAY_INFORMATION info)
{
    UNREFERENCED_PARAMETER(target_id);
    if (info && disp) {
        RtlZeroMemory(info, sizeof(*info));
        info->Width = disp->width ? disp->width : ADRENO_DISP_DEFAULT_WIDTH;
        info->Height = disp->height ? disp->height : ADRENO_DISP_DEFAULT_HEIGHT;
        info->Pitch = disp->pitch ? disp->pitch : (info->Width * 4u);
        info->ColorFormat = disp->format ? disp->format : D3DDDIFMT_A8R8G8B8;
        if (disp->fb_pa.QuadPart)
            info->PhysicAddress = disp->fb_pa;
        else
            info->PhysicAddress.QuadPart = ADRENO_DISP_DEFAULT_FB_PA;
        info->TargetId = ADRENO_DISP_CHILD_UID;
        info->AcpiId = 0;
    }
    if (disp)
        AdrenoDispStop(disp);
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoDispSystemDisplayEnable(PADRENO_WDDM_DISPLAY disp,
                                       D3DDDI_VIDEO_PRESENT_TARGET_ID target_id,
                                       PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS flags,
                                       UINT *width, UINT *height,
                                       D3DDDIFORMAT *format)
{
    UNREFERENCED_PARAMETER(target_id);
    UNREFERENCED_PARAMETER(flags);
    if (!disp || !width || !height || !format)
        return STATUS_INVALID_PARAMETER;
    *width = disp->width ? disp->width : ADRENO_DISP_DEFAULT_WIDTH;
    *height = disp->height ? disp->height : ADRENO_DISP_DEFAULT_HEIGHT;
    *format = disp->format ? disp->format : D3DDDIFMT_A8R8G8B8;
    return STATUS_SUCCESS;
}

VOID AdrenoDispSystemDisplayWrite(PADRENO_WDDM_DISPLAY disp,
                                  PVOID source, UINT src_w, UINT src_h,
                                  UINT src_stride, UINT pos_x, UINT pos_y)
{
    UINT y;
    if (!disp || !disp->fb_va || !source || src_w == 0 || src_h == 0)
        return;
    if (pos_x >= disp->width || pos_y >= disp->height)
        return;
    {
        UINT copy_w = (pos_x + src_w > disp->width) ? (disp->width - pos_x) : src_w;
        UINT copy_h = (pos_y + src_h > disp->height) ? (disp->height - pos_y) : src_h;
        UINT row_bytes = copy_w * 4u;
        const uint8_t *src_ptr = (const uint8_t *)source;
        uint8_t *dst_ptr = (uint8_t *)disp->fb_va + (SIZE_T)pos_y * disp->pitch + (SIZE_T)pos_x * 4u;
        for (y = 0; y < copy_h; y++) {
            RtlCopyMemory(dst_ptr, src_ptr, row_bytes);
            src_ptr += src_stride;
            dst_ptr += disp->pitch;
        }
    }
}

NTSTATUS AdrenoDispGetChildContainerId(PADRENO_WDDM_DISPLAY disp,
                                       ULONG child_uid,
                                       PDXGK_CHILD_CONTAINER_ID cid)
{
    if (!disp || !disp->enabled || !cid)
        return STATUS_NOT_SUPPORTED;
    if (child_uid != ADRENO_DISP_CHILD_UID)
        return STATUS_INVALID_PARAMETER;
    cid->ContainerId.Data1 = 0x52305100u;
    cid->ContainerId.Data2 = 0x5332u;
    cid->ContainerId.Data3 = 0x324Fu;
    cid->ContainerId.Data4[0] = 'L'; cid->ContainerId.Data4[1] = 'E';
    cid->ContainerId.Data4[2] = 'D'; cid->ContainerId.Data4[3] = '0';
    cid->ContainerId.Data4[4] = 'S'; cid->ContainerId.Data4[5] = '2';
    cid->ContainerId.Data4[6] = '2'; cid->ContainerId.Data4[7] = '0';
    RtlZeroMemory(&cid->EldInfo, sizeof(cid->EldInfo));
    return STATUS_SUCCESS;
}
