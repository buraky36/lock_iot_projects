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

static bool s_connected = false;
static bool s_scan_requested = false;
static bool s_scan_completed = false;
static bool s_pending_get_time = false;
static bool s_pending_ip_action = false;
static int64_t s_reconnect_retry_time_ms = 0;
static uint16_t s_ap_count = 0;
static wifi_ap_record_t s_ap_records[20]; // Max 20 APs to save RAM

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "STA Started");
            esp_wifi_set_ps(WIFI_PS_NONE);
            wifi_config_t conf;
            if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && strlen((char*)conf.sta.ssid) > 0) {
                conf.sta.scan_method = WIFI_FAST_SCAN;
                conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
                esp_wifi_set_config(WIFI_IF_STA, &conf);
                ESP_LOGI(TAG, "Found saved Wi-Fi credentials. Trying to connect to: %s (Fast Scan)", conf.sta.ssid);
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "No saved Wi-Fi credentials found. Waiting for BLE Provisioning...");
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            ESP_LOGW(TAG, "Disconnected from AP. Retrying connection in background (1s backoff)...");
            s_reconnect_retry_time_ms = (esp_timer_get_time() / 1000) + 1000;
        } else if (event_id == WIFI_EVENT_SCAN_DONE) {
            uint16_t number = 20;
            esp_wifi_scan_get_ap_num(&s_ap_count);
            esp_wifi_scan_get_ap_records(&number, s_ap_records);
            s_ap_count = (s_ap_count > 20) ? 20 : s_ap_count;
            s_scan_completed = true;
            s_scan_requested = false;
            ESP_LOGI(TAG, "Scan done, %d APs found", s_ap_count);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
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
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_LOGI(TAG, "Connecting to %s (Fast Scan)", ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
    return true;
}

void wifi_manager_disconnect(void) {
    esp_wifi_disconnect();
}

void wifi_manager_clear_credentials(void) {
    esp_wifi_disconnect();
    esp_wifi_restore(); // Clears NVS flash for Wi-Fi configurations
    ESP_LOGI(TAG, "Wi-Fi credentials completely cleared from NVS.");
}

bool wifi_manager_is_connected(void) {
    return s_connected;
}

void wifi_manager_request_scan(void) {
    if (s_scan_requested) return;
    s_scan_requested = true;
    s_scan_completed = false;
    
    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false
    };
    esp_wifi_scan_start(&scan_config, false); // false = non-blocking
    ESP_LOGI(TAG, "Starting non-blocking Wi-Fi scan");
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
    if (!s_connected && s_reconnect_retry_time_ms > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= s_reconnect_retry_time_ms) {
            s_reconnect_retry_time_ms = 0;
            esp_wifi_connect();
        }
    }
}
