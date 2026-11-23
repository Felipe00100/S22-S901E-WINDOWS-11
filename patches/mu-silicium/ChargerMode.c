#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/EFIChargerEx.h>
#include <Protocol/EFII2C.h>

//
// Galaxy S22 (r0q) MAX77705 Charger & Fuel Gauge on SM8450 QUP0 SE5 (i2c5)
// Direct SM8450 TLMM GPIO pages (mapped in r0q MemoryMapLib 0x0F000000..0x0FFFFFFF):
//   GPIO 206 = I2C5 SDA (0x0F1CE000), GPIO 207 = I2C5 SCL (0x0F1CF000)
//
#define TLMM_GPIO206_BASE             0x0F1CE000ULL
#define TLMM_GPIO207_BASE             0x0F1CF000ULL
#define TLMM_GPIO_CFG_OFF             0x0000ULL
#define TLMM_GPIO_IN_OUT_OFF          0x0004ULL
#define TLMM_CFG_LOW_MASK             0x000003FFU  // PULL[1:0] | FUNC[5:2] | DRV[8:6] | OE[9]
#define TLMM_CFG_PULL_NONE            0x00000000U
#define TLMM_CFG_PULL_UP              0x00000003U
#define TLMM_CFG_OE_BIT               0x00000200U
#define TLMM_IO_IN_BIT                0x00000001U

#define MAX77705_I2C_FREQ_KHZ         400
#define MAX77705_FG_ADDR              0x36
#define MAX77705_CHG_ADDR             0x69

// MAX17042 / MAX77705 Fuel Gauge registers (16-bit LE)
#define MAX17042_REG_REPSOC           0x06
#define MAX17042_REG_TEMP             0x08
#define MAX17042_REG_VCELL            0x09
#define MAX17042_REG_CURRENT          0x0A

// MAX77705 Charger registers (8-bit, base 0xB0)
#define MAX77705_CHG_REG_INT_OK       0xB2
#define MAX77705_CHG_REG_DETAILS_01   0xB4
#define MAX77705_CHG_REG_CNFG_00      0xB7
#define MAX77705_CHG_REG_CNFG_02      0xB9
#define MAX77705_CHG_REG_CNFG_06      0xBD
#define MAX77705_CHG_REG_CNFG_09      0xC0
#define MAX77705_CHGIN_OK_BIT         (1U << 6)
#define MAX77705_WCIN_OK_BIT          (1U << 5)

// Timing constants
#define CHARGER_TICK_100NS            1000000ULL  // 100 ms
#define REFRESH_TICKS                 10          // 1.0 s
#define EXIT_COMBO_HOLD_TICKS         30          // 3.0 s holding Vol+ & Vol-
#define UNPLUG_CONFIRM_SECONDS        2

typedef struct {
  BOOLEAN  ValidI2c;
  BOOLEAN  ValidChargerEx;
  BOOLEAN  UsbPresent;
  BOOLEAN  ChgRegOk;
  UINT32   SocPercent;
  UINT32   VoltageMv;
  INT32    CurrentMa;
  INT32    PowerMw;      // Signed milliwatts (+ charging, - discharging)
  INT32    TempC;
  UINT8    ChgDetails;
  UINT8    ChgIntOk;
} CHARGER_TELEMETRY;

typedef struct {
  EFI_QCOM_I2C_PROTOCOL  *I2c;
  VOID                   *Handle;
  UINT32                 FoundInstance;
  UINT32                 OpenMask;
  BOOLEAN                HaveI2cProto;
  BOOLEAN                UseTlmm;
  UINT64                 SdaBase;
  UINT64                 SclBase;
  UINT32                 SdaUpperCfg;
  UINT32                 SclUpperCfg;
  UINT32                 PullBits;
  UINT32                 InitG206Cfg;
  UINT32                 InitG206Io;
  UINT32                 InitG207Cfg;
  UINT32                 InitG207Io;
} MAX77705_BUS;

STATIC
VOID
TlmmSetLine (
  IN UINT64   Base,
  IN UINT32   UpperCfg,
  IN UINT32   PullBits,
  IN BOOLEAN  ReleaseHigh
  )
{
  if (ReleaseHigh) {
    MmioWrite32 (Base + TLMM_GPIO_CFG_OFF, UpperCfg | PullBits);
  } else {
    MmioWrite32 (Base + TLMM_GPIO_IN_OUT_OFF, 0);
    MmioWrite32 (Base + TLMM_GPIO_CFG_OFF, UpperCfg | TLMM_CFG_OE_BIT);
  }
}

STATIC
BOOLEAN
TlmmReadLine (
  IN UINT64  Base
  )
{
  return (MmioRead32 (Base + TLMM_GPIO_IN_OUT_OFF) & TLMM_IO_IN_BIT) != 0;
}

STATIC
BOOLEAN
TlmmWaitSclHigh (
  IN MAX77705_BUS  *Bus
  )
{
  UINTN  Timeout;

  TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, TRUE);
  for (Timeout = 0; Timeout < 200; Timeout++) {
    if (TlmmReadLine (Bus->SclBase)) {
      return TRUE;
    }
    gBS->Stall (5);
  }
  return FALSE;
}

STATIC
VOID
TlmmI2cBusRecover (
  IN MAX77705_BUS  *Bus
  )
{
  UINTN  i;

  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, TRUE);
  for (i = 0; i < 9; i++) {
    TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, TRUE);
    gBS->Stall (8);
    if (TlmmReadLine (Bus->SdaBase)) {
      break;
    }
    TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, FALSE);
    gBS->Stall (8);
  }
  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, FALSE);
  gBS->Stall (8);
  TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, TRUE);
  gBS->Stall (8);
  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, TRUE);
  gBS->Stall (8);
}

STATIC
VOID
TlmmI2cStart (
  IN MAX77705_BUS  *Bus
  )
{
  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, TRUE);
  TlmmWaitSclHigh (Bus);
  gBS->Stall (6);
  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, FALSE);
  gBS->Stall (6);
  TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, FALSE);
  gBS->Stall (6);
}

STATIC
VOID
TlmmI2cStop (
  IN MAX77705_BUS  *Bus
  )
{
  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, FALSE);
  gBS->Stall (6);
  TlmmWaitSclHigh (Bus);
  gBS->Stall (6);
  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, TRUE);
  gBS->Stall (6);
}

STATIC
BOOLEAN
TlmmI2cWriteByte (
  IN MAX77705_BUS  *Bus,
  IN UINT8         Byte
  )
{
  INTN     Bit;
  BOOLEAN  Ack;

  for (Bit = 7; Bit >= 0; Bit--) {
    TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, ((Byte >> Bit) & 1U) != 0);
    gBS->Stall (6);
    if (!TlmmWaitSclHigh (Bus)) {
      return FALSE;
    }
    gBS->Stall (6);
    TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, FALSE);
    gBS->Stall (6);
  }

  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, TRUE);
  gBS->Stall (6);
  if (!TlmmWaitSclHigh (Bus)) {
    return FALSE;
  }
  Ack = !TlmmReadLine (Bus->SdaBase);
  gBS->Stall (6);
  TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, FALSE);
  gBS->Stall (6);
  return Ack;
}

STATIC
UINT8
TlmmI2cReadByte (
  IN MAX77705_BUS  *Bus,
  IN BOOLEAN       SendAck
  )
{
  INTN   Bit;
  UINT8  Byte = 0;

  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, TRUE);
  for (Bit = 7; Bit >= 0; Bit--) {
    gBS->Stall (6);
    if (!TlmmWaitSclHigh (Bus)) {
      break;
    }
    if (TlmmReadLine (Bus->SdaBase)) {
      Byte |= (UINT8)(1U << Bit);
    }
    gBS->Stall (6);
    TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, FALSE);
  }

  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, !SendAck);
  gBS->Stall (6);
  TlmmWaitSclHigh (Bus);
  gBS->Stall (6);
  TlmmSetLine (Bus->SclBase, Bus->SclUpperCfg, Bus->PullBits, FALSE);
  TlmmSetLine (Bus->SdaBase, Bus->SdaUpperCfg, Bus->PullBits, TRUE);
  gBS->Stall (6);
  return Byte;
}

STATIC
i2c_status
TlmmI2cReadRegs (
  IN  MAX77705_BUS  *Bus,
  IN  UINT8         SlaveAddr7,
  IN  UINT8         Reg,
  OUT UINT8         *Buf,
  IN  UINT16        Len
  )
{
  UINT16      i;
  i2c_status  St = I2C_SUCCESS;

  TlmmI2cStart (Bus);
  if (!TlmmI2cWriteByte (Bus, (UINT8)(SlaveAddr7 << 1)) || !TlmmI2cWriteByte (Bus, Reg)) {
    St = I2C_ERROR_ADDR_NACK;
    goto Done;
  }
  TlmmI2cStart (Bus);
  if (!TlmmI2cWriteByte (Bus, (UINT8)((SlaveAddr7 << 1) | 1U))) {
    St = I2C_ERROR_ADDR_NACK;
    goto Done;
  }
  for (i = 0; i < Len; i++) {
    Buf[i] = TlmmI2cReadByte (Bus, (i + 1) < Len);
  }

Done:
  TlmmI2cStop (Bus);
  return St;
}

STATIC
i2c_status
TlmmI2cWriteReg (
  IN  MAX77705_BUS  *Bus,
  IN  UINT8         SlaveAddr7,
  IN  UINT8         Reg,
  IN  UINT8         Val
  )
{
  i2c_status  St = I2C_SUCCESS;

  TlmmI2cStart (Bus);
  if (!TlmmI2cWriteByte (Bus, (UINT8)(SlaveAddr7 << 1)) ||
      !TlmmI2cWriteByte (Bus, Reg) ||
      !TlmmI2cWriteByte (Bus, Val))
  {
    St = I2C_ERROR_ADDR_NACK;
  }
  TlmmI2cStop (Bus);
  return St;
}

STATIC
BOOLEAN
IsPlausibleVcell (
  IN CONST UINT8  *Buf2
  )
{
  UINT16  Raw16;
  UINT32  Mv;

  Raw16 = (UINT16)(Buf2[0] | ((UINT16)Buf2[1] << 8));
  Mv    = (UINT32)(((UINT64)(Raw16 >> 3) * 625ULL) / 1000ULL);
  return (Mv >= 2000 && Mv <= 4800);
}

STATIC
VOID
ProbeMax77705Bus (
  OUT MAX77705_BUS  *Bus
  )
{
  EFI_STATUS        Status;
  UINT32            Inst;
  VOID              *Handle;
  i2c_slave_config  Cfg;
  UINT8             Buf2[2];
  UINT32            ReadBytes;
  UINTN             Try;

  ZeroMem (Bus, sizeof (*Bus));
  Bus->InitG206Cfg = MmioRead32 (TLMM_GPIO206_BASE + TLMM_GPIO_CFG_OFF);
  Bus->InitG206Io  = MmioRead32 (TLMM_GPIO206_BASE + TLMM_GPIO_IN_OUT_OFF);
  Bus->InitG207Cfg = MmioRead32 (TLMM_GPIO207_BASE + TLMM_GPIO_CFG_OFF);
  Bus->InitG207Io  = MmioRead32 (TLMM_GPIO207_BASE + TLMM_GPIO_IN_OUT_OFF);

  //
  // 1. Scan all Qualcomm I2CDxe instances (1..24) using Samsung ChgDxe's exact config
  //    (On SM8450 QUP0 SE5 / Linux i2c5 is typically I2C_INSTANCE_006.)
  //
  Status = gBS->LocateProtocol (&gQcomI2CProtocolGuid, NULL, (VOID **)&Bus->I2c);
  if (!EFI_ERROR (Status) && (Bus->I2c != NULL) && (Bus->I2c->open != NULL) && (Bus->I2c->read != NULL)) {
    Bus->HaveI2cProto = TRUE;

    ZeroMem (&Cfg, sizeof (Cfg));
    Cfg.bus_frequency_khz          = MAX77705_I2C_FREQ_KHZ;
    Cfg.slave_address              = MAX77705_FG_ADDR;
    Cfg.mode                       = I2C;
    Cfg.slave_max_clock_stretch_us = 500;

    for (Inst = I2C_INSTANCE_001; Inst <= I2C_INSTANCE_024; Inst++) {
      Handle = NULL;
      if (Bus->I2c->open ((i2c_instance)Inst, &Handle) != I2C_SUCCESS || Handle == NULL) {
        continue;
      }

      if (Inst < 32) {
        Bus->OpenMask |= (1U << Inst);
      }

      ZeroMem (Buf2, sizeof (Buf2));
      ReadBytes = 0;
      if (Bus->I2c->read (Handle, &Cfg, MAX17042_REG_VCELL, 1, Buf2, 2, &ReadBytes, 500) == I2C_SUCCESS &&
          IsPlausibleVcell (Buf2))
      {
        Bus->Handle        = Handle;
        Bus->FoundInstance = Inst;
        return;
      }

      if (Bus->I2c->close != NULL) {
        Bus->I2c->close (Handle);
      }
    }
  }

  //
  // 2. Fallback: Direct SM8450 TLMM I2C5 open-drain bit-bang (preserving upper CFG/EGPIO bits)
  //
  for (Try = 0; Try < 4; Try++) {
    BOOLEAN  SwapPins = ((Try & 2) != 0);
    UINT32   Pull     = ((Try & 1) != 0) ? TLMM_CFG_PULL_UP : TLMM_CFG_PULL_NONE;

    Bus->SdaBase     = SwapPins ? TLMM_GPIO207_BASE : TLMM_GPIO206_BASE;
    Bus->SclBase     = SwapPins ? TLMM_GPIO206_BASE : TLMM_GPIO207_BASE;
    Bus->SdaUpperCfg = (SwapPins ? Bus->InitG207Cfg : Bus->InitG206Cfg) & ~TLMM_CFG_LOW_MASK;
    Bus->SclUpperCfg = (SwapPins ? Bus->InitG206Cfg : Bus->InitG207Cfg) & ~TLMM_CFG_LOW_MASK;
    Bus->PullBits    = Pull;

    TlmmI2cBusRecover (Bus);
    ZeroMem (Buf2, sizeof (Buf2));
    if (TlmmI2cReadRegs (Bus, MAX77705_FG_ADDR, MAX17042_REG_VCELL, Buf2, 2) == I2C_SUCCESS &&
        IsPlausibleVcell (Buf2))
    {
      Bus->UseTlmm = TRUE;
      return;
    }
  }

  // Restore original TLMM mux config if TLMM fallback failed
  MmioWrite32 (TLMM_GPIO206_BASE + TLMM_GPIO_CFG_OFF, Bus->InitG206Cfg);
  MmioWrite32 (TLMM_GPIO207_BASE + TLMM_GPIO_CFG_OFF, Bus->InitG207Cfg);
}

STATIC
i2c_status
Max77705Read (
  IN  MAX77705_BUS  *Bus,
  IN  UINT32        SlaveAddr,
  IN  UINT8         Reg,
  OUT UINT8         *Buf,
  IN  UINT16        Len
  )
{
  i2c_slave_config  Cfg;
  UINT32            ReadBytes;

  if ((Bus->I2c != NULL) && (Bus->Handle != NULL)) {
    ZeroMem (&Cfg, sizeof (Cfg));
    Cfg.bus_frequency_khz          = MAX77705_I2C_FREQ_KHZ;
    Cfg.slave_address              = SlaveAddr;
    Cfg.mode                       = I2C;
    Cfg.slave_max_clock_stretch_us = 500;

    ReadBytes = 0;
    return Bus->I2c->read (Bus->Handle, &Cfg, Reg, 1, Buf, Len, &ReadBytes, 500);
  }

  if (Bus->UseTlmm) {
    return TlmmI2cReadRegs (Bus, (UINT8)SlaveAddr, Reg, Buf, Len);
  }

  return I2C_ERROR_UNSUPPORTED_CORE_INSTANCE;
}

STATIC
i2c_status
Max77705WriteReg (
  IN  MAX77705_BUS  *Bus,
  IN  UINT32        SlaveAddr,
  IN  UINT8         Reg,
  IN  UINT8         Val
  )
{
  i2c_slave_config  Cfg;
  UINT32            Written;
  UINT8             Data = Val;

  if ((Bus->I2c != NULL) && (Bus->Handle != NULL) && (Bus->I2c->write != NULL)) {
    ZeroMem (&Cfg, sizeof (Cfg));
    Cfg.bus_frequency_khz          = MAX77705_I2C_FREQ_KHZ;
    Cfg.slave_address              = SlaveAddr;
    Cfg.mode                       = I2C;
    Cfg.slave_max_clock_stretch_us = 500;

    Written = 0;
    return Bus->I2c->write (Bus->Handle, &Cfg, Reg, 1, &Data, 1, &Written, 500);
  }

  if (Bus->UseTlmm) {
    return TlmmI2cWriteReg (Bus, (UINT8)SlaveAddr, Reg, Val);
  }

  return I2C_ERROR_UNSUPPORTED_CORE_INSTANCE;
}

STATIC
VOID
ConfigureMax77705FastCharge (
  IN  MAX77705_BUS             *Bus,
  IN  CONST CHARGER_TELEMETRY  *Tel
  )
{
  UINT8  RegVal;
  UINT8  CcCode;
  UINT8  IlimCode;

  if (((Bus->Handle == NULL) && !Bus->UseTlmm) ||
      !Tel->ChgRegOk ||
      ((Tel->ChgIntOk & MAX77705_CHGIN_OK_BIT) == 0))
  {
    return;
  }

  // Thermal guard: 2500 mA below 43C, fall back to 900 mA if battery gets warm
  // CHG_CC step = 50 mA (0x32 = 2500 mA, 0x12 = 900 mA)
  // CHGIN_LIM = (code + 1) * 25 mA (0x63 = 2500 mA, 0x23 = 900 mA); hardware AICL stays enabled
  if (Tel->TempC >= 43) {
    CcCode   = 0x12U;
    IlimCode = 0x23U;
  } else {
    CcCode   = 0x32U;
    IlimCode = 0x63U;
  }

  // 1. Unlock charger writes (CHGPROT=11, WDTCLR=01, DIS_AICL=0)
  RegVal = 0x0DU;
  if (Max77705Read (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_06, &RegVal, 1) == I2C_SUCCESS) {
    RegVal = (UINT8)((RegVal & ~0x1FU) | 0x0DU);
  }
  (VOID)Max77705WriteReg (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_06, RegVal);

  // 2. Keep Charger+Buck ON (MODE=0x05) and disable 80s watchdog (WDTEN=0)
  if (Max77705Read (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_00, &RegVal, 1) == I2C_SUCCESS) {
    UINT8 Desired = (UINT8)((RegVal & ~0x1FU) | 0x05U);
    if (RegVal != Desired) {
      (VOID)Max77705WriteReg (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_00, Desired);
    }
  }

  // 3. Program Fast Charge CC current (CHG_CNFG_02 bits[5:0], preserving OTG_ILIM bits[7:6])
  if (Max77705Read (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_02, &RegVal, 1) == I2C_SUCCESS) {
    UINT8 Desired = (UINT8)((RegVal & 0xC0U) | (CcCode & 0x3FU));
    if (RegVal != Desired) {
      (VOID)Max77705WriteReg (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_02, Desired);
    }
  }

  // 4. Program CHGIN input current limit with CHG_EN=1 (CHG_CNFG_09)
  if (Max77705Read (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_09, &RegVal, 1) == I2C_SUCCESS) {
    UINT8 Desired = (UINT8)(0x80U | (IlimCode & 0x7FU));
    if (RegVal != Desired) {
      (VOID)Max77705WriteReg (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_CNFG_09, Desired);
    }
  }
}

STATIC
CONST CHAR16 *
GetChgStatusText (
  IN BOOLEAN  UsbPresent,
  IN UINT8    ChgDetails
  )
{
  if (!UsbPresent) {
    return L"USB Disconnected (Discharging)";
  }

  switch (ChgDetails & 0x0F) {
    case 0x00: return L"Pre-qualification";
    case 0x01: return L"Fast Charging (CC)";
    case 0x02: return L"Fast Charging (CV)";
    case 0x03: return L"Top-Off Charging";
    case 0x04: return L"Battery Full (Done)";
    case 0x06: return L"Timer Fault";
    case 0x07: return L"Suspended (Temp/JEITA)";
    case 0x08: return L"Charger Off";
    case 0x0A: return L"Thermal Shutdown";
    case 0x0B: return L"Watchdog Expired";
    default:   return L"Charging Active";
  }
}

STATIC
VOID
ReadChargerTelemetry (
  IN  MAX77705_BUS             *Bus,
  IN  EFI_CHARGER_EX_PROTOCOL  *ChargerEx,
  OUT CHARGER_TELEMETRY        *Tel
  )
{
  UINT8       Buf2[2];
  UINT8       Val8;
  UINT16      Raw16;
  INT16       RawS16;
  EFI_STATUS  Status;

  ZeroMem (Tel, sizeof (*Tel));

  if ((Bus->Handle != NULL) || Bus->UseTlmm) {
    if (Max77705Read (Bus, MAX77705_FG_ADDR, MAX17042_REG_REPSOC, Buf2, 2) == I2C_SUCCESS) {
      Tel->ValidI2c   = TRUE;
      Tel->SocPercent = Buf2[1];
      if (Tel->SocPercent > 100) {
        Tel->SocPercent = 100;
      }
    }

    if (Max77705Read (Bus, MAX77705_FG_ADDR, MAX17042_REG_VCELL, Buf2, 2) == I2C_SUCCESS) {
      Raw16          = (UINT16)(Buf2[0] | ((UINT16)Buf2[1] << 8));
      Tel->VoltageMv = (UINT32)(((UINT64)(Raw16 >> 3) * 625ULL) / 1000ULL);
      Tel->ValidI2c  = TRUE;
    }

    if (Max77705Read (Bus, MAX77705_FG_ADDR, MAX17042_REG_CURRENT, Buf2, 2) == I2C_SUCCESS) {
      RawS16 = (INT16)(Buf2[0] | ((UINT16)Buf2[1] << 8));
      // 1.5625 uV / Rsns (2 mOhm on S22 r0q -> 0.78125 mA/LSB)
      Tel->CurrentMa = (INT32)(((INT64)RawS16 * 78125LL) / 100000LL);
    }

    if (Max77705Read (Bus, MAX77705_FG_ADDR, MAX17042_REG_TEMP, Buf2, 2) == I2C_SUCCESS) {
      Tel->TempC = (INT8)Buf2[1];
    }

    if (Max77705Read (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_INT_OK, &Val8, 1) == I2C_SUCCESS) {
      Tel->ChgRegOk   = TRUE;
      Tel->ChgIntOk   = Val8;
      Tel->UsbPresent = ((Val8 & (MAX77705_CHGIN_OK_BIT | MAX77705_WCIN_OK_BIT)) != 0);
    } else if (Tel->ValidI2c) {
      // Fallback if 0x69 charger sub-address is NACKed while 0x36 fuel gauge works
      Tel->UsbPresent = (Tel->CurrentMa > 20);
    }

    if (Max77705Read (Bus, MAX77705_CHG_ADDR, MAX77705_CHG_REG_DETAILS_01, &Val8, 1) == I2C_SUCCESS) {
      Tel->ChgDetails = Val8;
    }

    Tel->PowerMw = (INT32)(((INT64)Tel->VoltageMv * (INT64)Tel->CurrentMa) / 1000LL);
  }

  if (ChargerEx != NULL) {
    BOOLEAN  Present = FALSE;
    UINT32   VoltMv  = 0;

    Status = ChargerEx->GetChargerPresence (&Present);
    if (!EFI_ERROR (Status)) {
      Tel->ValidChargerEx = TRUE;
      if (!Tel->ChgRegOk) {
        Tel->UsbPresent = Present;
      }
    }

    Status = ChargerEx->GetBatteryVoltage (&VoltMv);
    if (!EFI_ERROR (Status) && (Tel->VoltageMv == 0)) {
      Tel->VoltageMv = VoltMv;
    }
  }
}

STATIC
VOID
DrawChargerScreen (
  IN CONST CHARGER_TELEMETRY  *Tel,
  IN CONST MAX77705_BUS       *Bus,
  IN BOOLEAN                  DimMode,
  IN UINTN                    ComboTicks,
  IN BOOLEAN                  WasConnected
  )
{
  CHAR16  Bar[25];
  UINTN   Filled;
  UINTN   i;
  UINT32  VoltWhole  = Tel->VoltageMv / 1000;
  UINT32  VoltFrac   = Tel->VoltageMv % 1000;
  UINT32  AbsPowerMw = (Tel->PowerMw < 0) ? (UINT32)(-Tel->PowerMw) : (UINT32)Tel->PowerMw;
  UINT32  WattWhole  = AbsPowerMw / 1000;
  UINT32  WattFrac   = (AbsPowerMw % 1000) / 10;
  CHAR16  PowerSign  = (Tel->PowerMw < 0) ? L'-' : L'+';

  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  if (DimMode) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_DARKGRAY | EFI_BACKGROUND_BLACK);
    Print (L"\n\n\n\n  [OLED Saver] ");
    if (Tel->ValidI2c) {
      Print (L"%u%% | %u.%03u V | %c%u.%02u W | %d mA | %s\n",
        Tel->SocPercent,
        VoltWhole, VoltFrac,
        PowerSign, WattWhole, WattFrac,
        Tel->CurrentMa,
        Tel->UsbPresent ? L"USB ON" : L"USB OFF"
        );
    } else if (Tel->ValidChargerEx) {
      Print (L"%u.%03u V (%u mV) | %s\n",
        VoltWhole, VoltFrac, Tel->VoltageMv,
        Tel->UsbPresent ? L"USB ON" : L"USB OFF");
    } else {
      Print (L"Charging active...\n");
    }

    Print (L"  Vol+ to wake | Vol+ & Vol- (3s) or unplug USB to exit\n");
    if (ComboTicks > 0) {
      Print (L"  Exiting: %u%%\n", (UINT32)((ComboTicks * 100) / EXIT_COMBO_HOLD_TICKS));
    }

    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTCYAN | EFI_BACKGROUND_BLACK);
  Print (L"\n");
  Print (L"  ================================================\n");
  Print (L"         Galaxy S22 (r0q) - UEFI Charger Mode     \n");
  Print (L"  ================================================\n\n");

  if (Tel->ValidI2c) {
    Filled = (Tel->SocPercent * 20) / 100;
    if (Filled > 20) {
      Filled = 20;
    }

    Bar[0] = L'[';
    for (i = 0; i < 20; i++) {
      Bar[1 + i] = (i < Filled) ? L'#' : L'-';
    }
    Bar[21] = L']';
    Bar[22] = L'\0';

    if (Tel->SocPercent >= 80) {
      gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGREEN | EFI_BACKGROUND_BLACK);
    } else if (Tel->SocPercent >= 25) {
      gST->ConOut->SetAttribute (gST->ConOut, EFI_YELLOW | EFI_BACKGROUND_BLACK);
    } else {
      gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTRED | EFI_BACKGROUND_BLACK);
    }

    Print (L"  Battery  : %s  %u%%\n\n", Bar, Tel->SocPercent);

    gST->ConOut->SetAttribute (gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLACK);
    Print (L"  Voltage  : %u.%03u V (%u mV)\n", VoltWhole, VoltFrac, Tel->VoltageMv);
    Print (L"  Current  : %d mA\n", Tel->CurrentMa);
    Print (L"  Power    : %c%u.%02u W (%d mW)\n", PowerSign, WattWhole, WattFrac, Tel->PowerMw);
    Print (L"  Temp     : %d C\n", Tel->TempC);
    Print (L"  USB/AC   : %s\n", Tel->UsbPresent ? L"Connected" : L"Disconnected");
    Print (L"  Status   : %s\n", GetChgStatusText (Tel->UsbPresent, Tel->ChgDetails));
    if (Bus->FoundInstance != 0) {
      Print (L"  Bus      : MAX77705 via I2CDxe #%u\n\n", Bus->FoundInstance);
    } else {
      Print (L"  Bus      : MAX77705 via TLMM I2C5\n\n");
    }
  } else if (Tel->ValidChargerEx) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLACK);
    Print (L"  Voltage  : %u.%03u V (%u mV)\n", VoltWhole, VoltFrac, Tel->VoltageMv);
    Print (L"  USB/AC   : %s\n", Tel->UsbPresent ? L"Connected" : L"Disconnected");
    Print (L"  Status   : %s\n\n", Tel->UsbPresent ? L"Charging (ChargerEx)" : L"Discharging");
  } else {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_YELLOW | EFI_BACKGROUND_BLACK);
    Print (L"  Hardware charging initialized by Samsung ChgDxe.\n");
    Print (L"  (Live telemetry unavailable; device is safe to charge)\n");
    gST->ConOut->SetAttribute (gST->ConOut, EFI_DARKGRAY | EFI_BACKGROUND_BLACK);
    Print (L"  Diag: I2C=%c Open=0x%06x G206=%04x/%x G207=%04x/%x\n\n",
      Bus->HaveI2cProto ? L'Y' : L'N',
      Bus->OpenMask,
      Bus->InitG206Cfg & 0xFFFF, Bus->InitG206Io & 0xF,
      Bus->InitG207Cfg & 0xFFFF, Bus->InitG207Io & 0xF);
  }

  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  Print (L"  ------------------------------------------------\n");
  Print (L"  Controls (No Power Button Needed):\n");
  Print (L"   * Hold Vol+ & Vol- together (3s) : Exit to Menu\n");
  if (WasConnected) {
    Print (L"   * Unplug USB cable               : Auto-Exit\n");
  } else {
    Print (L"   * Plug then unplug USB cable     : Auto-Exit\n");
  }
  Print (L"   * Press Vol+ once                : Toggle OLED Saver\n");
  Print (L"  ------------------------------------------------\n");

  if (ComboTicks > 0) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTRED | EFI_BACKGROUND_BLACK);
    Print (L"\n  >>> Hold Vol+ & Vol- to exit: %u%% <<<\n",
      (UINT32)((ComboTicks * 100) / EXIT_COMBO_HOLD_TICKS));
  }
}

EFI_STATUS
EFIAPI
ChargerModeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS               Status;
  MAX77705_BUS             Bus;
  EFI_CHARGER_EX_PROTOCOL  *ChargerEx     = NULL;
  EFI_EVENT                TimerEvent     = NULL;
  EFI_EVENT                WaitList[2];
  UINTN                    Index;
  EFI_INPUT_KEY            Key;
  CHARGER_TELEMETRY        Tel;
  BOOLEAN                  DimMode        = FALSE;
  BOOLEAN                  WasConnected   = FALSE;
  UINTN                    UnplugSeconds  = 0;
  UINTN                    RefreshCounter = REFRESH_TICKS;
  UINTN                    TotalTicks     = 0;
  UINTN                    LastUpTick     = 0;
  UINTN                    LastDownTick   = 0;
  BOOLEAN                  SeenUp         = FALSE;
  BOOLEAN                  SeenDown       = FALSE;
  UINTN                    ComboHoldTicks = 0;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  ProbeMax77705Bus (&Bus);
  gBS->LocateProtocol (&gChargerExProtocolGuid, NULL, (VOID **)&ChargerEx);

  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
  if (EFI_ERROR (Status)) {
    goto Cleanup;
  }

  gBS->SetTimer (TimerEvent, TimerPeriodic, CHARGER_TICK_100NS);

  ReadChargerTelemetry (&Bus, ChargerEx, &Tel);
  if (Tel.UsbPresent) {
    WasConnected = TRUE;
    ConfigureMax77705FastCharge (&Bus, &Tel);
  }

  DrawChargerScreen (&Tel, &Bus, DimMode, 0, WasConnected);

  while (TRUE) {
    WaitList[0] = gST->ConIn->WaitForKey;
    WaitList[1] = TimerEvent;
    gBS->WaitForEvent (2, WaitList, &Index);

    if (Index == 0) {
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (!EFI_ERROR (Status)) {
        if (Key.ScanCode == SCAN_ESC) {
          break;
        } else if (Key.ScanCode == SCAN_UP) {
          SeenUp     = TRUE;
          LastUpTick = TotalTicks;
          if (!SeenDown || ((TotalTicks - LastDownTick) > 6)) {
            DimMode = !DimMode;
            DrawChargerScreen (&Tel, &Bus, DimMode, ComboHoldTicks, WasConnected);
          }
        } else if (Key.ScanCode == SCAN_DOWN) {
          SeenDown     = TRUE;
          LastDownTick = TotalTicks;
        }
      }
    } else {
      TotalTicks++;

      if (SeenUp && SeenDown &&
          ((TotalTicks - LastUpTick) <= 6) &&
          ((TotalTicks - LastDownTick) <= 6))
      {
        ComboHoldTicks++;
        if (ComboHoldTicks >= EXIT_COMBO_HOLD_TICKS) {
          break;
        }

        if ((ComboHoldTicks % 5) == 0) {
          DrawChargerScreen (&Tel, &Bus, DimMode, ComboHoldTicks, WasConnected);
        }
      } else if (ComboHoldTicks > 0) {
        ComboHoldTicks = 0;
        DrawChargerScreen (&Tel, &Bus, DimMode, 0, WasConnected);
      }

      RefreshCounter++;
      if (RefreshCounter >= REFRESH_TICKS) {
        RefreshCounter = 0;
        ReadChargerTelemetry (&Bus, ChargerEx, &Tel);

        if (Tel.UsbPresent) {
          WasConnected  = TRUE;
          UnplugSeconds = 0;
          ConfigureMax77705FastCharge (&Bus, &Tel);
        } else if (WasConnected && (Tel.ValidI2c || Tel.ValidChargerEx)) {
          UnplugSeconds++;
          if (UnplugSeconds >= UNPLUG_CONFIRM_SECONDS) {
            break;
          }
        }

        DrawChargerScreen (&Tel, &Bus, DimMode, ComboHoldTicks, WasConnected);
      }
    }
  }

Cleanup:
  if (TimerEvent != NULL) {
    gBS->SetTimer (TimerEvent, TimerCancel, 0);
    gBS->CloseEvent (TimerEvent);
  }

  if ((Bus.I2c != NULL) && (Bus.Handle != NULL) && (Bus.I2c->close != NULL)) {
    Bus.I2c->close (Bus.Handle);
  }

  if (Bus.UseTlmm) {
    MmioWrite32 (TLMM_GPIO206_BASE + TLMM_GPIO_CFG_OFF, Bus.InitG206Cfg);
    MmioWrite32 (TLMM_GPIO207_BASE + TLMM_GPIO_CFG_OFF, Bus.InitG207Cfg);
  }

  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  gST->ConOut->ClearScreen (gST->ConOut);
  return EFI_SUCCESS;
}
