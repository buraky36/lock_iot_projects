#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// OTA Firmware Targets (Onloi Protocol Section 2.1)
#define OTA_TARGET_ALL_LOCAL         0x00
#define OTA_TARGET_UNIT1_ESP         0x01
#define OTA_TARGET_UNIT2_STM         0x02
#define OTA_TARGET_UNIT3_BLE_SUB     0x03

// OTA State Codes (Onloi Protocol Section 7)
#define OTA_STATE_IDLE               0x00
#define OTA_STATE_ACCEPTED           0x01
#define OTA_STATE_DOWNLOADING        0x02
#define OTA_STATE_VERIFYING          0x03
#define OTA_STATE_DOWNLOAD_VERIFIED  0x04
#define OTA_STATE_ESP_APPLYING       0x10
#define OTA_STATE_STM32_IMAGE_READY  0x23
#define OTA_STATE_STM32_TRANSFERRING 0x24
#define OTA_STATE_STM32_VERIFYING    0x25
#define OTA_STATE_BLE_SUB_IMAGE_READY 0x33
#define OTA_STATE_BLE_SUB_TRANSFERRING 0x34
#define OTA_STATE_BLE_SUB_VERIFYING  0x35
#define OTA_STATE_COMPLETED          0x06
#define OTA_STATE_FAILED             0x07
#define OTA_STATE_ABORTED            0x08

// OTA Error Codes for FIRMWARE_STATUS
#define OTA_ERR_NONE                 0x00
#define OTA_ERR_NETWORK              0x01  // HTTP connection or read failed
#define OTA_ERR_SHA256_MISMATCH      0x02  // SHA-256 digest mismatch
#define OTA_ERR_CRC32_MISMATCH       0x03  // CRC-32 checksum mismatch
#define OTA_ERR_TARGET_INVALID       0x04  // Unsupported target (e.g. 0x02, 0x03 on ESP-only)
#define OTA_ERR_PRODUCT_ID_MISMATCH  0x05  // Product ID mismatch
#define OTA_ERR_HW_REV_MISMATCH      0x06  // Hardware revision mismatch
#define OTA_ERR_FLASH_WRITE          0x07  // Partition write or begin failed
#define OTA_ERR_URL_EXPIRED          0x08  // URL expired before download
#define OTA_ERR_PAYLOAD_INVALID      0x09  // Malformed frame or invalid URL
#define OTA_ERR_ALREADY_IN_PROGRESS  0x0A  // An OTA update is already running
#define OTA_ERR_IMAGE_TOO_LARGE      0x0B  // Image size exceeds OTA partition size

// Parameters passed to start OTA update
typedef struct {
    uint32_t request_id;
    uint8_t firmware_target;
    uint32_t firmware_version;
    uint32_t plain_fw_length;
    uint8_t plain_fw_sha256[32];
    bool has_sha256;
    uint32_t plain_fw_crc32;
    bool has_crc32;
    char *url; // Dynamically allocated string (ownership transferred to ota_manager)
} ota_update_params_t;

// Callback signature for OTA progress / status updates
typedef void (*ota_status_cb_t)(uint32_t request_id, uint8_t target, uint8_t state,
                               uint8_t progress_pct, uint8_t error_code,
                               uint32_t bytes_transferred, uint32_t reported_version);

/**
 * @brief Register callback for OTA status and progress updates.
 */
void ota_manager_set_status_cb(ota_status_cb_t cb);

/**
 * @brief Starts the full Onloi v2.18 OTA update process in background.
 *
 * @param params Pointer to OTA parameters. Note: params->url ownership is transferred!
 * @return true if started successfully, false if another OTA is running or error.
 */
bool ota_manager_start_update(const ota_update_params_t *params);

/**
 * @brief Abort active OTA update.
 *
 * @param request_id The OTA requestId to abort.
 * @param reason Abort reason code (e.g. 0x01 = mobile/admin abort).
 * @return true if abort requested or acknowledged.
 */
bool ota_manager_abort(uint32_t request_id, uint8_t reason);

/**
 * @brief Check if OTA update is currently running.
 */
bool ota_manager_is_running(void);

/**
 * @brief Legacy simple URL OTA starter (backward compatibility).
 *
 * @param url The HTTPS URL to download firmware from.
 * @return true if OTA task started.
 */
bool ota_manager_start(const char *url);

#endif // OTA_MANAGER_H
