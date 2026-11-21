#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

#define BLE_MAC_LEN 6
#define BLE_MAX_DATA_LEN 31

typedef struct {
  uint8_t mac[BLE_MAC_LEN];
  uint8_t addr_type; // 0: Public, 1: Random
  int8_t rssi;
  uint8_t data[BLE_MAX_DATA_LEN];
  uint8_t data_len;
  bool is_onloi_beacon;
  char device_code[7]; // e.g., "OKGWX1" + null terminator
  bool claimed;
} ble_scan_report_t;

#define ONLOI_NUS_SVC_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define ONLOI_NUS_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define ONLOI_NUS_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

typedef enum {
  BLE_OP_PASSTHROUGH_CMD = 0, // Relay pre-encrypted command frame (Blind Relay)
  BLE_OP_DEVICE_CLAIM = 1,    // KEX_INIT + KEY_DELIVER claim handshake
  BLE_OP_DEVICE_UNCLAIM = 2,  // UNCLAIM soft reset (0x73)
  BLE_OP_KEY_ROTATE = 3,      // KEY_DELIVER (purpose=0x02 ROTATE)
  BLE_OP_AUTO_CONNECT = 4,    // Background persistent connection for telemetry
  BLE_OP_SUB_OTA = 5,         // Sub-device BLE OTA transfer (0x78-0x7B)
  BLE_OP_GET_FW_INFO = 6,     // Sub-device GET_FIRMWARE_INFO (0x5C -> 0x3A)
} ble_op_type_t;

typedef struct {
  uint32_t ota_request_id;
  uint8_t firmware_target;    // 0x03 (SUB_ESP) or 0x04 (SUB_STM)
  uint8_t unit_target;        // 0x01 (ESP32) or 0x02 (STM32)
  uint16_t product_id;
  uint8_t hw_rev;
  uint32_t fw_version;
  uint32_t total_length;
  uint32_t plain_fw_crc32;
  uint8_t plain_fw_sha256[32];
} ble_sub_ota_meta_t;

typedef struct {
  ble_op_type_t op_type;
  uint8_t mac[BLE_MAC_LEN];
  uint8_t addr_type;     // 0: Public, 1: Random
  char service_uuid[48]; // e.g. "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
  char char_uuid[48];    // RX (Write) Char UUID
  char tx_char_uuid[48]; // TX (Notify) Char UUID
  uint8_t cmd_data[256];
  uint16_t cmd_len;
  bool is_encrypted;     // true if cmd_data is already BLE sessionKey-encrypted
  char request_id[64];
  char target_device_id[32];
  uint32_t current_key_version;
  uint32_t timeout_ms;
  ble_sub_ota_meta_t sub_ota;
} ble_lock_cmd_t;

// Queue to send scanned BLE data to MQTT Manager
extern QueueHandle_t ble_scan_queue;

// Queue to receive lock control commands from MQTT Manager
extern QueueHandle_t ble_cmd_queue;

/**
 * @brief Initialize the NimBLE stack
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_manager_init(void);

/**
 * @brief Start BLE scanner and listener tasks
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_manager_start(void);

/**
 * @brief Stop BLE scanner and listener tasks
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_manager_stop(void);

/**
 * @brief Manually trigger or restart BLE scanning (e.g. on button short-press)
 */
void ble_manager_trigger_scan(void);

/**
 * @brief Check if BLE manager is currently scanning
 * @return true if scanning, false otherwise
 */
bool ble_manager_is_scanning(void);

#endif // BLE_MANAGER_H
