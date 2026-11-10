#include "nv_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "../hal_io/include/hw_config.h"

static const char *TAG = "NV_STORAGE";
static const char *NAMESPACE_CFG = "config";
static const char *NAMESPACE_USERS = "users";

static uint8_t user_active_map[1250]; // 1250 bytes * 8 bits = 10000 bits. Bit 1 represents UserID 1, etc.

static inline bool is_numeric_card_str(const char *str) {
    if (!str || *str == '\0') return false;
    for (const char *p = str; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

static void nv_storage_save_active_map(void) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "usr_map", user_active_map, sizeof(user_active_map));
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void nv_storage_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS Init failed (%s), erasing NVS and retrying...", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS Initialized");

    // Ensure default config exists
    sys_config_t conf;
    nv_storage_get_config(&conf);
    nv_storage_set_config(&conf);
    nv_storage_set_hw_version(active_hw_version);

    // Load or rebuild active users map
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(user_active_map);
        err = nvs_get_blob(handle, "usr_map", user_active_map, &size);
        nvs_close(handle);
    } else {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Active user map not found. Rebuilding from NVS... (This will take a few seconds)");
        memset(user_active_map, 0, sizeof(user_active_map));
        if (nvs_open(NAMESPACE_USERS, NVS_READONLY, &handle) == ESP_OK) {
            char key[16];
            size_t size;
            for (uint16_t i = 1; i <= 9988; i++) {
                snprintf(key, sizeof(key), "u%u", i);
                if (nvs_get_blob(handle, key, NULL, &size) == ESP_OK) {
                    user_active_map[i / 8] |= (1 << (i % 8));
                }
            }
            nvs_close(handle);
        }
        nv_storage_save_active_map();
        ESP_LOGI(TAG, "Active user map rebuilt successfully.");
    }
    
    nv_storage_init_offline_logs();
}

void nv_storage_init_offline_logs(void) {
    // No-op: offline logs not retained
}

void nv_storage_add_offline_log(offline_log_t log) {
    (void)log;
}

void nv_storage_get_offline_logs(offline_log_t *logs, uint8_t *count) {
    (void)logs;
    if (count) *count = 0;
}

void nv_storage_clear_offline_logs(uint8_t count) {
    (void)count;
}

uint8_t nv_storage_get_hw_version(void) {
    nvs_handle_t handle;
    uint8_t hw_version = 0;
    if (nvs_open(NAMESPACE_CFG, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "hw_ver", &hw_version);
        nvs_close(handle);
    }
    return hw_version;
}

void nv_storage_set_hw_version(uint8_t hw_version) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "hw_ver", hw_version);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void nv_storage_get_config(sys_config_t *config) {
    // 1. Set Factory Defaults first
    memset(config, 0, sizeof(sys_config_t));
    strcpy(config->master_code, "123456");
    config->master_card_count = 0;
    config->relay_time = 5;
    config->access_mode = 6; 
    config->alarm_time = 1;
    config->sound_enabled = true;
    config->led_always_on = true;
    config->reserved_mode = 0;
    config->reserved_door_det = 0;
    config->reserved_0 = 26;
    config->reserved_1 = 4;
    config->reserved_2 = false;

    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        size_t size = sizeof(sys_config_t);
        // Load only if size exactly matches
        if (nvs_get_blob(handle, "sys", config, &size) != ESP_OK) {
            ESP_LOGW(TAG, "Config not found in NVS, saving defaults");
            nvs_set_blob(handle, "sys", config, sizeof(sys_config_t));
            nvs_commit(handle);
        } else if (config->reserved_2 == true) {
            // One-time migration from legacy default (34-bit) to new default (26-bit) after OTA
            config->reserved_0 = 26;
            config->reserved_2 = false;
            nvs_set_blob(handle, "sys", config, sizeof(sys_config_t));
            nvs_commit(handle);
            ESP_LOGI(TAG, "Migrated NVS Wiegand/RFID default format to 26-bit");
        }
        nvs_close(handle);
    }
}

bool nv_storage_is_master_enroll_needed(void) {
    nvs_handle_t handle;
    uint8_t done = 0;
    if (nvs_open(NAMESPACE_CFG, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "mst_done", &done);
        nvs_close(handle);
    }
    return (done == 0);
}

void nv_storage_set_master_enroll_done(void) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "mst_done", 1);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void nv_storage_clear_master_enroll_done(void) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "mst_done");
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void nv_storage_set_config(sys_config_t *config) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "sys", config, sizeof(sys_config_t));
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool nv_storage_add_user(const user_record_t *user) {
    if (user->user_id == 0 || user->user_id > 9988) {
        ESP_LOGE(TAG, "Invalid user_id %d. Must be 1-9988.", user->user_id);
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_USERS, NVS_READWRITE, &handle) != ESP_OK) return false;
    
    char key[16];
    snprintf(key, sizeof(key), "u%u", user->user_id);
    
    esp_err_t err = nvs_set_blob(handle, key, user, sizeof(user_record_t));
    if (err == ESP_OK) {
        nvs_commit(handle);
        user_active_map[user->user_id / 8] |= (1 << (user->user_id % 8));
        nv_storage_save_active_map();
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool nv_storage_get_user(uint16_t user_id, user_record_t *user_out) {
    if (user_id == 0 || user_id > 9988) return false;
    if (!(user_active_map[user_id / 8] & (1 << (user_id % 8)))) return false;

    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_USERS, NVS_READONLY, &handle) != ESP_OK) return false;
    
    char key[16];
    snprintf(key, sizeof(key), "u%u", user_id);
    
    size_t size = sizeof(user_record_t);
    esp_err_t err = nvs_get_blob(handle, key, user_out, &size);
    nvs_close(handle);
    return err == ESP_OK;
}

bool nv_storage_delete_user(uint16_t user_id) {
    if (user_id == 0 || user_id > 9988) return false;
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_USERS, NVS_READWRITE, &handle) != ESP_OK) return false;
    
    char key[16];
    snprintf(key, sizeof(key), "u%u", user_id);
    
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(handle);
        user_active_map[user_id / 8] &= ~(1 << (user_id % 8));
        nv_storage_save_active_map();
        err = ESP_OK;
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool nv_storage_delete_all_users(void) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_USERS, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        nvs_commit(handle);
        memset(user_active_map, 0, sizeof(user_active_map));
        nv_storage_save_active_map();
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool nv_storage_find_user_by_pin(const char *input_pin_buffer, user_record_t *out_user) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_USERS, NVS_READONLY, &handle) != ESP_OK) return false;
    
    char key[16];
    size_t size = sizeof(user_record_t);
    bool found = false;
    
    for (uint16_t i = 1; i <= 9988; i++) {
        if (!(user_active_map[i / 8] & (1 << (i % 8)))) continue; // Skip if bit is not set

        snprintf(key, sizeof(key), "u%u", i);
        size = sizeof(user_record_t);
        if (nvs_get_blob(handle, key, out_user, &size) == ESP_OK) {
            if (out_user->is_active && strlen(out_user->pin) >= 4) {
                if (strlen(out_user->pin) == 6) {
                    // Hidden PIN feature: allow 6-digit PINs to be hidden inside up to 10 digits
                    if (strstr(input_pin_buffer, out_user->pin) != NULL) {
                        found = true;
                        break;
                    }
                } else {
                    // Exact match for 4 or 5 digit PINs
                    if (strcmp(input_pin_buffer, out_user->pin) == 0) {
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    nvs_close(handle);
    return found;
}

bool nv_storage_find_user_by_card(const char *card_id_str, user_record_t *out_user) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_USERS, NVS_READONLY, &handle) != ESP_OK) return false;
    
    char key[16];
    size_t size = sizeof(user_record_t);
    bool found = false;
    
    bool in_is_num = is_numeric_card_str(card_id_str);
    uint32_t in_val = in_is_num ? (uint32_t)strtoul(card_id_str, NULL, 10) : 0;

    for (uint16_t i = 1; i <= 9988; i++) {
        if (!(user_active_map[i / 8] & (1 << (i % 8)))) continue;

        snprintf(key, sizeof(key), "u%u", i);
        size = sizeof(user_record_t);
        if (nvs_get_blob(handle, key, out_user, &size) == ESP_OK) {
            if (out_user->is_active) {
                if (strcmp(out_user->card_id, card_id_str) == 0) {
                    found = true;
                    break;
                }
                if (in_is_num && is_numeric_card_str(out_user->card_id)) {
                    uint32_t db_val = (uint32_t)strtoul(out_user->card_id, NULL, 10);
                    if ((db_val & 0x00FFFFFF) == (in_val & 0x00FFFFFF)) {
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    nvs_close(handle);
    return found;
}

bool nv_storage_find_user_by_wiegand34(uint32_t wg34_id, user_record_t *out_user) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_USERS, NVS_READONLY, &handle) != ESP_OK) return false;
    
    char key[16];
    size_t size = sizeof(user_record_t);
    bool found = false;
    
    uint32_t wg_masked = wg34_id & 0xFF00FFFF;

    for (uint16_t i = 1; i <= 9988; i++) {
        if (!(user_active_map[i / 8] & (1 << (i % 8)))) continue;

        snprintf(key, sizeof(key), "u%u", i);
        size = sizeof(user_record_t);
        if (nvs_get_blob(handle, key, out_user, &size) == ESP_OK) {
            if (out_user->is_active && strlen(out_user->card_id) > 0) {
                uint32_t db_id = (uint32_t)strtoul(out_user->card_id, NULL, 10);
                if (db_id > 0) {
                    uint32_t db_le = ((db_id & 0xFF) << 24) | (((db_id >> 8) & 0xFF) << 16) | (((db_id >> 16) & 0xFF) << 8) | ((db_id >> 24) & 0xFF);
                    if ((db_le & 0xFF00FFFF) == wg_masked) {
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    nvs_close(handle);
    return found;
}

bool nv_storage_find_user_by_qr(const char *qr_str, user_record_t *out_user) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_USERS, NVS_READONLY, &handle) != ESP_OK) return false;
    
    char key[16];
    size_t size = sizeof(user_record_t);
    bool found = false;
    
    for (uint16_t i = 1; i <= 9988; i++) {
        if (!(user_active_map[i / 8] & (1 << (i % 8)))) continue;

        snprintf(key, sizeof(key), "u%u", i);
        size = sizeof(user_record_t);
        if (nvs_get_blob(handle, key, out_user, &size) == ESP_OK) {
            if (out_user->is_active && (strcmp(out_user->qr_id, qr_str) == 0 || strcmp(out_user->card_id, qr_str) == 0)) {
                found = true;
                break;
            }
        }
    }
    nvs_close(handle);
    return found;
}

bool nv_storage_is_master_credential(const char *cred) {
    if (cred == NULL || strlen(cred) == 0) return false;
    
    sys_config_t conf;
    nv_storage_get_config(&conf);
    
    bool in_is_num = is_numeric_card_str(cred);
    uint32_t in_val = in_is_num ? (uint32_t)strtoul(cred, NULL, 10) : 0;

    for (uint8_t i = 0; i < conf.master_card_count; i++) {
        if (strcmp(conf.master_cards[i], cred) == 0) {
            return true;
        }
        if (in_is_num && is_numeric_card_str(conf.master_cards[i])) {
            uint32_t db_val = (uint32_t)strtoul(conf.master_cards[i], NULL, 10);
            if ((db_val & 0x00FFFFFF) == (in_val & 0x00FFFFFF)) {
                return true;
            }
        }
    }
    return false;
}

bool nv_storage_find_master_by_wiegand34(uint32_t wg34_id, char *out_master) {
    sys_config_t conf;
    nv_storage_get_config(&conf);
    
    uint32_t wg_masked = wg34_id & 0xFF00FFFF;

    for (uint8_t i = 0; i < conf.master_card_count; i++) {
        if (strlen(conf.master_cards[i]) > 0) {
            uint32_t db_id = (uint32_t)strtoul(conf.master_cards[i], NULL, 10);
            if (db_id > 0) {
                uint32_t db_le = ((db_id & 0xFF) << 24) | (((db_id >> 8) & 0xFF) << 16) | (((db_id >> 16) & 0xFF) << 8) | ((db_id >> 24) & 0xFF);
                if ((db_le & 0xFF00FFFF) == wg_masked) {
                    if (out_master) {
                        strcpy(out_master, conf.master_cards[i]);
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

int32_t nv_storage_get_master_id(const char *cred) {
    if (cred == NULL || strlen(cred) == 0) return -1;
    
    sys_config_t conf;
    nv_storage_get_config(&conf);
    
    bool in_is_num = is_numeric_card_str(cred);
    uint32_t in_val = in_is_num ? (uint32_t)strtoul(cred, NULL, 10) : 0;

    for (uint8_t i = 0; i < conf.master_card_count; i++) {
        if (strcmp(conf.master_cards[i], cred) == 0) {
            return conf.master_ids[i];
        }
        if (in_is_num && is_numeric_card_str(conf.master_cards[i])) {
            uint32_t db_val = (uint32_t)strtoul(conf.master_cards[i], NULL, 10);
            if ((db_val & 0x00FFFFFF) == (in_val & 0x00FFFFFF)) {
                return conf.master_ids[i];
            }
        }
    }
    return -1;
}

bool nv_storage_add_master_credential(uint16_t id, const char *cred) {
    if (cred == NULL || strlen(cred) == 0) return false;

    sys_config_t conf;
    nv_storage_get_config(&conf);

    for (uint8_t i = 0; i < conf.master_card_count; i++) {
        if (strcmp(conf.master_cards[i], cred) == 0) {
            return true;
        }
    }

    if (conf.master_card_count >= 10) return false;

    uint16_t new_id = id;
    if (new_id == 0) {
        new_id = 10000;
        bool found = true;
        while (found && new_id <= 10010) {
            found = false;
            for (uint8_t i = 0; i < conf.master_card_count; i++) {
                if (conf.master_ids[i] == new_id) {
                    found = true;
                    new_id++;
                    break;
                }
            }
        }
        if (new_id > 10010) return false;
    }

    strncpy(conf.master_cards[conf.master_card_count], cred, sizeof(conf.master_cards[0]) - 1);
    conf.master_cards[conf.master_card_count][sizeof(conf.master_cards[0]) - 1] = '\0';
    conf.master_ids[conf.master_card_count] = new_id;
    conf.master_card_count++;

    nv_storage_set_config(&conf);
    return true;
}

bool nv_storage_delete_master_credential(const char *cred) {
    if (cred == NULL || strlen(cred) == 0) return false;

    sys_config_t conf;
    nv_storage_get_config(&conf);

    int found_idx = -1;
    for (uint8_t i = 0; i < conf.master_card_count; i++) {
        if (strcmp(conf.master_cards[i], cred) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx == -1 && is_numeric_card_str(cred)) {
        uint32_t in_val = (uint32_t)strtoul(cred, NULL, 10);
        for (uint8_t i = 0; i < conf.master_card_count; i++) {
            if (is_numeric_card_str(conf.master_cards[i])) {
                uint32_t db_val = (uint32_t)strtoul(conf.master_cards[i], NULL, 10);
                if ((db_val & 0x00FFFFFF) == (in_val & 0x00FFFFFF)) {
                    found_idx = i;
                    break;
                }
            }
        }
    }

    if (found_idx == -1) {
        uint32_t wg_id = (uint32_t)strtoul(cred, NULL, 10);
        if (wg_id > 0) {
            char matched_master[32];
            if (nv_storage_find_master_by_wiegand34(wg_id, matched_master)) {
                for (uint8_t i = 0; i < conf.master_card_count; i++) {
                    if (strcmp(conf.master_cards[i], matched_master) == 0) {
                        found_idx = i;
                        break;
                    }
                }
            }
        }
    }

    if (found_idx == -1) return false;

    for (uint8_t i = (uint8_t)found_idx; i < conf.master_card_count - 1; i++) {
        strncpy(conf.master_cards[i], conf.master_cards[i + 1], sizeof(conf.master_cards[0]));
        conf.master_ids[i] = conf.master_ids[i + 1];
    }
    memset(conf.master_cards[conf.master_card_count - 1], 0, sizeof(conf.master_cards[0]));
    conf.master_ids[conf.master_card_count - 1] = 0;
    conf.master_card_count--;

    nv_storage_set_config(&conf);
    return true;
}

bool nv_storage_delete_master_by_id(uint16_t id) {
    if (id < 10000 || id > 10010) return false;

    sys_config_t conf;
    nv_storage_get_config(&conf);

    int found_idx = -1;
    for (uint8_t i = 0; i < conf.master_card_count; i++) {
        if (conf.master_ids[i] == id) {
            found_idx = i;
            break;
        }
    }

    if (found_idx == -1) return false;

    for (uint8_t i = (uint8_t)found_idx; i < conf.master_card_count - 1; i++) {
        strncpy(conf.master_cards[i], conf.master_cards[i + 1], sizeof(conf.master_cards[0]));
        conf.master_ids[i] = conf.master_ids[i + 1];
    }
    memset(conf.master_cards[conf.master_card_count - 1], 0, sizeof(conf.master_cards[0]));
    conf.master_ids[conf.master_card_count - 1] = 0;
    conf.master_card_count--;

    nv_storage_set_config(&conf);
    return true;
}

void nv_storage_clear_master_credentials(void) {
    sys_config_t conf;
    nv_storage_get_config(&conf);
    conf.master_card_count = 0;
    memset(conf.master_cards, 0, sizeof(conf.master_cards));
    memset(conf.master_ids, 0, sizeof(conf.master_ids));
    nv_storage_set_config(&conf);
}

bool nv_storage_save_onloi_mqtt_config(const onloi_mqtt_config_t *config) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) != ESP_OK) return false;
    nvs_set_blob(handle, "mqtt_cfg", config, sizeof(onloi_mqtt_config_t));
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool nv_storage_get_onloi_mqtt_config(onloi_mqtt_config_t *config) {
    if (!config) return false;
    memset(config, 0, sizeof(onloi_mqtt_config_t));
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(onloi_mqtt_config_t);
    esp_err_t err = nvs_get_blob(handle, "mqtt_cfg", config, &size);
    nvs_close(handle);
    if (err == ESP_OK && size > 0) {
        return true;
    }
    return false;
}

void nv_storage_clear_onloi_mqtt_config(void) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "mqtt_cfg");
        nvs_commit(handle);
        nvs_close(handle);
    }
}

uint16_t nv_storage_get_free_user_id(void) {
    for (uint16_t i = 1; i <= 9988; i++) {
        if (!(user_active_map[i / 8] & (1 << (i % 8)))) {
            return i;
        }
    }
    return 0; // Full
}

void nv_storage_get_osdp_config(osdp_config_t *cfg) {
    if (!cfg) return;
    cfg->address = 0x00;
    cfg->baudrate = 9600;
    cfg->enabled = true;

    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t addr = 0;
        if (nvs_get_u8(handle, "osdp_addr", &addr) == ESP_OK) {
            cfg->address = addr;
        }
        uint32_t baud = 0;
        if (nvs_get_u32(handle, "osdp_baud", &baud) == ESP_OK && baud > 0) {
            cfg->baudrate = baud;
        }
        uint8_t en = 1;
        if (nvs_get_u8(handle, "osdp_en", &en) == ESP_OK) {
            cfg->enabled = (en != 0);
        }
        nvs_close(handle);
    }
}

void nv_storage_set_osdp_config(const osdp_config_t *cfg) {
    if (!cfg) return;
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "osdp_addr", cfg->address);
        nvs_set_u32(handle, "osdp_baud", cfg->baudrate);
        nvs_set_u8(handle, "osdp_en", cfg->enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static uint32_t calculate_ota_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

bool nv_storage_save_active_ota_job(uint32_t request_id, uint8_t target, uint32_t target_version, const char *active_dev_id) {
    if (request_id == 0 || target_version == 0) return false;

    ota_job_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = OTA_JOB_MAGIC;
    record.request_id = request_id;
    record.target = target;
    record.target_version = target_version;
    if (active_dev_id) {
        strncpy(record.active_dev_id, active_dev_id, sizeof(record.active_dev_id) - 1);
    }
    record.crc32 = calculate_ota_crc32((const uint8_t *)&record, offsetof(ota_job_record_t, crc32));

    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, "ota_job", &record, sizeof(record));
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved active OTA job: Req=0x%08lX, Target=0x%02X, Ver=0x%08lX, DevId=%s",
             (unsigned long)request_id, target, (unsigned long)target_version, record.active_dev_id);
    return err == ESP_OK;
}

bool nv_storage_get_active_ota_job(ota_job_record_t *job) {
    if (!job) return false;
    memset(job, 0, sizeof(ota_job_record_t));

    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t size = sizeof(ota_job_record_t);
    esp_err_t err = nvs_get_blob(handle, "ota_job", job, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(ota_job_record_t)) {
        // Key does not exist: normal boot or fresh out-of-factory device
        return false;
    }

    // Strict validation to prevent false recovery triggers on fresh factory devices or uninitialized flash:
    if (job->magic != OTA_JOB_MAGIC) {
        ESP_LOGW(TAG, "OTA job magic mismatch: 0x%08lX (expected 0x%08lX)", (unsigned long)job->magic, (unsigned long)OTA_JOB_MAGIC);
        return false;
    }
    if (job->request_id == 0 || job->target_version == 0) {
        return false;
    }
    uint32_t calc_crc = calculate_ota_crc32((const uint8_t *)job, offsetof(ota_job_record_t, crc32));
    if (job->crc32 != calc_crc) {
        ESP_LOGW(TAG, "OTA job CRC32 mismatch: 0x%08lX != 0x%08lX", (unsigned long)job->crc32, (unsigned long)calc_crc);
        return false;
    }

    return true;
}

void nv_storage_clear_active_ota_job(void) {
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "ota_job");
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Cleared active OTA job from NVS");
    }
}

