#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char url[256];
    char version[32];
    char sha256[65];     // Hex string of SHA256 (64 characters)
    char signature[128]; // Base64 Ed25519 signature
} ota_update_params_t;

typedef struct {
    uint32_t request_id;
    uint8_t firmware_target;    // 0x01 (GW ESP32), 0x03 (SUB ESP32), 0x04 (SUB STM32)
    uint16_t product_id;
    uint8_t hw_rev;
    char sub_device_id[17];
    uint8_t sub_mac[6];
    uint8_t sub_addr_type;
    uint32_t fw_version;
    uint32_t plain_fw_length;
    uint8_t plain_fw_sha256[32];
    uint32_t plain_fw_crc32;
    uint64_t url_expires_at;
    char url[512];
} ota_v2_params_t;

typedef void (*ota_status_cb_t)(const char *status, const char *reason);
typedef void (*ota_fw_status_cb_t)(uint32_t request_id, uint8_t firmware_target,
                                   uint8_t state, uint8_t progress_pct,
                                   uint8_t error_code, uint32_t bytes_transferred,
                                   uint32_t reported_version);

/**
 * @brief Initializes the OTA manager.
 *        Checks if running partition was newly updated and cancels rollback.
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ota_manager_init(void);

/**
 * @brief Sets a callback to receive OTA progress and completion status.
 * @param cb Callback function
 */
void ota_manager_set_status_cb(ota_status_cb_t cb);

/**
 * @brief Sets a callback to emit v2.19 FIRMWARE_STATUS (0x39) binary events.
 */
void ota_manager_set_fw_status_cb(ota_fw_status_cb_t cb);

/**
 * @brief Starts an OTA update from the given parameters (URL, checksum, signature).
 *        Runs in a background FreeRTOS task.
 * @param params Pointer to OTA update parameters
 * @return esp_err_t ESP_OK if task was spawned successfully
 */
esp_err_t ota_manager_start_update(const ota_update_params_t *params);

/**
 * @brief Starts a v2.19 binary FIRMWARE_UPDATE (0x53) for GW (0x01) or BLE Sub-device (0x03/0x04).
 */
esp_err_t ota_manager_start_v2_update(const ota_v2_params_t *params);

/**
 * @brief Request abort of active OTA download or BLE sub-device transfer.
 */
void ota_manager_abort(uint32_t request_id, uint8_t reason);

/**
 * @brief Check whether abort has been requested (optionally matching request_id, 0 = any).
 */
bool ota_manager_is_aborted(uint32_t request_id);

/**
 * @brief Clear abort flag before starting a new OTA job.
 */
void ota_manager_clear_abort(void);

#endif // OTA_MANAGER_H
