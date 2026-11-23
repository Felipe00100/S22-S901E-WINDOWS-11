#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/EFIPmicPon.h>
#include <Protocol/EFIResetReason.h>

#pragma pack(1)
typedef struct {
  CHAR16  ReasonStr[12]; // L"RESET_PARAM\0" (12 CHAR16 = 24 bytes)
  UINT8   ReasonData;    // 0x01 = mode-recovery (per pmk8350.dtsi)
} QCOM_RESET_PARAM_DATA;
#pragma pack()

//
// Reboot directly into Android/TWRP Recovery on Galaxy S22 (SM8450 / r0q)
// WITHOUT modifying any UFS partitions:
//   1. Writes mode-recovery = 0x01 shifted to bits[7:1] (= 0x02, mask 0xFE per pmk8350.dtsi)
//      into PMK8350 PON SOFT_SPARE / SDAM2 reboot-reason.
//   2. Passes Qualcomm ResetRuntimeDxe L"RESET_PARAM\0" + 0x01 (EFI_NO_MAPPING) to EfiResetWarm.
//
EFI_STATUS
EFIAPI
RebootRecoveryEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                  Status;
  EFI_QCOM_PMIC_PON_PROTOCOL  *PmicPon = NULL;
  QCOM_RESET_PARAM_DATA       ResetData;

  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTCYAN | EFI_BACKGROUND_BLACK);
  Print (L"\n\n  Rebooting Galaxy S22 (r0q) into Recovery (TWRP)...\n");

  Status = gBS->LocateProtocol (&gQcomPmicPonProtocolGuid, NULL, (VOID **)&PmicPon);
  if (!EFI_ERROR (Status) && (PmicPon != NULL) && (PmicPon->SetSpareReg != NULL)) {
    // pmk8350.dtsi: reboot-reason@48 { bits = <1 7>; mode-recovery = <0x01>; } -> (0x01 << 1) = 0x02
    PmicPon->SetSpareReg (0, EFI_PM_PON_SOFT_SPARE, 0x02, 0xFE);
  }

  ZeroMem (&ResetData, sizeof (ResetData));
  CopyMem (ResetData.ReasonStr, STR_RESET_PARAM, sizeof (ResetData.ReasonStr));
  ResetData.ReasonData = 0x01; // mode-recovery

  gBS->Stall (250000); // 250 ms

  // Qualcomm r0q/ResetRuntimeDxe parses L"RESET_PARAM\0" + 1-byte ReasonData on EFI_NO_MAPPING
  gRT->ResetSystem (EfiResetWarm, EFI_NO_MAPPING, sizeof (ResetData), &ResetData);

  // Fallback if ResetRuntimeDxe returns
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, sizeof (ResetData), &ResetData);
  gRT->ResetSystem (EfiResetCold, EFI_NO_MAPPING, sizeof (ResetData), &ResetData);

  return EFI_SUCCESS;
}
