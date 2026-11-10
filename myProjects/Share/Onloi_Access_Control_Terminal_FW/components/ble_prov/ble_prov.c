#include "ble_prov.h"
#include "esp_bt.h"
#include "../hal_io/include/hw_config.h"
#include "hal_io.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nv_storage.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "wifi_manager.h"
#include "../api_client/include/api_client.h"
#include "app_state_machine.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "BLE_PROV";
static uint8_t ble_rx_buffer[2048];
static int ble_rx_len = 0;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t char_val_handle_tx;
static uint8_t nimble_port_own_addr_type;
static char device_id_str[32] = "SMART_000000";

#define SOF_BYTE 0x4F
#define VER_BYTE 0x02

#define MSG_TYPE_REQUEST 0x01
#define MSG_TYPE_RESPONSE 0x02

#define CMD_HANDSHAKE 0x01
#define CMD_GET_SSIDS 0x03
#define CMD_CONNECT_WIFI 0x04
#define CMD_SET_CLOUD_CONFIG 0x06

static bool s_pending_scan_response = false;
static uint16_t s_pending_scan_seq = 0;
static bool s_should_stop_ble = false;
static uint32_t s_stop_ble_time = 0;
static bool s_ble_advertising_enabled = false;
static bool s_ble_initialized = false;
static int64_t s_last_ble_activity_time_ms = 0;
#define BLE_CONN_IDLE_TIMEOUT_MS 10000 // 10 seconds timeout for inactive BLE connections

// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
// 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_chr_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
// 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_chr_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t calculate_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static void send_ble_response(uint8_t cmd, uint16_t seq, uint8_t status, const uint8_t *payload, uint16_t payload_len) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "No BLE connection to notify");
        return;
    }

    if (payload_len > 490) payload_len = 490; // Protect against large payloads
    static uint8_t resp[512];
    resp[0] = SOF_BYTE;
    resp[1] = VER_BYTE;
    resp[2] = MSG_TYPE_RESPONSE;
    resp[3] = cmd;
    resp[4] = (seq >> 8) & 0xFF;
    resp[5] = seq & 0xFF;
    resp[6] = (payload_len >> 8) & 0xFF;
    resp[7] = payload_len & 0xFF;
    resp[8] = status;

    uint64_t ts = (uint64_t)(esp_timer_get_time() / 1000ULL);
    for (int i = 0; i < 8; i++) {
        resp[9 + i] = (ts >> (56 - i * 8)) & 0xFF;
    }

    if (payload_len > 0 && payload != NULL) {
        memcpy(resp + 17, payload, payload_len);
    }

    uint16_t crc = calculate_crc16(resp, 17 + payload_len);
    resp[17 + payload_len] = (crc >> 8) & 0xFF;
    resp[17 + payload_len + 1] = crc & 0xFF;

    size_t total_len = 19 + payload_len;
    const size_t MTU_CHUNK = 19;

    // ESP_LOGI(TAG, "Sending BLE Response CMD: 0x%02X, Seq: %d, TotalLen: %zu", cmd, seq, total_len); // [V2.0.6 log cleanup]

    for (size_t pos = 0; pos < total_len; pos += MTU_CHUNK) {
        size_t chunkSize = (total_len - pos < MTU_CHUNK) ? (total_len - pos) : MTU_CHUNK;
        struct os_mbuf *txom = ble_hs_mbuf_from_flat(resp + pos, chunkSize);
        ble_gatts_notify_custom(conn_handle, char_val_handle_tx, txom);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void handle_ble_command(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len) {
    s_last_ble_activity_time_ms = esp_timer_get_time() / 1000;

    static sys_config_t sys_cfg;
    nv_storage_get_config(&sys_cfg);

    // 1. Allow UNCLAIM / DISCONNECT / RESET commands even if online/claimed
    if (cmd == 0x73 || cmd == 0x59 || cmd == 0x50) {
        ESP_LOGW(TAG, "BLE Reset/Unclaim command 0x%02X received! Unpairing device...", cmd);
        uint8_t ok_res = 0x00;
        send_ble_response(cmd, seq, 0x00, &ok_res, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        
        s_ble_advertising_enabled = true;
        // Clear Wi-Fi credentials
        wifi_manager_clear_credentials();
        // Clear MQTT credentials in NVS
        nv_storage_clear_onloi_mqtt_config();
        nv_storage_clear_active_ota_job();
        // Set is_online = false (preserving local PIN/NFC users if 0x73/0x59, or wiping if 0x50)
        sys_cfg.is_online = false;
        if (cmd == 0x50) {
            app_set_state(STATE_FACTORY_RESET);
            return;
        }
        nv_storage_set_config(&sys_cfg);
        // Feedback & restart into unclaimed BLE setup mode
        hal_io_led_set(LED_COLOR_ORANGE, LED_MODE_SOLID);
        hal_io_buzzer_beep(2700, 300, 2);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    switch (cmd) {
        case CMD_HANDSHAKE: {
            uint8_t resp_payload[24] = {0};
            size_t id_len = strlen(device_id_str);
            memcpy(resp_payload, device_id_str, id_len > 16 ? 16 : id_len);
            
            size_t code_len = strlen(device_code_str);
            memcpy(resp_payload + 16, device_code_str, code_len > 8 ? 8 : code_len);
            
            send_ble_response(CMD_HANDSHAKE, seq, 0x00, resp_payload, 24);
            break;
        }
        case CMD_SET_CLOUD_CONFIG: {
            if (payload_len < 41) {
                ESP_LOGE(TAG, "SET_CLOUD_CONFIG payload too short: %d", payload_len);
                uint8_t result = 0x01;
                send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                break;
            }

            size_t off = 0;
            uint8_t flags = payload[off++];
            uint16_t port = (payload[off] << 8) | payload[off + 1];
            off += 2;

            uint8_t host_len = payload[off++];
            if (off + host_len > payload_len) {
                uint8_t result = 0x01;
                send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                break;
            }
            char host[128] = {0};
            memcpy(host, payload + off, (host_len < sizeof(host) - 1) ? host_len : sizeof(host) - 1);
            off += host_len;

            if (off >= payload_len) {
                uint8_t result = 0x01;
                send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                break;
            }
            uint8_t user_len = payload[off++];
            if (off + user_len > payload_len) {
                uint8_t result = 0x01;
                send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                break;
            }
            char username[64] = {0};
            memcpy(username, payload + off, (user_len < sizeof(username) - 1) ? user_len : sizeof(username) - 1);
            off += user_len;

            if (off >= payload_len) {
                uint8_t result = 0x01;
                send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                break;
            }
            uint8_t pass_len = payload[off++];
            if (off + pass_len > payload_len) {
                uint8_t result = 0x01;
                send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                break;
            }
            char password[64] = {0};
            memcpy(password, payload + off, (pass_len < sizeof(password) - 1) ? pass_len : sizeof(password) - 1);
            off += pass_len;

            if (off >= payload_len) {
                uint8_t result = 0x01;
                send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                break;
            }
            uint8_t client_id_len = payload[off++];
            char client_id[64] = {0};
            if (client_id_len > 0) {
                if (off + client_id_len > payload_len) {
                    uint8_t result = 0x01;
                    send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                    break;
                }
                memcpy(client_id, payload + off, (client_id_len < sizeof(client_id) - 1) ? client_id_len : sizeof(client_id) - 1);
                off += client_id_len;
            } else {
                snprintf(client_id, sizeof(client_id), "onloi_d_%s", device_id_str);
            }

            if (off + 32 + 2 > payload_len) {
                uint8_t result = 0x01;
                send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
                break;
            }
            uint8_t device_secret[32];
            memcpy(device_secret, payload + off, 32);
            off += 32;

            uint16_t session_epoch = (payload[off] << 8) | payload[off + 1];
            off += 2;

            static onloi_mqtt_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            strncpy(cfg.host, host, sizeof(cfg.host) - 1);
            cfg.port = port;
            strncpy(cfg.client_id, client_id, sizeof(cfg.client_id) - 1);
            strncpy(cfg.username, username, sizeof(cfg.username) - 1);
            strncpy(cfg.password, password, sizeof(cfg.password) - 1);
            cfg.flags = flags;
            memcpy(cfg.device_secret, device_secret, 32);
            cfg.session_epoch = session_epoch;
            cfg.is_claimed = true;

            bool ok = nv_storage_save_onloi_mqtt_config(&cfg);
            ESP_LOGI(TAG, "SET_CLOUD_CONFIG: Host=%s, Port=%d, TLS=%d, User=%s, ClientId=%s, Epoch=%d, Saved=%d",
                     cfg.host, cfg.port, (flags & 0x01), cfg.username, cfg.client_id, cfg.session_epoch, ok);

            api_client_reload_config();

            uint8_t result = ok ? 0x00 : 0x01;
            send_ble_response(CMD_SET_CLOUD_CONFIG, seq, 0x00, &result, 1);
            break;
        }
        case CMD_GET_SSIDS: {
            ESP_LOGI(TAG, "Requesting WiFi scan");
            wifi_manager_request_scan();
            s_pending_scan_seq = seq;
            s_pending_scan_response = true;
            break;
        }
        case CMD_CONNECT_WIFI: {
            if (payload_len >= 8) {
                uint8_t ssid_len = payload[0];
                if (1 + ssid_len + 6 < payload_len) {
                    char ssid[33] = {0};
                    memcpy(ssid, payload + 1, (ssid_len < 32) ? ssid_len : 32);

                    size_t pass_offset = 1 + ssid_len + 6;
                    uint8_t pass_len = payload[pass_offset];
                    char pass[65] = {0};
                    if (pass_offset + 1 + pass_len <= payload_len) {
                        memcpy(pass, payload + pass_offset + 1, (pass_len < 64) ? pass_len : 64);
                    }

                    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
                    bool success = wifi_manager_connect(ssid, pass);
                    if (success) {
                        uint8_t result = 0x00;
                        send_ble_response(CMD_CONNECT_WIFI, seq, 0x00, &result, 1);
                        // Do not terminate BLE prematurely here; wifi_manager_tick will call ble_prov_stop() once IP is acquired.
                    } else {
                        uint8_t result = 0x01;
                        send_ble_response(CMD_CONNECT_WIFI, seq, 0x00, &result, 1);
                    }
                    break;
                }
            }
            uint8_t err_res = 0x01;
            send_ble_response(CMD_CONNECT_WIFI, seq, 0x00, &err_res, 1);
            break;
        }
        default:
            break;
    }
}

static int gatts_access_rx_cb(uint16_t conn, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        s_last_ble_activity_time_ms = esp_timer_get_time() / 1000;
        uint16_t mbuf_len = OS_MBUF_PKTLEN(ctxt->om);
        if (ble_rx_len + mbuf_len > sizeof(ble_rx_buffer)) {
            ble_rx_len = 0; // Overflow, reset
        }
        os_mbuf_copydata(ctxt->om, 0, mbuf_len, ble_rx_buffer + ble_rx_len);
        ble_rx_len += mbuf_len;

        while (ble_rx_len >= 19) {
            size_t sof_idx = 0;
            while (sof_idx < ble_rx_len && ble_rx_buffer[sof_idx] != SOF_BYTE) {
                sof_idx++;
            }
            if (sof_idx > 0) {
                memmove(ble_rx_buffer, ble_rx_buffer + sof_idx, ble_rx_len - sof_idx);
                ble_rx_len -= sof_idx;
                continue;
            }

            if (ble_rx_buffer[1] != VER_BYTE) {
                memmove(ble_rx_buffer, ble_rx_buffer + 1, --ble_rx_len);
                continue;
            }

            uint16_t payload_len = (ble_rx_buffer[6] << 8) | ble_rx_buffer[7];
            size_t total_expected = 19 + payload_len;

            if (ble_rx_len < total_expected) {
                break; // Need more data
            }

            uint16_t calculated = calculate_crc16(ble_rx_buffer, 17 + payload_len);
            uint16_t received = (ble_rx_buffer[17 + payload_len] << 8) | ble_rx_buffer[17 + payload_len + 1];

            if (calculated != received) {
                ESP_LOGE(TAG, "CRC Mismatch. Calc: %04X, Rx: %04X", calculated, received);
                memmove(ble_rx_buffer, ble_rx_buffer + 1, --ble_rx_len);
                continue;
            }

            uint8_t cmd = ble_rx_buffer[3];
            uint16_t seq = (ble_rx_buffer[4] << 8) | ble_rx_buffer[5];
            uint8_t *payload = ble_rx_buffer + 17;

            handle_ble_command(cmd, seq, payload, payload_len);

            memmove(ble_rx_buffer, ble_rx_buffer + total_expected, ble_rx_len - total_expected);
            ble_rx_len -= total_expected;
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &gatt_svr_chr_rx_uuid.u,
                .access_cb = gatts_access_rx_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &gatt_svr_chr_tx_uuid.u,
                .access_cb = gatts_access_rx_cb, // MUST NOT BE NULL IN NIMBLE
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &char_val_handle_tx,
            },
            {0}
        }
    },
    {0}
};

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "Connected");
            conn_handle = event->connect.conn_handle;
            s_last_ble_activity_time_ms = esp_timer_get_time() / 1000;
            ble_rx_len = 0;
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected");
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_rx_len = 0;
            s_pending_scan_response = false;
            {
                static sys_config_t config;
                nv_storage_get_config(&config);
                if (config.is_online || !s_ble_advertising_enabled) {
                    s_ble_advertising_enabled = false;
                    ESP_LOGI(TAG, "Device is in ONLINE mode (enabled=%d). BLE advertising will NOT restart.", s_ble_advertising_enabled);
                } else {
                    static struct ble_gap_adv_params adv_params;
                    memset(&adv_params, 0, sizeof(adv_params));
                    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
                    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
                    adv_params.itvl_min = 160; // 100 ms (160 * 0.625ms) - frees 2.4GHz RF for Wi-Fi coexistence
                    adv_params.itvl_max = 240; // 150 ms (240 * 0.625ms)
                    ble_gap_adv_start(nimble_port_own_addr_type, NULL, BLE_HS_FOREVER,
                                      &adv_params, ble_gap_event, NULL);
                }
            }
            break;
    }
    return 0;
}

static void ble_app_on_sync(void) {
    ESP_LOGI(TAG, "BLE synced");
    ble_hs_id_infer_auto(0, &nimble_port_own_addr_type);

    static sys_config_t config;
    nv_storage_get_config(&config);
    if (config.is_online || !s_ble_advertising_enabled) {
        s_ble_advertising_enabled = false;
        ESP_LOGI(TAG, "Device is in ONLINE mode. BLE advertising is disabled.");
        ble_gap_adv_stop();
        return;
    }

    static struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 160; // 100 ms - frees 2.4GHz RF for Wi-Fi coexistence
    adv_params.itvl_max = 240; // 150 ms

    static struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    static ble_uuid128_t svc_uuid_copy;
    svc_uuid_copy = gatt_svr_svc_uuid;
    fields.uuids128 = &svc_uuid_copy;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    
    // Add Service Data (UUID: FFF0, Data: device_code_str e.g. "OE0113")
    static uint8_t svc_data[8];
    svc_data[0] = 0xF0; // FFF0 (Little Endian)
    svc_data[1] = 0xFF;
    memcpy(&svc_data[2], device_code_str, 6);
    fields.svc_data_uuid16 = svc_data;
    fields.svc_data_uuid16_len = 8;
    ble_gap_adv_set_fields(&fields);

    static struct ble_hs_adv_fields scanned_fields;
    memset(&scanned_fields, 0, sizeof(scanned_fields));
    scanned_fields.name = (uint8_t *)ble_name_str;
    scanned_fields.name_len = strlen(ble_name_str);
    scanned_fields.name_is_complete = 1;

    // Manufacturer Data (Section 11.7): "OK" (2B) + deviceCode (6B) + claimed (1B: 0x00=unclaimed)
    static uint8_t mfg_data[9];
    mfg_data[0] = 0x4F; // 'O'
    mfg_data[1] = 0x4B; // 'K'
    memcpy(&mfg_data[2], device_code_str, 6);
    mfg_data[8] = 0x00; // unclaimed setup mode
    scanned_fields.mfg_data = mfg_data;
    scanned_fields.mfg_data_len = 9;
    ble_gap_adv_rsp_set_fields(&scanned_fields);

    ESP_LOGI(TAG, "BLE Advertising started (unclaimed, name=%s)", ble_name_str);
    ble_gap_adv_start(nimble_port_own_addr_type, NULL, BLE_HS_FOREVER,
                      &adv_params, ble_gap_event, NULL);
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_deinit();
    s_ble_initialized = false;
    ESP_LOGI(TAG, "BLE Host Task Stopped and NimBLE RAM freed");
    nimble_port_freertos_deinit();
}

void ble_prov_init(void) {
    ESP_LOGI(TAG, "Initializing BLE Provisioning");

    static sys_config_t cfg;
    nv_storage_get_config(&cfg);
    s_ble_advertising_enabled = !cfg.is_online;
    if (cfg.is_online) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        ESP_LOGI(TAG, "Device is in ONLINE mode at boot. Released BT Controller & NimBLE (~75KB total RAM saved).");
        s_ble_initialized = false;
        return;
    }
    ESP_LOGI(TAG, "BLE initial state: is_online=%d, advertising_enabled=%d", cfg.is_online, s_ble_advertising_enabled);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    uint32_t low = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    const char *code = (strlen(device_code_str) > 0 && strcmp(device_code_str, "UNKNOWN") != 0) ? device_code_str : "OE0113";
    snprintf(device_id_str, sizeof(device_id_str), "%.8s_%06lX", code, (unsigned long)low);

    ESP_LOGI(TAG, "Using BLE Name: %s, DeviceID: %s", ble_name_str, device_id_str);

    nimble_port_init();
    
    int rc;
    ble_gatts_count_cfg(gatt_svcs);
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
    
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    
    rc = ble_svc_gap_device_name_set(ble_name_str);
    if (rc != 0) ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
    
    nimble_port_freertos_init(ble_host_task);
    s_ble_initialized = true;
}

void ble_prov_tick(void) {
    if (!s_ble_initialized) return;
    int64_t now_ms = esp_timer_get_time() / 1000;

    // Disconnect if idle for longer than BLE_CONN_IDLE_TIMEOUT_MS (10s)
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE && !s_should_stop_ble) {
        if (s_pending_scan_response) {
            // Keep timer alive while scanning Wi-Fi
            s_last_ble_activity_time_ms = now_ms;
        } else if (now_ms - s_last_ble_activity_time_ms >= BLE_CONN_IDLE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "BLE connection idle timeout (%d ms)! Disconnecting client...", BLE_CONN_IDLE_TIMEOUT_MS);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            s_last_ble_activity_time_ms = now_ms;
        }
    }

    if (s_pending_scan_response && wifi_manager_is_scan_done()) {
        uint16_t ap_count = wifi_manager_get_ap_count();
        uint8_t count = (ap_count > 10) ? 10 : ap_count;
        static uint8_t scan_payload[512];
        scan_payload[0] = 0;
        size_t offset = 1;
        uint8_t added_count = 0;

        for (int i = 0; i < count; i++) {
            uint8_t bssid[6];
            int8_t rssi;
            uint8_t channel;
            char ssid[33];
            if (wifi_manager_get_ap_record(i, bssid, &rssi, &channel, ssid)) {
                uint8_t ssid_len = strlen(ssid);
                if (offset + 9 + ssid_len > sizeof(scan_payload)) break;

                memcpy(scan_payload + offset, bssid, 6);
                offset += 6;
                scan_payload[offset++] = (uint8_t)rssi;
                scan_payload[offset++] = (channel < 15) ? 0x01 : 0x02; // 2.4 vs 5
                scan_payload[offset++] = ssid_len;
                memcpy(scan_payload + offset, ssid, ssid_len);
                offset += ssid_len;
                added_count++;
            }
        }
        scan_payload[0] = added_count;
        send_ble_response(CMD_GET_SSIDS, s_pending_scan_seq, 0x00, scan_payload, offset);
        s_pending_scan_response = false;
    }

    if (s_should_stop_ble && xTaskGetTickCount() >= s_stop_ble_time) {
        s_should_stop_ble = false;
        ESP_LOGI(TAG, "Provisioning finished, updating BLE advertisement and disconnecting setup client...");
        ble_prov_stop();
    }
}

void ble_prov_stop(void) {
    s_ble_advertising_enabled = false;
    s_should_stop_ble = false;
    if (!s_ble_initialized) {
        return;
    }
    ESP_LOGI(TAG, "Device is online. Terminating BLE setup client, stopping advertising & freeing BLE RAM...");
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();
    nimble_port_stop();
}
