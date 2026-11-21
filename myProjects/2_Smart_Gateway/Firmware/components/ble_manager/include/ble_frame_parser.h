#ifndef BLE_FRAME_PARSER_H
#define BLE_FRAME_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#define BLE_FRAME_SOF 0x4F

// Protocol Message Types (Section 11.2)
#define BLE_MSG_TYPE_REQ    0x01 // REQUEST: Client -> Device, response expected
#define BLE_MSG_TYPE_RESP   0x02 // RESPONSE: Device -> Client
#define BLE_MSG_TYPE_EVENT  0x03 // EVENT: Device -> Client (unsolicited notify)
#define BLE_MSG_TYPE_CMD    0x04 // COMMAND: Server/App -> Device

// Protocol CMD codes (v2.19 Catalog + GW v1.3 + Sub-device OTA)
// Access & Status Events
#define BLE_CMD_LOCK_OPENED          0x10
#define BLE_CMD_PIN_ENTERED          0x12
#define BLE_CMD_NFC_SCANNED          0x13
#define BLE_CMD_BUTTON_UNLOCK_REQ    0x16
#define BLE_CMD_LOCK_CLOSED          0x17
#define BLE_CMD_DOOR_POSITION        0x18
#define BLE_CMD_BOLT_STATE           0x19
#define BLE_CMD_HANDLE_EVENT         0x1A

// Enroll Events & Commands
#define BLE_CMD_NFC_ENROLL_READY     0x22
#define BLE_CMD_NFC_ENROLLED         0x23
#define BLE_CMD_FP_ENROLL_READY      0x24
#define BLE_CMD_FP_SCAN_STEP         0x25
#define BLE_CMD_FP_ENROLLED          0x26
#define BLE_CMD_NFC_ENROLL_START     0x44
#define BLE_CMD_FP_ENROLL_START      0x46

// Telemetry & Sync
#define BLE_CMD_BATTERY_REPORT       0x32
#define BLE_CMD_TIME_SYNC            0x33
#define BLE_CMD_OFFLINE_LOG_BATCH    0x35

// Control & Management Commands
#define BLE_CMD_REMOTE_OPEN          0x40
#define BLE_CMD_REMOTE_CLOSE         0x4C
#define BLE_CMD_FACTORY_RESET        0x50
#define BLE_CMD_LOCAL_FACTORY_RESET  0x61

// OTA & Firmware Info
#define BLE_CMD_FIRMWARE_STATUS      0x39 // FIRMWARE_STATUS: OTA progress/result EVENT
#define BLE_CMD_FIRMWARE_INFO        0x3A // FIRMWARE_INFO: Version info EVENT
#define BLE_CMD_FIRMWARE_UPDATE      0x53 // FIRMWARE_UPDATE: Server->GW/Device OTA command
#define BLE_CMD_FIRMWARE_ABORT       0x5A // FIRMWARE_ABORT: Server->GW/Device OTA abort
#define BLE_CMD_GET_FIRMWARE_INFO    0x5C // GET_FIRMWARE_INFO: Server->GW/Device version query
#define BLE_CMD_SESSION_INIT         0x70 // SESSION_INIT: Authenticate with localKey + ECDH
#define BLE_CMD_KEX_INIT             0x71 // KEX_INIT: Claim bootstrap ephemeral ECDH
#define BLE_CMD_KEY_DELIVER          0x72 // KEY_DELIVER: Deliver localKey (claim/rotate)
#define BLE_CMD_UNCLAIM              0x73 // UNCLAIM: Clear localKey & claim status
#define BLE_CMD_BLE_OTA_START        0x78 // BLE_OTA_START: Sub-device BLE OTA start
#define BLE_CMD_BLE_OTA_CHUNK        0x79 // BLE_OTA_CHUNK: Sub-device BLE OTA data chunk
#define BLE_CMD_BLE_OTA_FINISH       0x7A // BLE_OTA_FINISH: Sub-device BLE OTA verify & apply
#define BLE_CMD_BLE_OTA_ABORT        0x7B // BLE_OTA_ABORT: Sub-device BLE OTA abort

// Firmware Target codes (v2.19 + SubDevice OTA Report Section 3.1)
#define FW_TARGET_LOCAL_ALL        0x00 // Local units (ESP+STM)
#define FW_TARGET_UNIT1_ESP        0x01 // Device/GW own ESP32
#define FW_TARGET_UNIT2_STM        0x02 // Device own STM32
#define FW_TARGET_UNIT3_BLE_SUB_ESP 0x03 // BLE Sub-device ESP32
#define FW_TARGET_UNIT4_BLE_SUB_STM 0x04 // BLE Sub-device STM32

// Firmware Status State codes (v2.19 Section 47)
#define FW_STATE_IDLE                 0x00
#define FW_STATE_ACCEPTED             0x01
#define FW_STATE_DOWNLOADING          0x02
#define FW_STATE_VERIFYING            0x03
#define FW_STATE_DOWNLOAD_VERIFIED    0x04
#define FW_STATE_COMPLETED            0x06
#define FW_STATE_FAILED               0x07
#define FW_STATE_ABORTED              0x08
#define FW_STATE_ESP_APPLYING         0x10
#define FW_STATE_STM32_IMAGE_READY    0x23
#define FW_STATE_STM32_TRANSFERRING   0x24
#define FW_STATE_STM32_VERIFYING      0x25
#define FW_STATE_BLE_SUB_IMAGE_READY  0x33
#define FW_STATE_BLE_SUB_TRANSFERRING 0x34
#define FW_STATE_BLE_SUB_VERIFYING    0x35

// Protocol Status codes (Section 11.4 & v2.19 OTA)
#define BLE_STATUS_OK                   0x00
#define BLE_STATUS_AUTH_FAILED          0x01
#define BLE_STATUS_REPLAY_DETECTED      0x02
#define BLE_STATUS_ALREADY_CLAIMED      0x03
#define BLE_STATUS_NOT_CLAIMED          0x04
#define BLE_STATUS_TARGET_INVALID       0x04
#define BLE_STATUS_TIMEOUT              0x05
#define BLE_STATUS_BUSY                 0x06
#define BLE_STATUS_PRODUCT_MISMATCH     0x08
#define BLE_STATUS_INSUFFICIENT_STORAGE 0x09
#define BLE_STATUS_LOW_BATTERY          0x0A
#define BLE_STATUS_CHECKSUM_MISMATCH    0x0B
#define BLE_STATUS_SUB_ADDR_REQUIRED    0x10
#define BLE_STATUS_SUB_NOT_FOUND        0x11
#define BLE_STATUS_DOWNLOAD_FAILED      0x12

typedef struct {
    uint8_t sof;
    uint8_t ver;
    uint8_t type;
    uint8_t cmd;
    uint16_t seq;
    uint16_t len;
    uint8_t status;
    uint64_t timestamp;
    uint8_t *payload;
    uint16_t crc;
} ble_frame_t;

uint16_t calc_crc16_ccitt(const uint8_t *data, uint16_t len);
bool ble_frame_parse(const uint8_t *data, uint16_t data_len, ble_frame_t *out_frame);
bool ble_frame_parse_with_auth(const uint8_t *data, uint16_t data_len, ble_frame_t *out_frame,
                               const uint8_t **out_auth_tag, bool *out_has_auth_tag);
uint16_t ble_frame_build_req(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf);
uint16_t ble_frame_build_resp(uint8_t cmd, uint16_t seq, uint8_t status, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf);
uint16_t ble_frame_build_event(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf);
uint16_t ble_frame_build_cmd(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf);
uint16_t ble_frame_build_with_type(uint8_t msg_type, uint8_t cmd, uint16_t seq, uint8_t status,
                                   uint64_t timestamp, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf);

#endif // BLE_FRAME_PARSER_H
