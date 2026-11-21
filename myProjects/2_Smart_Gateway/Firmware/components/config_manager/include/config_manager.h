#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define MQTT_URI_MAX_LEN  128
#define DEVICE_ID_MAX_LEN 32

typedef struct {
    char wifi_ssid[WIFI_SSID_MAX_LEN];
    char wifi_pass[WIFI_PASS_MAX_LEN];
    char mqtt_uri[MQTT_URI_MAX_LEN];
    uint16_t mqtt_port;
    char device_id[DEVICE_ID_MAX_LEN];
    bool is_configured;
    bool is_claimed;
    uint8_t local_key[32];
} gateway_config_t;

// ==========================================
// Sub-device key store definitions (NVS: "subkeys")
// ==========================================
#define SUBKEY_MAX_DEVICES   16
#define SUBKEY_DEVICE_ID_LEN 16
#define SUBKEY_LOCAL_KEY_LEN 32

typedef struct {
    uint8_t mac[6];
    uint8_t addr_type; // 0: Public, 1: Random
    char device_id[SUBKEY_DEVICE_ID_LEN + 1]; // null-terminated, up to 16 chars
    uint8_t local_key[SUBKEY_LOCAL_KEY_LEN];
    uint32_t key_version;
    bool in_use;
} subkey_entry_t;

/**
 * @brief Initialize NVS storage and subkey store
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t config_manager_init(void);

/**
 * @brief Read configuration from NVS
 * @param config Pointer to store configuration data
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t config_manager_load(gateway_config_t *config);

/**
 * @brief Write configuration to NVS
 * @param config Pointer to the configuration to save
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t config_manager_save(const gateway_config_t *config);

/**
 * @brief Reset configuration in NVS (erases configuration data)
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t config_manager_reset(void);

// ==========================================
// Sub-device Key Store API
// ==========================================

/**
 * @brief Initialize subkey storage (loads all keys from "subkeys" namespace into cache)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t subkey_init(void);

/**
 * @brief Look up subkey entry by 6-byte BLE MAC address
 * @param mac 6-byte MAC address
 * @param out_entry Destination buffer for subkey entry
 * @return esp_err_t ESP_OK if found, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t subkey_get_by_mac(const uint8_t *mac, subkey_entry_t *out_entry);

/**
 * @brief Look up subkey entry by deviceId string
 * @param device_id Null-terminated device ID string
 * @param out_entry Destination buffer for subkey entry
 * @return esp_err_t ESP_OK if found, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t subkey_get_by_device_id(const char *device_id, subkey_entry_t *out_entry);

/**
 * @brief Store or update a subkey entry in NVS and RAM cache
 * @param entry Pointer to entry to store
 * @return esp_err_t ESP_OK on success
 */
esp_err_t subkey_put(const subkey_entry_t *entry);

/**
 * @brief Delete a subkey entry from NVS and RAM cache by MAC address
 * @param mac 6-byte MAC address
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t subkey_delete_by_mac(const uint8_t *mac);

/**
 * @brief Delete a subkey entry from NVS and RAM cache by deviceId string
 * @param device_id Null-terminated device ID string
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t subkey_delete_by_device_id(const char *device_id);

/**
 * @brief Get all stored subkeys
 * @param entries Output array of subkey entries
 * @param max_entries Capacity of output array
 * @param out_count Number of entries returned
 * @return esp_err_t ESP_OK on success
 */
esp_err_t subkey_get_all(subkey_entry_t *entries, size_t max_entries, size_t *out_count);

/**
 * @brief Check if a sub-device MAC is claimed (stored in subkey store)
 * @param mac 6-byte MAC address
 * @return true if claimed, false otherwise
 */
bool subkey_is_claimed(const uint8_t *mac);

#endif // CONFIG_MANAGER_H
