#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config_manager.h"

static const char *TAG = "CONFIG_MGR";
static const char *NVS_NAMESPACE = "storage";
static const char *NVS_KEY = "gw_config";

// Subkeys NVS storage
static const char *NVS_SUBKEYS_NS = "subkeys";
static const char *NVS_SUBKEYS_IDX = "sub_idx";

typedef struct {
    uint8_t count;
    uint8_t macs[SUBKEY_MAX_DEVICES][6];
} __attribute__((packed)) subkey_nvs_idx_t;

typedef struct {
    uint8_t mac[6];
    uint8_t addr_type;
    char device_id[SUBKEY_DEVICE_ID_LEN + 1];
    uint8_t local_key[SUBKEY_LOCAL_KEY_LEN];
    uint32_t key_version;
} __attribute__((packed)) subkey_nvs_blob_t;

static subkey_entry_t s_subkey_cache[SUBKEY_MAX_DEVICES];
static SemaphoreHandle_t s_subkey_mutex = NULL;
static bool s_subkey_initialized = false;

static void mac_to_nvs_key(const uint8_t *mac, char *out_key, size_t max_len) {
    snprintf(out_key, max_len, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t subkey_init(void)
{
    if (s_subkey_mutex == NULL) {
        s_subkey_mutex = xSemaphoreCreateMutex();
    }
    if (s_subkey_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create subkey mutex");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_subkey_mutex, portMAX_DELAY);
    memset(s_subkey_cache, 0, sizeof(s_subkey_cache));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_SUBKEYS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Subkeys namespace empty or not initialized yet (%s)", esp_err_to_name(err));
        s_subkey_initialized = true;
        xSemaphoreGive(s_subkey_mutex);
        return ESP_OK;
    }

    subkey_nvs_idx_t idx;
    size_t idx_size = sizeof(idx);
    err = nvs_get_blob(handle, NVS_SUBKEYS_IDX, &idx, &idx_size);
    if (err != ESP_OK || idx.count == 0) {
        ESP_LOGI(TAG, "No subkeys registered in index");
        nvs_close(handle);
        s_subkey_initialized = true;
        xSemaphoreGive(s_subkey_mutex);
        return ESP_OK;
    }

    uint8_t loaded_count = 0;
    for (uint8_t i = 0; i < idx.count && i < SUBKEY_MAX_DEVICES; i++) {
        char key[13];
        mac_to_nvs_key(idx.macs[i], key, sizeof(key));

        subkey_nvs_blob_t blob;
        size_t blob_size = sizeof(blob);
        err = nvs_get_blob(handle, key, &blob, &blob_size);
        if (err == ESP_OK) {
            memcpy(s_subkey_cache[loaded_count].mac, blob.mac, 6);
            s_subkey_cache[loaded_count].addr_type = blob.addr_type;
            strncpy(s_subkey_cache[loaded_count].device_id, blob.device_id, SUBKEY_DEVICE_ID_LEN);
            s_subkey_cache[loaded_count].device_id[SUBKEY_DEVICE_ID_LEN] = '\0';
            memcpy(s_subkey_cache[loaded_count].local_key, blob.local_key, SUBKEY_LOCAL_KEY_LEN);
            s_subkey_cache[loaded_count].key_version = blob.key_version;
            s_subkey_cache[loaded_count].in_use = true;

            ESP_LOGI(TAG, "Loaded subkey [%u]: MAC=%02x:%02x:%02x:%02x:%02x:%02x, DeviceId='%s', v=%lu",
                     loaded_count,
                     blob.mac[0], blob.mac[1], blob.mac[2], blob.mac[3], blob.mac[4], blob.mac[5],
                     blob.device_id, (unsigned long)blob.key_version);
            loaded_count++;
        } else {
            ESP_LOGW(TAG, "Failed to load subkey for MAC key %s: %s", key, esp_err_to_name(err));
        }
    }

    nvs_close(handle);
    s_subkey_initialized = true;
    xSemaphoreGive(s_subkey_mutex);
    ESP_LOGI(TAG, "Subkey store initialized with %u active devices", loaded_count);
    return ESP_OK;
}

esp_err_t subkey_get_by_mac(const uint8_t *mac, subkey_entry_t *out_entry)
{
    if (!mac || !out_entry) return ESP_ERR_INVALID_ARG;
    if (!s_subkey_initialized) {
        subkey_init();
    }

    xSemaphoreTake(s_subkey_mutex, portMAX_DELAY);
    for (int i = 0; i < SUBKEY_MAX_DEVICES; i++) {
        if (s_subkey_cache[i].in_use && memcmp(s_subkey_cache[i].mac, mac, 6) == 0) {
            memcpy(out_entry, &s_subkey_cache[i], sizeof(subkey_entry_t));
            xSemaphoreGive(s_subkey_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_subkey_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t subkey_get_by_device_id(const char *device_id, subkey_entry_t *out_entry)
{
    if (!device_id || !out_entry) return ESP_ERR_INVALID_ARG;
    if (!s_subkey_initialized) {
        subkey_init();
    }

    xSemaphoreTake(s_subkey_mutex, portMAX_DELAY);
    for (int i = 0; i < SUBKEY_MAX_DEVICES; i++) {
        if (s_subkey_cache[i].in_use && strcmp(s_subkey_cache[i].device_id, device_id) == 0) {
            memcpy(out_entry, &s_subkey_cache[i], sizeof(subkey_entry_t));
            xSemaphoreGive(s_subkey_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_subkey_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t subkey_put(const subkey_entry_t *entry)
{
    if (!entry) return ESP_ERR_INVALID_ARG;
    if (!s_subkey_initialized) {
        subkey_init();
    }

    xSemaphoreTake(s_subkey_mutex, portMAX_DELAY);

    // 1. Check if MAC or deviceId already exists in cache
    int target_idx = -1;
    for (int i = 0; i < SUBKEY_MAX_DEVICES; i++) {
        if (s_subkey_cache[i].in_use) {
            if (memcmp(s_subkey_cache[i].mac, entry->mac, 6) == 0 ||
                (strlen(entry->device_id) > 0 && strcmp(s_subkey_cache[i].device_id, entry->device_id) == 0)) {
                target_idx = i;
                break;
            }
        }
    }

    // 2. If not found, find first free slot
    if (target_idx < 0) {
        for (int i = 0; i < SUBKEY_MAX_DEVICES; i++) {
            if (!s_subkey_cache[i].in_use) {
                target_idx = i;
                break;
            }
        }
    }

    if (target_idx < 0) {
        ESP_LOGE(TAG, "Subkey store full (max %d devices)", SUBKEY_MAX_DEVICES);
        xSemaphoreGive(s_subkey_mutex);
        return ESP_ERR_NO_MEM;
    }

    // 3. Write to NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_SUBKEYS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS subkeys namespace: %s", esp_err_to_name(err));
        xSemaphoreGive(s_subkey_mutex);
        return err;
    }

    subkey_nvs_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    memcpy(blob.mac, entry->mac, 6);
    blob.addr_type = entry->addr_type;
    strncpy(blob.device_id, entry->device_id, SUBKEY_DEVICE_ID_LEN);
    blob.device_id[SUBKEY_DEVICE_ID_LEN] = '\0';
    memcpy(blob.local_key, entry->local_key, SUBKEY_LOCAL_KEY_LEN);
    blob.key_version = entry->key_version;

    char key[13];
    mac_to_nvs_key(entry->mac, key, sizeof(key));
    err = nvs_set_blob(handle, key, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write subkey blob for %s: %s", key, esp_err_to_name(err));
        nvs_close(handle);
        xSemaphoreGive(s_subkey_mutex);
        return err;
    }

    // Update index
    subkey_nvs_idx_t idx;
    size_t idx_size = sizeof(idx);
    memset(&idx, 0, sizeof(idx));
    esp_err_t get_idx_err = nvs_get_blob(handle, NVS_SUBKEYS_IDX, &idx, &idx_size);
    if (get_idx_err != ESP_OK) {
        idx.count = 0;
    }

    bool in_idx = false;
    for (uint8_t i = 0; i < idx.count; i++) {
        if (memcmp(idx.macs[i], entry->mac, 6) == 0) {
            in_idx = true;
            break;
        }
    }
    if (!in_idx && idx.count < SUBKEY_MAX_DEVICES) {
        memcpy(idx.macs[idx.count], entry->mac, 6);
        idx.count++;
        nvs_set_blob(handle, NVS_SUBKEYS_IDX, &idx, sizeof(idx));
    }

    nvs_commit(handle);
    nvs_close(handle);

    // 4. Update RAM cache
    memcpy(&s_subkey_cache[target_idx], entry, sizeof(subkey_entry_t));
    s_subkey_cache[target_idx].in_use = true;

    ESP_LOGI(TAG, "Subkey stored for Device '%s' (MAC %02x:%02x:%02x:%02x:%02x:%02x, ver=%lu)",
             entry->device_id,
             entry->mac[0], entry->mac[1], entry->mac[2], entry->mac[3], entry->mac[4], entry->mac[5],
             (unsigned long)entry->key_version);

    xSemaphoreGive(s_subkey_mutex);
    return ESP_OK;
}

esp_err_t subkey_delete_by_mac(const uint8_t *mac)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (!s_subkey_initialized) {
        subkey_init();
    }

    xSemaphoreTake(s_subkey_mutex, portMAX_DELAY);

    int target_idx = -1;
    for (int i = 0; i < SUBKEY_MAX_DEVICES; i++) {
        if (s_subkey_cache[i].in_use && memcmp(s_subkey_cache[i].mac, mac, 6) == 0) {
            target_idx = i;
            break;
        }
    }

    if (target_idx < 0) {
        xSemaphoreGive(s_subkey_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_SUBKEYS_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        char key[13];
        mac_to_nvs_key(mac, key, sizeof(key));
        nvs_erase_key(handle, key);

        subkey_nvs_idx_t idx;
        size_t idx_size = sizeof(idx);
        memset(&idx, 0, sizeof(idx));
        if (nvs_get_blob(handle, NVS_SUBKEYS_IDX, &idx, &idx_size) == ESP_OK) {
            uint8_t new_count = 0;
            uint8_t new_macs[SUBKEY_MAX_DEVICES][6];
            for (uint8_t i = 0; i < idx.count; i++) {
                if (memcmp(idx.macs[i], mac, 6) != 0) {
                    memcpy(new_macs[new_count++], idx.macs[i], 6);
                }
            }
            idx.count = new_count;
            memcpy(idx.macs, new_macs, sizeof(new_macs));
            nvs_set_blob(handle, NVS_SUBKEYS_IDX, &idx, sizeof(idx));
        }
        nvs_commit(handle);
        nvs_close(handle);
    }

    memset(&s_subkey_cache[target_idx], 0, sizeof(subkey_entry_t));
    s_subkey_cache[target_idx].in_use = false;

    ESP_LOGI(TAG, "Subkey deleted for MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    xSemaphoreGive(s_subkey_mutex);
    return ESP_OK;
}

esp_err_t subkey_delete_by_device_id(const char *device_id)
{
    if (!device_id) return ESP_ERR_INVALID_ARG;
    subkey_entry_t entry;
    if (subkey_get_by_device_id(device_id, &entry) == ESP_OK) {
        return subkey_delete_by_mac(entry.mac);
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t subkey_get_all(subkey_entry_t *entries, size_t max_entries, size_t *out_count)
{
    if (!entries || !out_count || max_entries == 0) return ESP_ERR_INVALID_ARG;
    if (!s_subkey_initialized) {
        subkey_init();
    }

    xSemaphoreTake(s_subkey_mutex, portMAX_DELAY);
    size_t count = 0;
    for (int i = 0; i < SUBKEY_MAX_DEVICES && count < max_entries; i++) {
        if (s_subkey_cache[i].in_use) {
            memcpy(&entries[count], &s_subkey_cache[i], sizeof(subkey_entry_t));
            count++;
        }
    }
    *out_count = count;
    xSemaphoreGive(s_subkey_mutex);
    return ESP_OK;
}

bool subkey_is_claimed(const uint8_t *mac)
{
    if (!mac) return false;
    subkey_entry_t entry;
    return (subkey_get_by_mac(mac, &entry) == ESP_OK);
}

// ==========================================
// Gateway Global Config Management
// ==========================================

esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing NVS Flash...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS Flash needs format. Erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS Flash initialized successfully.");
        // Initialize subkey store automatically
        subkey_init();
    } else {
        ESP_LOGE(TAG, "Failed to initialize NVS Flash: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t config_manager_load(gateway_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    ESP_LOGI(TAG, "Loading configuration from NVS...");
    memset(config, 0, sizeof(gateway_config_t));

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS namespace (might be empty/first boot): %s", esp_err_to_name(err));
        config->is_configured = false;
        return err;
    }

    size_t required_size = sizeof(gateway_config_t);
    err = nvs_get_blob(handle, NVS_KEY, config, &required_size);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuration loaded successfully:");
        ESP_LOGI(TAG, "  - Configured: %s", config->is_configured ? "Yes" : "No");
        ESP_LOGI(TAG, "  - Wi-Fi SSID: %s", config->wifi_ssid);
        ESP_LOGI(TAG, "  - MQTT URI  : %s", config->mqtt_uri);
        ESP_LOGI(TAG, "  - MQTT Port : %d", config->mqtt_port);
        ESP_LOGI(TAG, "  - Device ID : %s", config->device_id);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Configuration blob not found in NVS (first run).");
        config->is_configured = false;
    } else {
        ESP_LOGE(TAG, "Error reading configuration blob: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_save(const gateway_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    ESP_LOGI(TAG, "Saving configuration to NVS...");
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS namespace for writing: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY, config, sizeof(gateway_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Configuration saved and committed successfully.");
        } else {
            ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Failed to write NVS config blob: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t config_manager_reset(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    ESP_LOGW(TAG, "Resetting/Erasing gateway configuration in NVS...");
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_key(handle, NVS_KEY);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Configuration reset successfully.");
        } else {
            ESP_LOGE(TAG, "Failed to commit configuration erase: %s", esp_err_to_name(err));
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No configuration found to erase.");
        err = ESP_OK; // Safe to ignore
    } else {
        ESP_LOGE(TAG, "Failed to erase configuration key: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}
