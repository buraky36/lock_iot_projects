#include "ble_server.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_frame_parser.h"
#include "crypto_manager.h"
#include "config_manager.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "cJSON.h"
#include "gateway_version.h"
#include "wifi_manager.h"

static const char *TAG = "BLE_SERVER";
static uint8_t g_session_key[CRYPTO_AES_KEY_SIZE];

// 6E400101-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x01, 0x40, 0x6e);

// RX: 6E400102-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x01, 0x40, 0x6e);

// TX: 6E400103-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x01, 0x40, 0x6e);

static uint16_t tx_handle;
static uint16_t conn_handle;
static bool s_is_claimed = false;
static bool s_is_app_connected = false;

bool ble_server_is_app_connected(void)
{
    return s_is_app_connected;
}

void ble_server_set_claimed(bool claimed)
{
    s_is_claimed = claimed;
    ESP_LOGI(TAG, "Gateway claim state changed -> claimed=%d", claimed);
    if (claimed) {
        gateway_config_t config;
        config_manager_load(&config);
        if (config.is_configured) {
            ble_gap_adv_stop();
            ESP_LOGI(TAG, "Gateway fully configured: Stopped BLE peripheral advertising.");
        } else {
            ESP_LOGI(TAG, "Gateway claimed but Wi-Fi not configured. Keeping BLE active.");
        }
    } else {
        ble_server_start_adv();
    }
}

bool ble_server_is_claimed(void)
{
    return s_is_claimed;
}

static void delayed_restart_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    vTaskDelete(NULL);
}

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt,
                               void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        ESP_LOGI(TAG, "Data received on RX characteristic. Length: %d", len);
        
        uint8_t *rx_buf = malloc(len);
        if (rx_buf == NULL) return 0;
        os_mbuf_copydata(ctxt->om, 0, len, rx_buf);

        ble_frame_t rx_frame;
        if (ble_frame_parse(rx_buf, len, &rx_frame)) {
            ESP_LOGI(TAG, "Parsed Frame -> CMD: 0x%02X, TYPE: 0x%02X, SEQ: %d, LEN: %d", 
                     rx_frame.cmd, rx_frame.type, rx_frame.seq, rx_frame.len);
            
            if (rx_frame.cmd == 0x01 && rx_frame.type == 0x01) { // HANDSHAKE (Protocol v2.17)
                ESP_LOGI(TAG, "Received HANDSHAKE Request (0x01)");
                uint8_t mac[6];
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                char device_id[17] = {0};
                snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X%02X%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                
                uint8_t payload[24] = {0};
                memcpy(payload, device_id, strlen(device_id));
                memcpy(payload + 16, GATEWAY_DEVICE_CODE, strlen(GATEWAY_DEVICE_CODE));
                
                uint8_t tx_buf[64];
                uint16_t tx_len = ble_frame_build_resp(0x01, rx_frame.seq, 0x00, payload, sizeof(payload), tx_buf);
                struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                ble_gatts_notify_custom(conn_handle, tx_handle, om);
                
            } else if (rx_frame.cmd == 0x02 && rx_frame.type == 0x01) { // PROVISION (Protocol v2.17)
                ESP_LOGI(TAG, "Received PROVISION Request (0x02)");
                if (rx_frame.len == 32) {
                    extern uint8_t g_provisioning_token[32];
                    memcpy(g_provisioning_token, rx_frame.payload, 32);
                }
                uint8_t resp_payload[1] = {0x00};
                uint8_t tx_buf[64];
                uint16_t tx_len = ble_frame_build_resp(0x02, rx_frame.seq, 0x00, resp_payload, sizeof(resp_payload), tx_buf);
                struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                ble_gatts_notify_custom(conn_handle, tx_handle, om);
                
            } else if (rx_frame.cmd == 0x71 && rx_frame.type == 0x01) { // KEX_INIT REQUEST
                ESP_LOGI(TAG, "Received KEX_INIT Request from Mobile App (seq=%d)", rx_frame.seq);
                
                if (s_is_claimed) {
                    ESP_LOGW(TAG, "Rejecting KEX_INIT: Device is already claimed (0x03 ALREADY_CLAIMED)");
                    uint8_t tx_buf[64];
                    uint16_t tx_len = ble_frame_build_resp(0x71, rx_frame.seq, 0x03, NULL, 0, tx_buf);
                    struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                    ble_gatts_notify_custom(conn_handle, tx_handle, om);
                    free(rx_buf);
                    return 0;
                }
                
                // 1. Parse Mobile's Ephemeral PubKey (32 bytes)
                if (rx_frame.len >= CRYPTO_X25519_KEY_SIZE) {
                    uint8_t peer_pub_key[CRYPTO_X25519_KEY_SIZE];
                    memcpy(peer_pub_key, rx_frame.payload, CRYPTO_X25519_KEY_SIZE);
                    
                    // 2. Generate our Ephemeral Keypair
                    uint8_t my_priv_key[CRYPTO_X25519_KEY_SIZE];
                    uint8_t my_pub_key[CRYPTO_X25519_KEY_SIZE];
                    if (crypto_generate_x25519_keypair(my_priv_key, my_pub_key)) {
                        
                        // 3. Compute Shared Secret
                        uint8_t shared_secret[CRYPTO_X25519_KEY_SIZE];
                        crypto_ecdh_shared_secret(my_priv_key, peer_pub_key, shared_secret);
                        
                        // Get unique deviceId from MAC
                        uint8_t mac[6];
                        esp_read_mac(mac, ESP_MAC_WIFI_STA);
                        char device_id[17] = {0};
                        snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X%02X%02X",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

                        // Section 2 Step 3 & Mobile App requirement:
                        // salt=deviceId[16], info="onloi-gw-session-v1"
                        uint8_t device_id_16[16] = {0};
                        memcpy(device_id_16, device_id, strlen(device_id));

                        const uint8_t *info = (const uint8_t *)"onloi-gw-session-v1";
                        crypto_hkdf_sha256(shared_secret, sizeof(shared_secret), 
                                           device_id_16, 16, 
                                           info, strlen((char*)info), 
                                           g_session_key, sizeof(g_session_key));
                        ESP_LOGI(TAG, "Derived ECDH Session Key with deviceId[16]: %s", device_id);
                        
                        // 4. Build 56-byte Response Payload (Section 2 Step 3):
                        // [0..31]=ephemeralPubKey, [32..47]=deviceId (16 bytes), [48..55]=deviceCode (8 bytes)
                        uint8_t resp_payload[56] = {0};
                        memcpy(resp_payload, my_pub_key, 32);
                        memcpy(&resp_payload[32], device_id_16, 16);
                        strncpy((char *)&resp_payload[48], GATEWAY_DEVICE_CODE, 8);

                        uint8_t tx_buf[128];
                        uint16_t tx_len = ble_frame_build_resp(0x71, rx_frame.seq, 0x00, resp_payload, sizeof(resp_payload), tx_buf);
                        
                        // 5. Send via Notification (TX characteristic)
                        struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                        ble_gatts_notify_custom(conn_handle, tx_handle, om);
                        ESP_LOGI(TAG, "Sent KEX_INIT Response (total %d bytes, status=0x00 OK)", tx_len);
                    }
                }
            } else if (rx_frame.cmd == 0x72 && rx_frame.type == 0x01) { // KEY_DELIVER REQUEST
                ESP_LOGI(TAG, "Received KEY_DELIVER Request (len=%d)", rx_frame.len);
                bool dec_ok = false;
                uint8_t *plaintext = NULL;
                uint16_t plain_len = 0;

                // Case 1: Purpose byte at offset 0, followed by 12-byte IV, ciphertext, 16-byte TAG (len >= 29)
                if (rx_frame.len >= 29) {
                    uint8_t purpose = rx_frame.payload[0];
                    uint8_t *iv = rx_frame.payload + 1;
                    uint16_t cipher_len = rx_frame.len - 29;
                    uint8_t *ciphertext = rx_frame.payload + 13;
                    uint8_t *tag = rx_frame.payload + 13 + cipher_len;
                    plaintext = malloc(cipher_len + 1);
                    if (plaintext != NULL) {
                        // Try without AAD
                        if (crypto_aes_gcm_decrypt(g_session_key, iv, ciphertext, cipher_len, NULL, 0, tag, plaintext)) {
                            dec_ok = true;
                            plain_len = cipher_len;
                        } else if (crypto_aes_gcm_decrypt(g_session_key, iv, ciphertext, cipher_len, &purpose, 1, tag, plaintext)) {
                            // Try with purpose as AAD
                            dec_ok = true;
                            plain_len = cipher_len;
                        }
                    }
                }

                // Case 2: No purpose byte, payload is [12-byte IV | ciphertext | 16-byte TAG] (len >= 28)
                if (!dec_ok && rx_frame.len >= 28) {
                    if (plaintext == NULL) {
                        plaintext = malloc(rx_frame.len - 28 + 1);
                    }
                    if (plaintext != NULL) {
                        uint8_t *iv = rx_frame.payload;
                        uint16_t cipher_len = rx_frame.len - 28;
                        uint8_t *ciphertext = rx_frame.payload + 12;
                        uint8_t *tag = rx_frame.payload + 12 + cipher_len;
                        if (crypto_aes_gcm_decrypt(g_session_key, iv, ciphertext, cipher_len, NULL, 0, tag, plaintext)) {
                            dec_ok = true;
                            plain_len = cipher_len;
                        }
                    }
                }

                if (dec_ok && plaintext != NULL) {
                    plaintext[plain_len] = '\0';
                    ESP_LOGI(TAG, "KEY_DELIVER: Successfully decrypted! Plain length=%d", plain_len);

                    if (plain_len == 32) {
                        ESP_LOGI(TAG, "Received 32-byte localKey for Gateway claim!");
                        ESP_LOG_BUFFER_HEX_LEVEL(TAG, plaintext, 32, ESP_LOG_INFO);

                        gateway_config_t cfg;
                        memset(&cfg, 0, sizeof(cfg));
                        config_manager_load(&cfg);
                        memcpy(cfg.local_key, plaintext, 32);
                        cfg.is_claimed = true;
                        uint8_t mac[6];
                        esp_read_mac(mac, ESP_MAC_WIFI_STA);
                        snprintf(cfg.device_id, sizeof(cfg.device_id), "%02X%02X%02X%02X%02X%02X",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                        config_manager_save(&cfg);

                        ble_server_set_claimed(true);
                    }

                    // Check if plaintext contains Wi-Fi JSON or config
                    char wifi_ssid[33] = {0};
                    char wifi_pass[65] = {0};
                    bool got_wifi = false;

                    if (plaintext[0] == '{') {
                        cJSON *json = cJSON_Parse((char*)plaintext);
                        if (json != NULL) {
                            cJSON *s_item = cJSON_GetObjectItemCaseSensitive(json, "s");
                            cJSON *p_item = cJSON_GetObjectItemCaseSensitive(json, "p");
                            if (cJSON_IsString(s_item) && cJSON_IsString(p_item)) {
                                strncpy(wifi_ssid, s_item->valuestring, sizeof(wifi_ssid) - 1);
                                strncpy(wifi_pass, p_item->valuestring, sizeof(wifi_pass) - 1);
                                got_wifi = true;
                            }
                            cJSON_Delete(json);
                        }
                    }

                    // Response according to Section 3 Step 5: 1 byte payload: 0x00 OK
                    uint8_t resp_payload[1] = {0x00};
                    uint8_t tx_buf[64];
                    uint16_t tx_len = ble_frame_build_resp(0x72, rx_frame.seq, 0x00, resp_payload, sizeof(resp_payload), tx_buf);
                    struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                    ble_gatts_notify_custom(conn_handle, tx_handle, om);
                    ESP_LOGI(TAG, "Sent KEY_DELIVER Response (STATUS=0x00 OK, payload=0x00)");

                    if (got_wifi) {
                        ESP_LOGI(TAG, "Configuring Wi-Fi -> SSID: '%s'", wifi_ssid);
                        gateway_config_t new_cfg;
                        memset(&new_cfg, 0, sizeof(new_cfg));
                        strncpy(new_cfg.wifi_ssid, wifi_ssid, sizeof(new_cfg.wifi_ssid) - 1);
                        strncpy(new_cfg.wifi_pass, wifi_pass, sizeof(new_cfg.wifi_pass) - 1);
                        strncpy(new_cfg.mqtt_uri, "mqtts://mqtt.api.onloi.com:8883", sizeof(new_cfg.mqtt_uri) - 1);
                        new_cfg.mqtt_port = 8883; // TLS (Protocol Step 10)
                        uint8_t mac[6];
                        esp_read_mac(mac, ESP_MAC_WIFI_STA);
                        snprintf(new_cfg.device_id, sizeof(new_cfg.device_id), "%02X%02X%02X%02X%02X%02X",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                        new_cfg.is_configured = true;
                        config_manager_save(&new_cfg);
                        ESP_LOGI(TAG, "Saved config to NVS! Restarting in 2s...");
                        xTaskCreate(delayed_restart_task, "restart_task", 2048, NULL, 5, NULL);
                    }
                } else {
                    ESP_LOGE(TAG, "KEY_DELIVER Decryption Failed!");
                    uint8_t tx_buf[64];
                    uint16_t tx_len = ble_frame_build_resp(0x72, rx_frame.seq, 0x01, NULL, 0, tx_buf); // Fail (AUTH_FAILED)
                    struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                    ble_gatts_notify_custom(conn_handle, tx_handle, om);
                }
                if (plaintext) free(plaintext);
            } else if (rx_frame.cmd == 0x03 && rx_frame.type == 0x01) { // GET_SSIDS REQUEST
                ESP_LOGI(TAG, "Received GET_SSIDS (0x03) Request");
                wifi_ap_record_t *ap_records = malloc(10 * sizeof(wifi_ap_record_t));
                uint16_t ap_count = 10;
                esp_err_t scan_res = ESP_FAIL;
                if (ap_records != NULL) {
                    scan_res = wifi_manager_scan_ssids(ap_records, &ap_count);
                }
                if (scan_res != ESP_OK) {
                    ap_count = 0;
                }
                
                uint8_t *resp_payload = malloc(512);
                uint16_t pt_len = 1; // Byte 0 reserved for actual added count
                uint8_t added_count = 0;
                if (resp_payload != NULL) {
                    for (int i = 0; i < ap_count; i++) {
                        uint8_t s_len = strlen((char *)ap_records[i].ssid);
                        if (s_len == 0) continue; // Skip hidden SSIDs
                        // Prevent BLE MTU overflow: Keep total payload <= 180 bytes (frame <= 196 bytes)
                        if (pt_len + 6 + 1 + 1 + 1 + s_len > 180) {
                            ESP_LOGW(TAG, "Reached safe BLE payload limit (%d bytes), truncating AP list at %d APs", pt_len, added_count);
                            break;
                        }
                        memcpy(&resp_payload[pt_len], ap_records[i].bssid, 6);
                        pt_len += 6;
                        resp_payload[pt_len++] = (uint8_t)ap_records[i].rssi;
                        resp_payload[pt_len++] = (ap_records[i].primary <= 14) ? 0x01 : 0x02;
                        resp_payload[pt_len++] = s_len;
                        memcpy(&resp_payload[pt_len], ap_records[i].ssid, s_len);
                        pt_len += s_len;
                        added_count++;
                    }
                    resp_payload[0] = added_count; // Set final count
                    
                    uint8_t *tx_buf = malloc(pt_len + 32);
                    if (tx_buf != NULL) {
                        uint16_t tx_len = ble_frame_build_resp(0x03, rx_frame.seq, 0x00, resp_payload, pt_len, tx_buf);
                        struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                        int rc = ble_gatts_notify_custom(conn_handle, tx_handle, om);
                        if (rc != 0) {
                            ESP_LOGE(TAG, "Failed to send GET_SSIDS notify: rc=%d", rc);
                        } else {
                            ESP_LOGI(TAG, "Sent GET_SSIDS Response (count=%d, pt_len=%d, tx_len=%d)", added_count, pt_len, tx_len);
                        }
                        free(tx_buf);
                    }
                    free(resp_payload);
                }
                if (ap_records != NULL) {
                    free(ap_records);
                }
            } else if (rx_frame.cmd == 0x04 && rx_frame.type == 0x01) { // CONNECT_WIFI REQUEST
                ESP_LOGI(TAG, "Received CONNECT_WIFI (0x04) Request");
                char wifi_ssid[33] = {0};
                char wifi_pass[65] = {0};
                bool got_creds = false;
                
                if (rx_frame.len > 0) {
                    uint8_t s_len = rx_frame.payload[0];
                    if (s_len < sizeof(wifi_ssid) && rx_frame.len > 1 + s_len) {
                        memcpy(wifi_ssid, &rx_frame.payload[1], s_len);
                        
                        // Check for macAddress (6 bytes)
                        uint16_t pass_len_offset = 1 + s_len + 6;
                        if (rx_frame.len > pass_len_offset) {
                            uint8_t p_len = rx_frame.payload[pass_len_offset];
                            if (p_len < sizeof(wifi_pass) && rx_frame.len >= pass_len_offset + 1 + p_len) {
                                memcpy(wifi_pass, &rx_frame.payload[pass_len_offset + 1], p_len);
                                got_creds = true;
                            }
                        }
                    }
                }
                
                if (got_creds) {
                    ESP_LOGI(TAG, "Configuring Wi-Fi -> SSID: '%s'", wifi_ssid);
                    
                    uint8_t tx_buf[64];
                    uint8_t result = 0x00;
                    uint16_t tx_len = ble_frame_build_resp(0x04, rx_frame.seq, 0x00, &result, 1, tx_buf);
                    struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                    ble_gatts_notify_custom(conn_handle, tx_handle, om);
                    ESP_LOGI(TAG, "Sent CONNECT_WIFI Response (OK)");

                    gateway_config_t *new_cfg = malloc(sizeof(gateway_config_t));
                    if (new_cfg) {
                        memset(new_cfg, 0, sizeof(*new_cfg));
                        config_manager_load(new_cfg);
                        strncpy(new_cfg->wifi_ssid, wifi_ssid, sizeof(new_cfg->wifi_ssid) - 1);
                        strncpy(new_cfg->wifi_pass, wifi_pass, sizeof(new_cfg->wifi_pass) - 1);
                        if (strlen(new_cfg->mqtt_uri) == 0) {
                            strncpy(new_cfg->mqtt_uri, "mqtts://mqtt.api.onloi.com:8883", sizeof(new_cfg->mqtt_uri) - 1);
                            new_cfg->mqtt_port = 8883; // TLS (Protocol Step 10)
                        }
                        
                        uint8_t mac[6];
                        esp_read_mac(mac, ESP_MAC_WIFI_STA);
                        snprintf(new_cfg->device_id, sizeof(new_cfg->device_id), "%02X%02X%02X%02X%02X%02X",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                        new_cfg->is_configured = true;
                        new_cfg->is_claimed = true;

                        config_manager_save(new_cfg);
                        free(new_cfg);
                    }

                    ble_server_set_claimed(true);
                    ESP_LOGI(TAG, "Saved config to NVS! Restarting to connect Wi-Fi in 2s...");
                    xTaskCreate(delayed_restart_task, "restart_task", 2048, NULL, 5, NULL);
                } else {
                    ESP_LOGE(TAG, "CONNECT_WIFI Payload parse failed!");
                    uint8_t tx_buf[64];
                    uint8_t result = 0x01;
                    uint16_t tx_len = ble_frame_build_resp(0x04, rx_frame.seq, 0x01, &result, 1, tx_buf);
                    struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, tx_len);
                    ble_gatts_notify_custom(conn_handle, tx_handle, om);
                }
            }
        } else {
            ESP_LOGW(TAG, "Failed to parse incoming BLE frame (len=%d) (cmd=0x%02X)", len, rx_frame.cmd);
        }
        free(rx_buf);
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &gatt_rx_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &gatt_tx_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &tx_handle,
            },
            {
                0, /* No more characteristics in this service */
            }
        },
    },
    {
        0, /* No more services */
    },
};

static int ble_server_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Peripheral connected; status=%d", event->connect.status);
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            s_is_app_connected = true;
            ESP_LOGI(TAG, "App successfully connected via BLE.");
        } else {
            s_is_app_connected = false;
            if (!s_is_claimed) {
                ble_server_start_adv();
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Peripheral disconnected; reason=%d", event->disconnect.reason);
        s_is_app_connected = false;
        
        gateway_config_t config;
        config_manager_load(&config);
        if (!s_is_claimed) {
            ESP_LOGI(TAG, "Gateway is not claimed yet. Restarting BLE peripheral advertising.");
            ble_server_start_adv();
        } else {
            ESP_LOGI(TAG, "Gateway is claimed. BLE peripheral advertising remains OFF.");
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Client subscribed to TX");
        break;
    }
    return 0;
}

esp_err_t ble_server_init(void)
{
    gateway_config_t cfg;
    if (config_manager_load(&cfg) == ESP_OK) {
        if (!cfg.is_configured) {
            s_is_claimed = false;
        } else {
            s_is_claimed = cfg.is_claimed;
        }
        ESP_LOGI(TAG, "Loaded Gateway claim state: %s (configured=%d)", 
                 s_is_claimed ? "CLAIMED (1)" : "UNCLAIMED (0)", cfg.is_configured);
    }

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count GATT services; rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add GATT services; rc=%d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ble_server_start_adv(void)
{
    if (s_is_claimed) {
        ESP_LOGI(TAG, "Gateway is claimed. BLE peripheral advertising is disabled.");
        ble_gap_adv_stop();
        return;
    }

    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    int rc;

    // 1. Primary Advertising Packet (22 bytes <= 31 bytes)
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    fields.name = (uint8_t *)GATEWAY_DEVICE_CODE;
    fields.name_len = strlen(GATEWAY_DEVICE_CODE);
    fields.name_is_complete = 1;
    
    // Onloi Manufacturer Data format (Section 11.7 & 13.6):
    // [0..1]: 0x4F 0x4B ("OK" signature)
    // [2..7]: 6-byte ASCII deviceCode ("OG0610")
    // [8]   : claimed flag (0x00 = Unclaimed, 0x01 = Claimed)
    static uint8_t mfg_data[9] = {0x4F, 0x4B, 'O', 'G', '0', '6', '1', '0', 0x00};
    mfg_data[8] = s_is_claimed ? 0x01 : 0x00;
    fields.mfg_data = mfg_data;
    fields.mfg_data_len = sizeof(mfg_data);

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement data; rc=%d", rc);
        return;
    }

    // 2. Scan Response Packet (Section 2.1): includes Onloi Service UUID 6E400101-...
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = &gatt_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    rsp_fields.tx_pwr_lvl_is_present = 1;
    rsp_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "Warning setting adv rsp data; rc=%d", rc);
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    
    uint8_t own_addr_type;
    ble_hs_id_infer_auto(0, &own_addr_type);

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_server_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error enabling advertisement; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Started Advertising as Onloi Gateway Peripheral");
    }
}

void ble_server_stop_adv(void)
{
    ble_gap_adv_stop();
}
