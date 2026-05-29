/*
 * Samsung SEC TS Firmware s6d6ft0_v1.10_20170918
 * Converted from .i file
 */
#ifndef __SEC_TS_FIRMWARE_H__
#define __SEC_TS_FIRMWARE_H__

// Firmware version info
#define SEC_TS_FW_VERSION 0x0110  // v1.10
#define SEC_TS_FW_DATE 0x20170918 // 2017/09/18

// Firmware array - size: 4013 lines
static const uint8_t sec_ts_fw_data[] = {
#include "s6d6ft0_v1.10_20170918.i"
};

#define SEC_TS_FW_SIZE (sizeof(sec_ts_fw_data))

#endif // __SEC_TS_FIRMWARE_H__