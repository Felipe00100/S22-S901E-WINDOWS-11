#ifndef __EFII2C_H__
#define __EFII2C_H__

#define EFI_QCOM_I2C_PROTOCOL_GUID \
  { 0xb27ae8b1, 0x3e10, 0x4d07, { 0xab, 0x5c, 0xeb, 0x9a, 0x6d, 0xc6, 0xfa, 0x8f } }

extern EFI_GUID gQcomI2CProtocolGuid;

typedef enum {
  I2C_INSTANCE_001 = 1,
  I2C_INSTANCE_002,
  I2C_INSTANCE_003,
  I2C_INSTANCE_004,
  I2C_INSTANCE_005,
  I2C_INSTANCE_006,
  I2C_INSTANCE_007,
  I2C_INSTANCE_008,
  I2C_INSTANCE_009,
  I2C_INSTANCE_010,
  I2C_INSTANCE_011,
  I2C_INSTANCE_012,
  I2C_INSTANCE_013,
  I2C_INSTANCE_014,
  I2C_INSTANCE_015,
  I2C_INSTANCE_016,
  I2C_INSTANCE_017,
  I2C_INSTANCE_018,
  I2C_INSTANCE_019,
  I2C_INSTANCE_020,
  I2C_INSTANCE_021,
  I2C_INSTANCE_022,
  I2C_INSTANCE_023,
  I2C_INSTANCE_024,
  I2C_INSTANCE_MAX
} i2c_instance;

typedef enum {
  I2C_SUCCESS = 0,
  I2C_ERROR_INVALID_PARAMETER,
  I2C_ERROR_UNSUPPORTED_CORE_INSTANCE,
  I2C_ERROR_API_INVALID_EXECUTION_LEVEL,
  I2C_ERROR_API_NOT_SUPPORTED,
  I2C_ERROR_API_ASYNC_MODE_NOT_SUPPORTED,
  I2C_ERROR_API_PROTOCOL_MODE_NOT_SUPPORTED,
  I2C_ERROR_HANDLE_ALLOCATION,
  I2C_ERROR_HW_INFO_ALLOCATION,
  I2C_ERROR_IO_CTXT_ALLOCATION,
  I2C_ERROR_INPUT_FIFO_OVER_RUN,
  I2C_ERROR_OUTPUT_FIFO_UNDER_RUN,
  I2C_ERROR_INPUT_FIFO_UNDER_RUN,
  I2C_ERROR_OUTPUT_FIFO_OVER_RUN,
  I2C_ERROR_INVALID_CMD_OPCODE,
  I2C_ERROR_COMMAND_OVER_RUN,
  I2C_ERROR_COMMAND_ILLEGAL,
  I2C_ERROR_COMMAND_FAIL,
  I2C_ERROR_INVALID_CMD_SEQ,
  I2C_ERROR_BUS_NOT_IDLE,
  I2C_ERROR_START_STOP_UNEXPECTED,
  I2C_ERROR_DATA_NACK,
  I2C_ERROR_ADDR_NACK,
  I2C_ERROR_ARBITRATION_LOST,
  I2C_ERROR_TRANSFER_TIMEOUT
} i2c_status;

typedef enum {
  I2C = 0,
  SMBUS,
  I3C_SDR,
  I3C_HDR_DDR,
  I3C_BROADCAST_CCC,
  I3C_DIRECT_CCC,
  I3C_IBI_READ
} i2c_mode;

typedef struct {
  UINT32    bus_frequency_khz;
  UINT32    slave_address;
  i2c_mode  mode;
  UINT32    slave_max_clock_stretch_us;
  UINT32    core_configuration1;
  UINT32    core_configuration2;
} i2c_slave_config;

typedef struct {
  UINT8   *buffer;
  UINT32  length;
  UINT32  flags;
} i2c_descriptor;

typedef i2c_status (EFIAPI *EFI_QCOM_I2C_OPEN) (
  IN  i2c_instance  instance,
  OUT VOID          **i2c_handle
  );

typedef i2c_status (EFIAPI *EFI_QCOM_I2C_READ) (
  IN  VOID              *i2c_handle,
  IN  i2c_slave_config  *config,
  IN  UINT16            offset,
  IN  UINT16            offset_len,
  OUT UINT8             *buffer,
  IN  UINT16            buffer_len,
  OUT UINT32            *read,
  IN  UINT32            timeout_ms
  );

typedef i2c_status (EFIAPI *EFI_QCOM_I2C_WRITE) (
  IN  VOID              *i2c_handle,
  IN  i2c_slave_config  *config,
  IN  UINT16            offset,
  IN  UINT16            offset_len,
  IN  UINT8             *buffer,
  IN  UINT16            buffer_len,
  OUT UINT32            *written,
  IN  UINT32            timeout_ms
  );

typedef i2c_status (EFIAPI *EFI_QCOM_I2C_TRANSFER) (
  IN  VOID              *i2c_handle,
  IN  i2c_slave_config  *config,
  IN  i2c_descriptor    *desc,
  IN  UINT16            num_descriptors,
  IN  VOID              *func,
  IN  VOID              *ctxt,
  IN  UINT32            delay_us,
  OUT UINT32            *transferred
  );

typedef i2c_status (EFIAPI *EFI_QCOM_I2C_CLOSE) (
  IN  VOID  *i2c_handle
  );

typedef struct _EFI_QCOM_I2C_PROTOCOL {
  UINT64                 revision;
  EFI_QCOM_I2C_OPEN      open;
  EFI_QCOM_I2C_READ      read;
  EFI_QCOM_I2C_WRITE     write;
  EFI_QCOM_I2C_TRANSFER  transfer;
  EFI_QCOM_I2C_CLOSE     close;
} EFI_QCOM_I2C_PROTOCOL;

#endif /* __EFII2C_H__ */
