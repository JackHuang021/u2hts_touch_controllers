/*
  Copyright (C) CNflysky.
  U2HTS stands for "USB to HID TouchScreen".
  sec_ts.c: driver for Samsung SEC TouchScreen controllers.
  This file is licensed under GPL V3.
  All rights reserved.
*/

#include "u2hts_core.h"
#include "s6d6ft0_v1.10_20170918.h"

static bool sec_ts_setup(U2HTS_BUS_TYPES bus_type);
static bool sec_ts_coord_fetch();
static void sec_ts_get_config(u2hts_touch_controller_config* cfg);

// Configuration options
#ifndef SEC_TS_SKIP_FW_UPDATE
#define SEC_TS_SKIP_FW_UPDATE 0
#endif

#ifndef SEC_TS_SKIP_SELFTEST
#define SEC_TS_SKIP_SELFTEST 0
#endif

static u2hts_touch_controller_operations sec_ts_ops = {
    .setup = &sec_ts_setup,
    .fetch = &sec_ts_coord_fetch,
    .get_config = &sec_ts_get_config};

static u2hts_touch_controller sec_ts = {
    .name = "sec_ts",                           // controller name
    .irq_type = IRQ_TYPE_EDGE_FALLING,          // irq type
    .report_mode = UTC_REPORT_MODE_EVENT,        // report mode (event-driven)
    // I2C
    .i2c_config =
        {
            .primary_addr = 0x70,                // I2C slave addr
            .speed_hz = 400 * 1000,             // I2C speed in Hz
            .alt_addrs = (uint8_t[]){0x71, 0}   // Alternative I2C addrs
        },
    // SPI
    .spi_config =
        {
            .cpha = false,
            .cpol = false,
            .speed_hz = 1000 * 1000,
        },
    .operations = &sec_ts_ops
};

static U2HTS_BUS_TYPES sec_ts_bus_type = UB_I2C;

// Flag to track if firmware was updated
static bool sec_ts_fw_updated = false;

// register controller
// 注册控制器
U2HTS_TOUCH_CONTROLLER(sec_ts);

/* Samsung SEC TS Register Addresses */
#define SEC_TS_READ_DEVICE_ID         0x52
#define SEC_TS_READ_SUB_ID           0x53
#define SEC_TS_READ_ONE_EVENT        0x71
#define SEC_TS_READ_BOOT_STATUS      0x55
#define SEC_TS_READ_FW_VERSION      0xA3
#define SEC_TS_READ_SELFTEST_RESULT  0x80

#define SEC_TS_CMD_CLEAR_EVENT_STACK  0x60
#define SEC_TS_CMD_SW_RESET           0x42
#define SEC_TS_CMD_SENSE_ON           0x40
#define SEC_TS_CMD_SENSE_OFF          0x41
#define SEC_TS_CMD_CALIBRATION_OFFSET_SDC 0x4C
#define SEC_TS_CMD_CALIBRATION_OFFSET_SEC 0x4F
#define SEC_TS_CMD_SELFTEST           0x51
#define SEC_TS_CMD_SELFTEST_TYPE      0x5F
#define SEC_TS_CMD_SELFTEST_PTOP      0x82
#define SEC_TS_CMD_ENTER_FW_MODE      0x57
#define SEC_TS_CMD_WRITE_FW_BLK       0x53
#define SEC_TS_CMD_WRITE_FW_SHORT     0x54
#define SEC_TS_CMD_WRITE_FW_LONG      0x5A
#define SEC_TS_CMD_GET_CHECKSUM       0xA6

/* Flash/Storage commands */
#define SEC_TS_CMD_CS_CONTROL         0x8B
#define SEC_TS_CMD_SET_DATA_NUM       0xD1
#define SEC_TS_CMD_FLASH_SEND_DATA    0xEB
#define SEC_TS_CMD_FLASH_READ_DATA    0xEC

#define FLASH_CMD_RDSR               0x05
#define FLASH_CMD_WREN               0x06
#define FLASH_CMD_SE                 0x20
#define FLASH_CMD_PP                 0x02

/* Device IDs */
#define SEC_TS_ID_ON_FW              0xAC
#define SEC_TS_ID_ON_BOOT            0xD0

/* Event IDs */
#define SEC_TS_STATUS_EVENT          0
#define SEC_TS_COORDINATE_EVENT      1
#define SEC_TS_GESTURE_EVENT         2

/* Status Events */
#define SEC_TS_ACK_BOOT_COMPLETE     0x0C
#define SEC_TS_ACK_SELF_TEST_DONE   0x0A
#define TYPE_STATUS_EVENT_ACK        1

/* Coordinate Actions */
#define SEC_TS_COORD_ACTION_NONE     0
#define SEC_TS_COORD_ACTION_PRESS    1
#define SEC_TS_COORD_ACTION_MOVE     2
#define SEC_TS_COORD_ACTION_RELEASE  3

#define SEC_TS_EVENT_BUFF_SIZE       8
#define SEC_TS_MAX_TOUCH_COUNT       10
#define SEC_TS_FW_HEADER_SIGN        0x53494654  // "TFIS"
#define SEC_TS_FW_CHUNK_SIGN         0x53434654  // "TFCS"

#define SEC_TS_FLASH_SIZE_256        256
#define SEC_TS_WAIT_RETRY_CNT        100

// Firmware header structure
typedef struct __packed {
  uint32_t signature;      // "TFIS"
  uint32_t version;        // firmware version
  uint32_t flash_info0;   // max flash size
  uint32_t flash_info1;   // parameter area
  uint32_t flag;          // mode select/bootloader mode
  uint32_t setting;        // HWB settings
  uint32_t checksum;       // checksum
  uint32_t boot_start_addr[3];
  uint32_t flash_load_addr[3];
  uint32_t number_of_chunk[3];
} sec_ts_fw_header;

// Event buffer coordinate structure (matches Linux driver)
typedef struct __packed {
  uint8_t tchsta : 3;  // touch state
  uint8_t ttype : 3;   // touch type
  uint8_t eid : 2;     // event id

  uint8_t tid : 4;      // touch id
  uint8_t nt : 4;      // number of touch

  uint8_t x_11_4;      // X coordinate high bits
  uint8_t y_11_4;      // Y coordinate high bits

  uint8_t y_3_0 : 4;   // Y coordinate low bits
  uint8_t x_3_0 : 4;   // X coordinate low bits

  uint8_t z;            // pressure/touch width
  uint8_t major;        // touch major
  uint8_t minor;        // touch minor
} sec_ts_event_coord;

// Selftest result structure
typedef struct __packed {
  uint32_t signature;
  uint32_t version;
  uint32_t total_size;
  uint32_t crc32;
  uint32_t result;
  uint32_t try_cnt;
  uint32_t pass_cnt;
  uint32_t fail_cnt;
  uint32_t channel;
} sec_ts_selftest_header;

inline static void sec_ts_read_reg(uint8_t reg, void* data,
                                    size_t data_size) {
  u2hts_i2c_mem_read(sec_ts.i2c_config.primary_addr, reg, sizeof(reg),
                      data, data_size);
}

inline static void sec_ts_write_reg(uint8_t reg, void* data,
                                    size_t data_size) {
  u2hts_i2c_mem_write(sec_ts.i2c_config.primary_addr, reg,
                      sizeof(reg), data, data_size);
}

inline static void sec_ts_write_cmd(uint8_t reg) {
    u2hts_i2c_mem_write(sec_ts.i2c_config.primary_addr, reg, 0, NULL, 0);
}

inline static uint8_t sec_ts_read_byte(uint8_t reg) {
  uint8_t var = 0;
  sec_ts_read_reg(reg, &var, sizeof(var));
  return var;
}

// Delay function
inline static void sec_ts_delay_ms(uint32_t ms) { u2hts_delay_ms(ms); }

inline static bool sec_ts_wait_for_ready(uint8_t ack) {
  uint8_t tbuff[SEC_TS_EVENT_BUFF_SIZE];
  int retry = 0;

  while (1) {
    sec_ts_read_reg(SEC_TS_READ_ONE_EVENT, tbuff, sizeof(tbuff));
    if (tbuff[0] == TYPE_STATUS_EVENT_ACK && tbuff[1] == ack) {
      return true;
    }
    if (retry++ > SEC_TS_WAIT_RETRY_CNT) {
      U2HTS_LOG_ERROR("SEC_TS: Wait for ready timeout");
      return false;
    }
    sec_ts_delay_ms(20);
  }
}

// Flash operations
inline static void sec_ts_flash_cs_control(bool cs_high) {
  uint8_t data = cs_high ? 1 : 0;
  sec_ts_write_reg(SEC_TS_CMD_CS_CONTROL, &data, sizeof(data));
}

inline static void sec_ts_flash_set_datanum(uint16_t num) {
  uint8_t data[2];
  data[0] = (num >> 8) & 0xFF;
  data[1] = num & 0xFF;
  sec_ts_write_reg(SEC_TS_CMD_SET_DATA_NUM, data, sizeof(data));
}

inline static uint8_t sec_ts_flash_rdsr(void) {
  uint8_t data[2];
  sec_ts_flash_cs_control(false);
  sec_ts_flash_set_datanum(2);
  data[0] = FLASH_CMD_RDSR;
  sec_ts_write_reg(SEC_TS_CMD_FLASH_SEND_DATA, data, 1);
  sec_ts_flash_set_datanum(1);
  sec_ts_read_reg(SEC_TS_CMD_FLASH_READ_DATA, data, 1);
  sec_ts_flash_cs_control(true);
  return data[0];
}

inline static bool sec_ts_flash_is_busy(void) {
  return (sec_ts_flash_rdsr() & 0x01) == 0x01;
}

inline static bool sec_ts_flash_wait_busy(void) {
  int retry = 0;
  while (sec_ts_flash_is_busy()) {
    if (retry++ > SEC_TS_WAIT_RETRY_CNT) {
      U2HTS_LOG_ERROR("SEC_TS: Flash busy timeout");
      return false;
    }
    sec_ts_delay_ms(10);
  }
  return true;
}

inline static void sec_ts_flash_wren(void) {
  uint8_t data[2];
  sec_ts_flash_cs_control(false);
  sec_ts_flash_set_datanum(6);
  data[0] = FLASH_CMD_WREN;
  sec_ts_write_reg(SEC_TS_CMD_FLASH_SEND_DATA, data, 1);
  sec_ts_flash_cs_control(true);
}

// Calculate checksum
inline static uint8_t sec_ts_checksum(const uint8_t* data, int offset, int size) {
  uint8_t checksum = 0;
  for (int i = 0; i < size; i++)
    checksum += data[i + offset];
  return checksum;
}

// Read firmware version from IC
inline static bool sec_ts_read_fw_version_from_ic(uint8_t* version) {
  uint8_t data[20] = {0};
  sec_ts_read_reg(SEC_TS_READ_SUB_ID, data, sizeof(data));

  // Version is stored at offset 9-12
  version[0] = data[9];   // product code
  version[1] = data[10];  // project code
  version[2] = data[11];  // version major
  version[3] = data[12]; // version minor

  U2HTS_LOG_INFO("SEC_TS: IC FW version: %02X.%02X.%02X.%02X",
                  version[0], version[1], version[2], version[3]);
  return true;
}

// Check if firmware update is needed
inline static int sec_ts_check_firmware_version(const uint8_t* fw_data) {
  uint8_t ic_version[4] = {0};
  sec_ts_fw_header* fw_hd = (sec_ts_fw_header*)fw_data;

  uint8_t device_id[3] = {0};
  sec_ts_read_reg(SEC_TS_READ_DEVICE_ID, device_id, sizeof(device_id));
  U2HTS_LOG_INFO("SEC_TS: Device ID: %02X %02X %02X",
                  device_id[0], device_id[1], device_id[2]);

  // If in boot mode, update is needed
  if (device_id[0] == SEC_TS_ID_ON_BOOT) {
    return 2;  // bootloader mode, need update
  }

  sec_ts_read_fw_version_from_ic(ic_version);

  U2HTS_LOG_INFO("SEC_TS: BIN FW version: %02X.%02X.%02X.%02X",
                  (fw_hd->version >> 0) & 0xFF,
                  (fw_hd->version >> 8) & 0xFF,
                  (fw_hd->version >> 16) & 0xFF,
                  (fw_hd->version >> 24) & 0xFF);

  // Compare versions
  uint32_t bin_ver = fw_hd->version;
  uint32_t ic_ver = (ic_version[2] << 16) | (ic_version[3] << 24) |
                    (ic_version[0] << 0) | (ic_version[1] << 8);

  if ((bin_ver & 0xFFFF) != (ic_ver & 0xFFFF)) {
    return 1;  // product/project mismatch
  }

  if ((bin_ver >> 16) > (ic_ver >> 16)) {
    return 1;  // need update
  }

  return 0;  // no update needed
}

// Flash sector erase
inline static bool sec_ts_flash_erase_sector(uint32_t sector_idx) {
  uint8_t tbuf[5];

  if (sec_ts_flash_is_busy())
    return false;

  sec_ts_flash_wren();
  sec_ts_flash_cs_control(false);
  sec_ts_flash_set_datanum(5);

  tbuf[0] = SEC_TS_CMD_FLASH_SEND_DATA;
  tbuf[1] = FLASH_CMD_SE;
  tbuf[2] = (sector_idx >> 16) & 0xFF;
  tbuf[3] = (sector_idx >> 8) & 0xFF;
  tbuf[4] = sector_idx & 0xFF;

  sec_ts_write_reg(SEC_TS_CMD_FLASH_SEND_DATA, tbuf, sizeof(tbuf));
  sec_ts_flash_cs_control(true);

  return sec_ts_flash_wait_busy();
}

// Flash page write
inline static bool sec_ts_flash_write_page(uint32_t page_idx, const uint8_t* page_data) {
  uint8_t copy_data[3 + SEC_TS_FLASH_SIZE_256 + 1];
  uint8_t tcmd[2];
  int copy_left = SEC_TS_FLASH_SIZE_256 + 3;
  int copy_size = 0;
  int copy_max = SEC_TS_FLASH_SIZE_256 + 3;
  int i, j;

  copy_data[0] = (page_idx >> 8) & 0xFF;
  copy_data[1] = page_idx & 0xFF;

  for (i = 0; i < SEC_TS_FLASH_SIZE_256; i++)
    copy_data[2 + i] = page_data[i];

  copy_data[2 + SEC_TS_FLASH_SIZE_256] = sec_ts_checksum(copy_data, 0, 2 + SEC_TS_FLASH_SIZE_256);

  sec_ts_flash_cs_control(false);

  while (copy_left > 0) {
    int copy_cur = (copy_left > copy_max) ? copy_max : copy_left;

    if (copy_size == 0)
      tcmd[0] = 0xD9;
    else
      tcmd[0] = 0xDA;

    for (j = 0; j < copy_cur; j++)
      tcmd[1 + j] = copy_data[copy_size + j];

    sec_ts_write_reg(SEC_TS_CMD_FLASH_SEND_DATA, tcmd, 1 + copy_cur);

    copy_size += copy_cur;
    copy_left -= copy_cur;
  }

  sec_ts_delay_ms(5);
  sec_ts_flash_cs_control(true);

  return true;
}

// Write firmware to flash
inline static int sec_ts_flash_write(uint32_t addr, const uint8_t* data, uint32_t size) {
  uint32_t page_idx_start = addr / SEC_TS_FLASH_SIZE_256;
  uint32_t page_idx_end = (addr + size - 1) / SEC_TS_FLASH_SIZE_256;
  uint32_t page_num = page_idx_end - page_idx_start + 1;
  int size_left = size;
  int size_copy;
  uint8_t page_buf[SEC_TS_FLASH_SIZE_256];
  int page_idx;

  // Erase sectors first (erase in chunks of 16 pages)
  for (page_idx = (int)((page_num - 1) / 16); page_idx >= 0; page_idx--) {
    if (!sec_ts_flash_erase_sector(page_idx_start + page_idx * 16)) {
      U2HTS_LOG_ERROR("SEC_TS: Sector erase failed at %d", page_idx);
      return -1;
    }
  }
  sec_ts_delay_ms(page_num + 10);

  // Write pages
  size_copy = (size % SEC_TS_FLASH_SIZE_256);
  if (size_copy == 0)
    size_copy = SEC_TS_FLASH_SIZE_256;

  memset(page_buf, 0, SEC_TS_FLASH_SIZE_256);

  for (page_idx = (int)page_num - 1; page_idx >= 0; page_idx--) {
    memcpy(page_buf, data + (page_idx * SEC_TS_FLASH_SIZE_256), size_copy);
    if (!sec_ts_flash_write_page(page_idx + page_idx_start, page_buf)) {
      U2HTS_LOG_ERROR("SEC_TS: Page write failed at %d", page_idx);
      return -1;
    }
    size_copy = SEC_TS_FLASH_SIZE_256;
    sec_ts_delay_ms(5);
  }

  return size;
}

// Perform firmware update
inline static bool sec_ts_firmware_update(const uint8_t* fw_data, size_t fw_size) {
  uint8_t device_id[3] = {0};
  int ret;

  U2HTS_LOG_INFO("SEC_TS: Starting firmware update...");

  // Write firmware to flash
  ret = sec_ts_flash_write(0, fw_data, fw_size);
  if (ret < 0) {
    U2HTS_LOG_ERROR("SEC_TS: Firmware write failed");
    return false;
  }

  U2HTS_LOG_INFO("SEC_TS: Firmware written, resetting...");

  // Reset and wait for boot
  sec_ts_write_cmd(SEC_TS_CMD_SW_RESET);
  sec_ts_delay_ms(500);
  sec_ts_wait_for_ready(SEC_TS_ACK_BOOT_COMPLETE);

  // Verify
  sec_ts_read_reg(SEC_TS_READ_DEVICE_ID, device_id, sizeof(device_id));
  if (device_id[0] != SEC_TS_ID_ON_FW) {
    U2HTS_LOG_ERROR("SEC_TS: FW update verify failed, ID=%02X", device_id[0]);
    return false;
  }

  U2HTS_LOG_INFO("SEC_TS: Firmware update successful!");
  return true;
}

// Run calibration
inline static bool sec_ts_run_calibration(void) {
  U2HTS_LOG_INFO("SEC_TS: Running calibration...");

  sec_ts_write_cmd(SEC_TS_CMD_CALIBRATION_OFFSET_SDC);

  sec_ts_delay_ms(1000);

  // Reset after calibration
  sec_ts_write_cmd(SEC_TS_CMD_SW_RESET);
  sec_ts_delay_ms(500);
  sec_ts_wait_for_ready(SEC_TS_ACK_BOOT_COMPLETE);

  U2HTS_LOG_INFO("SEC_TS: Calibration done");
  return true;
}

// Run selftest
inline static bool sec_ts_run_selftest(void) {
  uint8_t cmd_data[10];
  uint8_t tpara = 0x03;
  bool ret;

  U2HTS_LOG_INFO("SEC_TS: Running selftest...");

  // Set selftest type
  cmd_data[0] = 0xFF;
  sec_ts_write_reg(SEC_TS_CMD_SELFTEST_TYPE, cmd_data, 1);
  sec_ts_delay_ms(100);

  // Set p-to-p test
  cmd_data[0] = 0x0;
  cmd_data[1] = 0x64;
  sec_ts_write_reg(SEC_TS_CMD_SELFTEST_PTOP, cmd_data, 4);
  sec_ts_delay_ms(100);

  // Execute selftest
  sec_ts_write_reg(SEC_TS_CMD_SELFTEST, &tpara, 1);
  sec_ts_delay_ms(1000);

  // Wait for result
  ret = sec_ts_wait_for_ready(SEC_TS_ACK_SELF_TEST_DONE);
  if (!ret) {
    U2HTS_LOG_ERROR("SEC_TS: Selftest timeout");
    return false;
  }

  // Reset after selftest
  sec_ts_write_cmd(SEC_TS_CMD_SW_RESET);
  sec_ts_delay_ms(500);

  U2HTS_LOG_INFO("SEC_TS: Selftest completed");
  return true;
}

inline static bool sec_ts_setup(U2HTS_BUS_TYPES bus_type) {
  int fw_check_result;

  // Save bus_type for further use
  sec_ts_bus_type = bus_type;

  // Detect controller on I2C
  if (bus_type == UB_I2C) {
    // Read device ID to verify controller presence
    uint8_t device_id[3] = {0};
    sec_ts_read_reg(SEC_TS_READ_DEVICE_ID, device_id, sizeof(device_id));
    U2HTS_LOG_INFO("SEC_TS: Device ID: %02X %02X %02X",
                   device_id[0], device_id[1], device_id[2]);

    // Send software reset
    sec_ts_write_cmd(SEC_TS_CMD_SW_RESET);
    sec_ts_delay_ms(500);

    // Wait for boot complete
    sec_ts_wait_for_ready(SEC_TS_ACK_BOOT_COMPLETE);

#if !SEC_TS_SKIP_FW_UPDATE
    // Check and update firmware
    fw_check_result = sec_ts_check_firmware_version(sec_ts_fw_data);
    if (fw_check_result > 0) {
      U2HTS_LOG_INFO("SEC_TS: Firmware update needed (result=%d)", fw_check_result);
      if (sec_ts_firmware_update(sec_ts_fw_data, SEC_TS_FW_SIZE)) {
        sec_ts_fw_updated = true;
        // Run calibration after firmware update
        sec_ts_run_calibration();
      }
    } else {
      U2HTS_LOG_INFO("SEC_TS: Firmware is up to date");
    }
#else
    U2HTS_LOG_INFO("SEC_TS: Skipping firmware update");
#endif

#if !SEC_TS_SKIP_SELFTEST
    // Run selftest (optional, can be disabled)
    if (!sec_ts_run_selftest()) {
      U2HTS_LOG_WARN("SEC_TS: Selftest reported issues");
    }
#endif

    U2HTS_LOG_INFO("SEC_TS: Controller initialized");
  }

  return true;
}

inline static bool sec_ts_coord_fetch() {
  uint8_t event_buff[SEC_TS_EVENT_BUFF_SIZE];
  uint8_t active_touches = 0;

  // Read all events until buffer is empty
  while (1) {
    sec_ts_read_reg(SEC_TS_READ_ONE_EVENT, event_buff, sizeof(event_buff));

    uint8_t event_id = event_buff[0] >> 6;

    if (event_id == SEC_TS_STATUS_EVENT) {
      // Status event - can indicate errors or acknowledgments
      if (event_buff[0] == TYPE_STATUS_EVENT_ACK &&
          event_buff[1] == SEC_TS_ACK_BOOT_COMPLETE) {
        // Boot complete acknowledgment
      }
      // Continue to read next event
      continue;
    }

    if (event_id == SEC_TS_COORDINATE_EVENT) {
      sec_ts_event_coord* coord = (sec_ts_event_coord*)event_buff;

      uint8_t tid = coord->tid;
      uint8_t action = coord->tchsta;

      // Skip hover/proximity events (tid == 10 for hover)
      if (tid >= SEC_TS_MAX_TOUCH_COUNT)
        continue;

      // Parse coordinates
      uint16_t x = (coord->x_11_4 << 4) | coord->x_3_0;
      uint16_t y = (coord->y_11_4 << 4) | coord->y_3_0;
      uint8_t pressure = coord->z;
      uint8_t major = coord->major;
      uint8_t minor = coord->minor;

      if (action == SEC_TS_COORD_ACTION_PRESS ||
          action == SEC_TS_COORD_ACTION_MOVE) {
        u2hts_set_tp(tid, true, tid, x, y, major, minor, pressure);
        active_touches++;
      } else if (action == SEC_TS_COORD_ACTION_RELEASE) {
        u2hts_set_tp(tid, false, tid, x, y, 0, 0, 0);
      }
      continue;
    }

    if (event_id == SEC_TS_GESTURE_EVENT) {
      // Gesture event - not processed in basic mode
      continue;
    }

    // Unknown event or no more events
    break;
  }

  // Set touch count and clear event stack
  u2hts_set_tp_count(active_touches);
  sec_ts_write_cmd(SEC_TS_CMD_CLEAR_EVENT_STACK);

  return true;
}

inline static void sec_ts_get_config(u2hts_touch_controller_config* cfg) {
  // SEC TS typically gets config via DT/platform data
  // Default values for common displays, should be configured
  cfg->x_max = 1080;   // Default to common resolution
  cfg->y_max = 1920;  // Default to common resolution
  cfg->max_tps = SEC_TS_MAX_TOUCH_COUNT;
}