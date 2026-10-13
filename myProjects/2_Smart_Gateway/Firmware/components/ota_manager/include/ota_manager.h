#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char url[256];
    char version[32];
    char sha256[65];     // Hex string of SHA256 (64 characters)
    char signature[128]; // Base64 Ed25519 signature
} ota_update_params_t;

typedef void (*ota_status_cb_t)(const char *status, const char *reason);

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
 * @brief Starts an OTA update from the given parameters (URL, checksum, signature).
 *        Runs in a background FreeRTOS task.
 * @param params Pointer to OTA update parameters
 * @return esp_err_t ESP_OK if task was spawned successfully
 */
esp_err_t ota_manager_start_update(const ota_update_params_t *params);

#endif // OTA_MANAGER_H
