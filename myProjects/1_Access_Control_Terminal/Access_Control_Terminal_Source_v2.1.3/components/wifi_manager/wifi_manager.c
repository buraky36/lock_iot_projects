#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>
#include "../nv_storage/include/nv_storage.h"
#include "../api_client/include/api_client.h"
#include "../ble_prov/include/ble_prov.h"

static const char *TAG = "WIFI_MGR";

#define MAX_PROV_WIFI_RETRIES 10

static bool s_connected = false;
static bool s_scan_requested = false;
static bool s_scan_pending_start = false;
static bool s_scan_completed = false;
static int64_t s_scan_req_time_ms = 0;
static bool s_pending_get_time = false;
static bool s_pending_ip_action = false;
static bool s_provisioning_connecting = false;
static int s_prov_retry_count = 0;
static bool s_pending_prov_abort = false;
static int64_t s_reconnect_retry_time_ms = 0;
static uint16_t s_ap_count = 0;
static wifi_ap_record_t s_ap_records[20]; // Max 20 APs to save RAM

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "STA Started");
            esp_wifi_set_ps(WIFI_PS_NONE);
            sys_config_t sys_cfg;
            nv_storage_get_config(&sys_cfg);
            wifi_config_t conf;
            if (sys_cfg.is_online && esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && strlen((char*)conf.sta.ssid) > 0) {
                conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
                conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
                esp_wifi_set_config(WIFI_IF_STA, &conf);
                ESP_LOGI(TAG, "Found saved Wi-Fi credentials. Trying to connect to: %s", conf.sta.ssid);
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "Setup mode or no saved Wi-Fi credentials (is_online=%d). Waiting for BLE Provisioning...", sys_cfg.is_online);
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
            int reason = dis ? (int)dis->reason : -1;
            s_connected = false;

            if (s_pending_prov_abort) {
                ESP_LOGI(TAG, "STA disconnected during abort/cancel (reason=%d).", reason);
                return;
            }

            if (s_provisioning_connecting) {
                s_prov_retry_count++;
                if (s_prov_retry_count >= MAX_PROV_WIFI_RETRIES) {
                    ESP_LOGW(TAG, "Provisioning Wi-Fi failed %d/%d times (reason=%d). Returning to idle BLE setup state.",
                             s_prov_retry_count, MAX_PROV_WIFI_RETRIES, reason);
                    s_provisioning_connecting = false;
                    s_reconnect_retry_time_ms = 0;
                    s_pending_prov_abort = true;
                } else {
                    ESP_LOGW(TAG, "Provisioning connect failed (attempt %d/%d, reason=%d). Retrying in 1s...",
                             s_prov_retry_count, MAX_PROV_WIFI_RETRIES, reason);
                    s_reconnect_retry_time_ms = (esp_timer_get_time() / 1000) + 1000;
                }
            } else {
                sys_config_t sys_cfg;
                nv_storage_get_config(&sys_cfg);
                if (sys_cfg.is_online) {
                    ESP_LOGW(TAG, "Disconnected from AP (reason=%d). Retrying connection in background (1s backoff)...", reason);
                    s_reconnect_retry_time_ms = (esp_timer_get_time() / 1000) + 1000;
                } else {
                    ESP_LOGI(TAG, "STA disconnected in BLE setup idle (reason=%d).", reason);
                    s_reconnect_retry_time_ms = 0;
                }
            }
        } else if (event_id == WIFI_EVENT_SCAN_DONE) {
            uint16_t number = 20;
            esp_wifi_scan_get_ap_num(&s_ap_count);
            esp_wifi_scan_get_ap_records(&number, s_ap_records);
            s_ap_count = (s_ap_count > 20) ? 20 : s_ap_count;
            s_scan_completed = true;
            s_scan_requested = false;
            s_scan_pending_start = false;
            ESP_LOGI(TAG, "Scan done, %d APs found", s_ap_count);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_provisioning_connecting = false;
        s_prov_retry_count = 0;
        s_pending_prov_abort = false;
        s_reconnect_retry_time_ms = 0;
        // Defer NVS write, BLE adv update, and MQTT reload to wifi_manager_tick (app_main)
        // to strictly protect sys_evt task from stack overflow!
        s_pending_ip_action = true;
    }
}

void wifi_manager_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    // esp_event_loop_create_default() should be called in main! 
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    esp_wifi_set_max_tx_power(78); // 19.5 dBm max TX power for strong signal
    ESP_LOGI(TAG, "Wi-Fi Power Save disabled (WIFI_PS_NONE), TX Power set to max.");
}

bool wifi_manager_connect(const char *ssid, const char *pass) {
    // Stop any active scan or stale reconnect before configuring new AP
    s_scan_requested = false;
    s_scan_pending_start = false;
    s_scan_completed = false;
    (void)esp_wifi_scan_stop();

    s_provisioning_connecting = false;
    s_prov_retry_count = 0;
    s_pending_prov_abort = false;
    s_reconnect_retry_time_ms = 0;
    s_connected = false;
    (void)esp_wifi_disconnect();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_LOGI(TAG, "Connecting to SSID: %s (All-Channel Scan, max %d retries)", ssid, MAX_PROV_WIFI_RETRIES);
    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(cfg_err));
        return false;
    }

    s_provisioning_connecting = true;
    s_prov_retry_count = 0;
    esp_err_t conn_err = esp_wifi_connect();
    if (conn_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect deferred (%s), scheduling retry...", esp_err_to_name(conn_err));
        s_reconnect_retry_time_ms = (esp_timer_get_time() / 1000) + 200;
    }
    return true;
}

void wifi_manager_disconnect(void) {
    s_provisioning_connecting = false;
    s_prov_retry_count = 0;
    s_reconnect_retry_time_ms = 0;
    esp_wifi_disconnect();
}

void wifi_manager_clear_credentials(void) {
    s_provisioning_connecting = false;
    s_prov_retry_count = 0;
    s_pending_prov_abort = false;
    s_reconnect_retry_time_ms = 0;
    esp_wifi_disconnect();
    esp_wifi_restore(); // Clears NVS flash for Wi-Fi configurations
    ESP_LOGI(TAG, "Wi-Fi credentials completely cleared from NVS.");
}

bool wifi_manager_is_connected(void) {
    return s_connected;
}

static esp_err_t start_non_blocking_scan(void) {
    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false
    };
    return esp_wifi_scan_start(&scan_config, false);
}

void wifi_manager_request_scan(void) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_scan_requested && !s_scan_pending_start && (now_ms - s_scan_req_time_ms) < 8000) {
        return;
    }

    // If STA is not connected yet (setup mode), cancel any pending/active connect so scan never gets blocked
    if (!s_connected) {
        bool was_connecting = s_provisioning_connecting || (s_reconnect_retry_time_ms > 0);
        s_provisioning_connecting = false;
        s_prov_retry_count = 0;
        s_reconnect_retry_time_ms = 0;
        s_pending_prov_abort = false;
        if (was_connecting) {
            ESP_LOGI(TAG, "Cancelling active Wi-Fi connect before starting scan...");
        }
        (void)esp_wifi_disconnect();
        sys_config_t sys_cfg;
        nv_storage_get_config(&sys_cfg);
        if (!sys_cfg.is_online) {
            wifi_config_t empty_cfg = {0};
            (void)esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
        }
    }

    s_scan_requested = true;
    s_scan_completed = false;
    s_scan_req_time_ms = now_ms;

    esp_err_t err = start_non_blocking_scan();
    if (err == ESP_OK) {
        s_scan_pending_start = false;
        ESP_LOGI(TAG, "Starting non-blocking Wi-Fi scan");
    } else {
        s_scan_pending_start = true;
        ESP_LOGW(TAG, "Wi-Fi scan deferred (%s), will start as soon as STA stops connecting...", esp_err_to_name(err));
    }
}

bool wifi_manager_is_scan_done(void) {
    return s_scan_completed;
}

uint16_t wifi_manager_get_ap_count(void) {
    return s_ap_count;
}

bool wifi_manager_get_ap_record(uint16_t index, uint8_t *bssid, int8_t *rssi, uint8_t *primary, char *ssid) {
    if (index >= s_ap_count) return false;
    if (bssid) memcpy(bssid, s_ap_records[index].bssid, 6);
    if (rssi) *rssi = s_ap_records[index].rssi;
    if (primary) *primary = s_ap_records[index].primary;
    if (ssid) {
        strncpy(ssid, (char*)s_ap_records[index].ssid, 32);
        ssid[32] = '\0';
    }
    return true;
}

void wifi_manager_tick(void) {
    if (s_pending_ip_action) {
        s_pending_ip_action = false;
        
        static sys_config_t config;
        nv_storage_get_config(&config);
        if (!config.is_online) {
            config.is_online = true;
            nv_storage_set_config(&config);
            ESP_LOGI(TAG, "Device is now permanently in ONLINE mode.");
        }
        
        // Safely update BLE advertisement status from app_main task (has 6KB+ stack)
        ble_prov_stop();

        // Immediately reload and start MQTT
        api_client_reload_config();
        
        // Notify backend that device is online to allow the app to add it
        s_pending_get_time = true;
    }

    if (s_pending_get_time) {
        s_pending_get_time = false;
        api_client_send_get_time();
    }
    if (s_pending_prov_abort) {
        s_pending_prov_abort = false;
        s_provisioning_connecting = false;
        s_prov_retry_count = 0;
        s_reconnect_retry_time_ms = 0;
        (void)esp_wifi_disconnect();
        sys_config_t sys_cfg;
        nv_storage_get_config(&sys_cfg);
        if (!sys_cfg.is_online) {
            wifi_config_t empty_cfg = {0};
            (void)esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
            ESP_LOGW(TAG, "Provisioning aborted/exhausted. Cleared pending Wi-Fi creds; staying in BLE setup idle.");
        }
    }

    if (s_scan_pending_start) {
        static int64_t s_last_scan_retry_ms = 0;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_last_scan_retry_ms >= 50) {
            s_last_scan_retry_ms = now_ms;
            if ((now_ms - s_scan_req_time_ms) > 5000) {
                s_scan_pending_start = false;
                s_scan_requested = false;
                s_scan_completed = true;
                s_ap_count = 0;
                ESP_LOGW(TAG, "Wi-Fi scan start timed out; returning 0 APs.");
            } else {
                esp_err_t err = start_non_blocking_scan();
                if (err == ESP_OK) {
                    s_scan_pending_start = false;
                    ESP_LOGI(TAG, "Deferred non-blocking Wi-Fi scan started.");
                }
            }
        }
    }

    if (!s_connected && s_reconnect_retry_time_ms > 0 && !s_scan_requested) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= s_reconnect_retry_time_ms) {
            s_reconnect_retry_time_ms = 0;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect retry failed (%s), backoff 500ms", esp_err_to_name(err));
                s_reconnect_retry_time_ms = now_ms + 500;
            }
        }
    }
}
