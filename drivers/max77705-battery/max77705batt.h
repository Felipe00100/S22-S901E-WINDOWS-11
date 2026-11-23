#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>

#define MAX77705_POOL_TAG                   '77XM'

//
// Galaxy S22 (r0q) Battery design specifications (from sm8450-samsung-r0q.dts)
//   3700 mAh @ 3.85 V nominal = 14245 mWh, sense resistor = 2 mOhm (rsns = 2000 uOhm)
//
#define S22_DESIGN_CAPACITY_MWH             14245
#define S22_DESIGN_VOLTAGE_MV               3850
#define S22_CRITICAL_BIAS_MWH               0
#define S22_ALERT_LOW_MWH                   (S22_DESIGN_CAPACITY_MWH / 10)  // 10%
#define S22_ALERT_WARN_MWH                  (S22_DESIGN_CAPACITY_MWH / 20)  // 5%

//
// SM8450 TLMM I2C5 GPIO pages:
//   GPIO 206 = I2C5 SDA (offset 0x0000 in 0x0F1CE000)
//   GPIO 207 = I2C5 SCL (offset 0x1000 in 0x0F1CE000)
//
#define TLMM_GPIO_STRIDE                    0x1000
#define TLMM_GPIO_CFG_OFFSET                0x0000
#define TLMM_GPIO_IN_OUT_OFFSET             0x0004

#define TLMM_PIN_IDX_SDA                    0
#define TLMM_PIN_IDX_SCL                    1

#define TLMM_CFG_PULL_NONE                  (0U << 0)
#define TLMM_CFG_PULL_UP                    (3U << 0)
#define TLMM_CFG_FUNC_GPIO                  (0U << 2)
#define TLMM_CFG_DRV_2MA                    (0U << 6)
#define TLMM_CFG_OE                         (1U << 9)

#define TLMM_IO_IN_BIT                      (1U << 0)
#define TLMM_IO_OUT_BIT                     (1U << 1)

//
// MAX77705 I2C 7-bit addresses & registers (ported from s22-linux kernel patches)
//
#define MAX77705_FG_ADDR                    0x36
#define MAX77705_CHG_ADDR                   0x69

// MAX17042 / MAX77705 Fuel Gauge (16-bit LE, 2 mOhm sense resistor)
#define MAX17042_REG_STATUS                 0x00
#define MAX17042_REG_VALRT_TH               0x01
#define MAX17042_REG_REPCAP                 0x05
#define MAX17042_REG_REPSOC                 0x06
#define MAX17042_REG_TEMP                   0x08
#define MAX17042_REG_VCELL                  0x09
#define MAX17042_REG_CURRENT                0x0A
#define MAX17042_REG_AVGCURRENT             0x0B
#define MAX17042_REG_FULLCAP                0x10
#define MAX17042_REG_CYCLES                 0x17

// MAX77705 Charger (8-bit, base 0xB0)
#define MAX77705_CHG_REG_INT                0xB0
#define MAX77705_CHG_REG_INT_MASK           0xB1
#define MAX77705_CHG_REG_INT_OK             0xB2
#define MAX77705_CHG_REG_DETAILS_00         0xB3
#define MAX77705_CHG_REG_DETAILS_01         0xB4
#define MAX77705_CHG_REG_CNFG_00            0xB7
#define MAX77705_CHG_REG_CNFG_02            0xB9
#define MAX77705_CHG_REG_CNFG_09            0xC0

#define MAX77705_AICL_I_BIT                 (1U << 7)
#define MAX77705_CHGIN_OK_BIT               (1U << 6)
#define MAX77705_WCIN_OK_BIT                (1U << 5)

// CNFG_00 charger modes
#define MAX77705_MODE_MASK                  0x0F
#define MAX77705_MODE_BUCK_ONLY             0x04  // USB power-path active, battery charging paused
#define MAX77705_MODE_CHARGER_BUCK          0x05  // Normal charging + buck
#define MAX77705_MODE_OTG_BOOST             0x0A  // OTG 5V VBUS boost

// CNFG_02 OTG_ILIM (bits 7:6 -> 3 = 1500 mA)
#define MAX77705_OTG_ILIM_MASK              0xC0
#define MAX77705_OTG_ILIM_1500MA            0xC0

typedef struct _MAX77705_TELEMETRY {
    BOOLEAN UsbOnline;
    BOOLEAN Charging;
    BOOLEAN ChargeLimitHeld;
    ULONG   SocPercent;
    ULONG   VoltageMv;
    LONG    CurrentMa;
    LONG    AvgCurrentMa;
    LONG    PowerMw;       // Signed mW (+ charging, - discharging)
    ULONG   FullCapMwh;
    ULONG   RemCapMwh;
    ULONG   CycleCount;
    LONG    TempTenthsK;   // BatteryTemperature unit: 0.1 Kelvin
} MAX77705_TELEMETRY, *PMAX77705_TELEMETRY;

typedef struct _MAX77705_DEVICE_CONTEXT {
    WDFDEVICE           Device;
    PVOID               ClassHandle;
    WDFWAITLOCK         I2cLock;
    WDFTIMER            PollTimer;

    volatile UCHAR      *I2cGpioBase;     // 2 pages: GPIO 206 (SDA), GPIO 207 (SCL)
    SIZE_T              I2cGpioLen;

    BOOLEAN             VirtualMode;      // TRUE when loaded as ROOT\MAXM7705 (QEMU VM test)
    ULONG               BatteryTag;
    ULONG               ChargeControlEndThreshold; // 50..100% (default 100%)
    MAX77705_TELEMETRY  LastTelemetry;
    ULONG               LastPowerState;
} MAX77705_DEVICE_CONTEXT, *PMAX77705_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MAX77705_DEVICE_CONTEXT, Max77705GetContext)

//
// i2c_tlmm.c
//
VOID
TlmmI2cInitPins(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx
    );

NTSTATUS
TlmmI2cReadRegs(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx,
    _In_ UCHAR SlaveAddr7,
    _In_ UCHAR RegAddr,
    _Out_writes_bytes_(Len) UCHAR *OutBuf,
    _In_ ULONG Len
    );

NTSTATUS
TlmmI2cWriteRegs(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx,
    _In_ UCHAR SlaveAddr7,
    _In_ UCHAR RegAddr,
    _In_reads_bytes_(Len) const UCHAR *Data,
    _In_ ULONG Len
    );

VOID
Max77705InitHardwareQuirks(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx
    );

VOID
Max77705SyncChargeLimit(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx,
    _In_ ULONG SocPercent
    );

NTSTATUS
Max77705ReadTelemetry(
    _In_ PMAX77705_DEVICE_CONTEXT DevCtx,
    _Out_ PMAX77705_TELEMETRY Tel
    );
