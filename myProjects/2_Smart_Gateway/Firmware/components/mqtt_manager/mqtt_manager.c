#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "ble_manager.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "gateway_version.h"
#include "esp_crt_bundle.h"
#include "ble_frame_parser.h"

static const char *TAG = "MQTT_MGR";

static esp_mqtt_client_handle_t client = NULL;
static bool s_mqtt_connected = false;
static char s_device_id[32] = {0};
static uint16_t s_session_epoch = 0;
static char s_pub_scan_topic[128] = {0};
static char s_pub_status_topic[128] = {0};
static char s_pub_notify_topic[128] = {0};
static char s_pub_ota_status_topic[128] = {0};
static char s_sub_cmd_topic[128] = {0};
static char s_sub_ota_topic[128] = {0};
static char s_sub_sub_ota_topic[128] = {0};
static char s_pub_sub_ota_ack_topic[128] = {0};
static char s_sub_claim_topic[128] = {0};
static char s_pub_claim_ack_topic[128] = {0};
static char s_sub_unclaim_topic[128] = {0};
static char s_pub_unclaim_ack_topic[128] = {0};
static char s_sub_keyrot_topic[128] = {0};
static char s_pub_keyrot_ack_topic[128] = {0};

// Helper: Convert byte array to hex string
static void bytes_to_hex(const uint8_t *src, size_t src_len, char *dst)
{
    for (size_t i = 0; i < src_len; i++) {
        sprintf(dst + (i * 2), "%02X", src[i]);
    }
}

// Helper: Convert hex string to byte array
static size_t hex_to_bytes(const char *src, uint8_t *dst, size_t dst_max_len)
{
    size_t len = strlen(src);
    size_t bytes_count = 0;
    
    for (size_t i = 0; i < len && bytes_count < dst_max_len; i += 2) {
        unsigned int byte;
        sscanf(src + i, "%2x", &byte);
        dst[bytes_count++] = (uint8_t)byte;
    }
    return bytes_count;
}

// Helper: Parse colon-separated MAC string to bytes
static void mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes)
{
    unsigned int mac[6];
    sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", 
           &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
    for (int i = 0; i < 6; i++) {
        mac_bytes[i] = (uint8_t)mac[i];
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker!");
            s_mqtt_connected = true;
            
            {
                char lifecycle_topic_new[128];
                snprintf(lifecycle_topic_new, sizeof(lifecycle_topic_new), "onloi/v1/d/%s/lifecycle", s_device_id);
                
                char lifecycle_topic_old[128];
                snprintf(lifecycle_topic_old, sizeof(lifecycle_topic_old), "/gateway/%s/lifecycle", s_device_id);

                uint8_t payload[3];
                payload[0] = (s_session_epoch >> 8) & 0xFF; // sessionEpoch MSB
                payload[1] = s_session_epoch & 0xFF;        // sessionEpoch LSB
                payload[2] = 0x00; // status
                
                uint8_t frame_buf[64];
                uint16_t frame_len = ble_frame_build_event(0x34, 1, payload, 3, frame_buf); // seq=1
                
                esp_mqtt_client_publish(client, lifecycle_topic_new, (const char *)frame_buf, frame_len, 1, 1);
                esp_mqtt_client_publish(client, lifecycle_topic_old, (const char *)frame_buf, frame_len, 1, 1);
            }
            
            // Subscribe to remote command topic
            esp_mqtt_client_subscribe(client, s_sub_cmd_topic, 1);
            esp_mqtt_client_subscribe(client, s_sub_ota_topic, 1);
            esp_mqtt_client_subscribe(client, s_sub_sub_ota_topic, 1);
            esp_mqtt_client_subscribe(client, s_sub_claim_topic, 1);
            esp_mqtt_client_subscribe(client, s_sub_unclaim_topic, 1);
            esp_mqtt_client_subscribe(client, s_sub_keyrot_topic, 1);
            ESP_LOGI(TAG, "Subscribed to all Gateway topics");
            
            // Publish online status
            char status_payload[192];
            const esp_app_desc_t *app_desc = esp_app_get_description();
            snprintf(status_payload, sizeof(status_payload), 
                     "{\"status\":\"online\", \"version\":\"%s\", \"deviceCode\":\"%s\", \"localKeyVersion\":1}", 
                     app_desc->version, GATEWAY_DEVICE_CODE);
            esp_mqtt_client_publish(client, s_pub_status_topic, status_payload, 0, 1, 1); // retained
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker.");
            s_mqtt_connected = false;
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscription acknowledged (Msg ID: %d)", event->msg_id);
            break;

        case MQTT_EVENT_DATA: {
            ESP_LOGI(TAG, "MQTT incoming message received. Topic: %.*s", event->topic_len, event->topic);
            
            // Null-terminate the payload string
            char *payload = malloc(event->data_len + 1);
            if (payload == NULL) {
                ESP_LOGE(TAG, "Out of memory to allocate payload buffer.");
                break;
            }
            memcpy(payload, event->data, event->data_len);
            payload[event->data_len] = '\0';
            
            ESP_LOGI(TAG, "Payload: %s", payload);

            // Check topic
            if (strncmp(event->topic, s_sub_cmd_topic, event->topic_len) == 0) {
                cJSON *root = cJSON_Parse(payload);
                if (root != NULL) {
                    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
                    cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");
                    cJSON *cmd_item = cJSON_GetObjectItem(root, "command");
                    cJSON *srv_uuid_item = cJSON_GetObjectItem(root, "service_uuid");
                    cJSON *chr_uuid_item = cJSON_GetObjectItem(root, "char_uuid");

                    if (cJSON_IsString(mac_item) && cJSON_IsString(cmd_item) &&
                        cJSON_IsString(srv_uuid_item) && cJSON_IsString(chr_uuid_item)) {
                        ble_lock_cmd_t ble_cmd;
                        memset(&ble_cmd, 0, sizeof(ble_lock_cmd_t));

                        mac_str_to_bytes(mac_item->valuestring, ble_cmd.mac);
                        ble_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item)) ? addr_type_item->valueint : 0;
                        strncpy(ble_cmd.service_uuid, srv_uuid_item->valuestring, sizeof(ble_cmd.service_uuid) - 1);
                        strncpy(ble_cmd.char_uuid, chr_uuid_item->valuestring, sizeof(ble_cmd.char_uuid) - 1);
                        ble_cmd.cmd_len = hex_to_bytes(cmd_item->valuestring, ble_cmd.cmd_data, sizeof(ble_cmd.cmd_data));

                        ESP_LOGI(TAG, "Decoded incoming lock command");

                        // Push command request to BLE Manager queue
                        if (ble_cmd_queue != NULL) {
                            if (xQueueSend(ble_cmd_queue, &ble_cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
                                ESP_LOGE(TAG, "Failed to push command to ble_cmd_queue (queue full).");
                            }
                        }
                    } else {
                        ESP_LOGE(TAG, "Invalid fields in JSON command payload.");
                    }
                    cJSON_Delete(root);
                }
            } else if (strncmp(event->topic, s_sub_ota_topic, event->topic_len) == 0) {
                cJSON *root = cJSON_Parse(payload);
                if (root != NULL) {
                    cJSON *url_item = cJSON_GetObjectItem(root, "url");
                    cJSON *ver_item = cJSON_GetObjectItem(root, "version");
                    cJSON *sha_item = cJSON_GetObjectItem(root, "sha256");
                    cJSON *sig_item = cJSON_GetObjectItem(root, "signature");

                    if (cJSON_IsString(url_item)) {
                        ota_update_params_t ota_params;
                        memset(&ota_params, 0, sizeof(ota_params));
                        strncpy(ota_params.url, url_item->valuestring, sizeof(ota_params.url) - 1);
                        if (cJSON_IsString(ver_item)) strncpy(ota_params.version, ver_item->valuestring, sizeof(ota_params.version) - 1);
                        if (cJSON_IsString(sha_item)) strncpy(ota_params.sha256, sha_item->valuestring, sizeof(ota_params.sha256) - 1);
                        if (cJSON_IsString(sig_item)) strncpy(ota_params.signature, sig_item->valuestring, sizeof(ota_params.signature) - 1);

                        ESP_LOGI(TAG, "Received OTA update request: URL=%s, Ver=%s, SHA=%s", 
                                 ota_params.url, ota_params.version, ota_params.sha256);
                        ota_manager_start_update(&ota_params);
                    } else {
                        ESP_LOGE(TAG, "Missing 'url' field in OTA JSON payload.");
                    }
                    cJSON_Delete(root);
                }
            } else if (strncmp(event->topic, s_sub_sub_ota_topic, event->topic_len) == 0) {
                cJSON *root = cJSON_Parse(payload);
                if (root != NULL) {
                    cJSON *target_id = cJSON_GetObjectItem(root, "targetDeviceId");
                    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
                    cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");
                    cJSON *srv_uuid_item = cJSON_GetObjectItem(root, "service_uuid");
                    cJSON *chr_uuid_item = cJSON_GetObjectItem(root, "char_uuid");
                    cJSON *cmd_item = cJSON_GetObjectItem(root, "command");
                    cJSON *req_id = cJSON_GetObjectItem(root, "requestId");

                    if (cJSON_IsString(mac_item) && cJSON_IsString(cmd_item)) {
                        ble_lock_cmd_t ble_cmd;
                        memset(&ble_cmd, 0, sizeof(ble_lock_cmd_t));
                        mac_str_to_bytes(mac_item->valuestring, ble_cmd.mac);
                        ble_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item)) ? addr_type_item->valueint : 0;
                        if (cJSON_IsString(srv_uuid_item)) strncpy(ble_cmd.service_uuid, srv_uuid_item->valuestring, sizeof(ble_cmd.service_uuid) - 1);
                        if (cJSON_IsString(chr_uuid_item)) strncpy(ble_cmd.char_uuid, chr_uuid_item->valuestring, sizeof(ble_cmd.char_uuid) - 1);
                        ble_cmd.cmd_len = hex_to_bytes(cmd_item->valuestring, ble_cmd.cmd_data, sizeof(ble_cmd.cmd_data));

                        ESP_LOGI(TAG, "Relaying sub_ota command to BLE node MAC: %s", mac_item->valuestring);
                        if (ble_cmd_queue != NULL) {
                            xQueueSend(ble_cmd_queue, &ble_cmd, pdMS_TO_TICKS(1000));
                        }

                        // Send ack back to /sub_ota/ack
                        cJSON *ack = cJSON_CreateObject();
                        if (target_id && cJSON_IsString(target_id)) cJSON_AddStringToObject(ack, "targetDeviceId", target_id->valuestring);
                        if (req_id && cJSON_IsNumber(req_id)) cJSON_AddNumberToObject(ack, "requestId", req_id->valueint);
                        else if (req_id && cJSON_IsString(req_id)) cJSON_AddStringToObject(ack, "requestId", req_id->valuestring);
                        cJSON_AddStringToObject(ack, "result", "relayed");
                        char *ack_str = cJSON_PrintUnformatted(ack);
                        if (ack_str != NULL) {
                            esp_mqtt_client_publish(client, s_pub_sub_ota_ack_topic, ack_str, 0, 1, 0);
                            free(ack_str);
                        }
                        cJSON_Delete(ack);
                    }
                    cJSON_Delete(root);
                }
            } else if (strncmp(event->topic, s_sub_claim_topic, event->topic_len) == 0) {
                cJSON *root = cJSON_Parse(payload);
                if (root != NULL) {
                    cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
                    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");

                    if (cJSON_IsString(mac_item)) {
                        ESP_LOGI(TAG, "Received device_claim for MAC: %s", mac_item->valuestring);
                        
                        cJSON *ack = cJSON_CreateObject();
                        if (req_id && cJSON_IsString(req_id)) {
                            cJSON_AddStringToObject(ack, "requestId", req_id->valuestring);
                        } else if (req_id && cJSON_IsNumber(req_id)) {
                            cJSON_AddNumberToObject(ack, "requestId", req_id->valueint);
                        }
                        cJSON_AddStringToObject(ack, "mac", mac_item->valuestring);
                        
                        uint8_t target_mac[6];
                        mac_str_to_bytes(mac_item->valuestring, target_mac);
                        char target_dev_id[17];
                        snprintf(target_dev_id, sizeof(target_dev_id), "%02X%02X%02X%02X%02X%02X",
                                 target_mac[0], target_mac[1], target_mac[2], target_mac[3], target_mac[4], target_mac[5]);
                        cJSON_AddStringToObject(ack, "deviceId", target_dev_id);
                        cJSON_AddStringToObject(ack, "deviceCode", "OD0220");
                        
                        uint8_t new_local_key[32];
                        esp_fill_random(new_local_key, 32);
                        char b64_key[64];
                        size_t b64_len = 0;
                        mbedtls_base64_encode((unsigned char *)b64_key, sizeof(b64_key), &b64_len, new_local_key, 32);
                        b64_key[b64_len] = '\0';
                        
                        cJSON_AddStringToObject(ack, "localKey", b64_key);
                        cJSON_AddNumberToObject(ack, "keyVersion", 1);
                        cJSON_AddStringToObject(ack, "result", "ok");

                        char *ack_str = cJSON_PrintUnformatted(ack);
                        if (ack_str != NULL) {
                            ESP_LOGI(TAG, "[PUBLISH CLAIM ACK] %s", ack_str);
                            esp_mqtt_client_publish(client, s_pub_claim_ack_topic, ack_str, 0, 1, 0);
                            free(ack_str);
                        }
                        cJSON_Delete(ack);
                    }
                    cJSON_Delete(root);
                }
            } else if (strncmp(event->topic, s_sub_unclaim_topic, event->topic_len) == 0) {
                cJSON *root = cJSON_Parse(payload);
                if (root != NULL) {
                    cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
                    cJSON *target_id = cJSON_GetObjectItem(root, "targetDeviceId");

                    cJSON *ack = cJSON_CreateObject();
                    if (req_id && cJSON_IsString(req_id)) {
                        cJSON_AddStringToObject(ack, "requestId", req_id->valuestring);
                    } else if (req_id && cJSON_IsNumber(req_id)) {
                        cJSON_AddNumberToObject(ack, "requestId", req_id->valueint);
                    }
                    if (target_id && cJSON_IsString(target_id)) {
                        cJSON_AddStringToObject(ack, "targetDeviceId", target_id->valuestring);
                    }
                    cJSON_AddStringToObject(ack, "result", "ok");

                    char *ack_str = cJSON_PrintUnformatted(ack);
                    if (ack_str != NULL) {
                        ESP_LOGI(TAG, "[PUBLISH UNCLAIM ACK] %s", ack_str);
                        esp_mqtt_client_publish(client, s_pub_unclaim_ack_topic, ack_str, 0, 1, 0);
                        free(ack_str);
                    }
                    cJSON_Delete(ack);
                    cJSON_Delete(root);
                }
            } else if (strncmp(event->topic, s_sub_keyrot_topic, event->topic_len) == 0) {
                cJSON *root = cJSON_Parse(payload);
                if (root != NULL) {
                    cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
                    cJSON *target_id = cJSON_GetObjectItem(root, "targetDeviceId");
                    cJSON *ver_item = cJSON_GetObjectItem(root, "currentKeyVersion");

                    int next_ver = (ver_item && cJSON_IsNumber(ver_item)) ? ver_item->valueint + 1 : 2;

                    uint8_t new_local_key[32];
                    esp_fill_random(new_local_key, 32);
                    char b64_key[64];
                    size_t b64_len = 0;
                    mbedtls_base64_encode((unsigned char *)b64_key, sizeof(b64_key), &b64_len, new_local_key, 32);
                    b64_key[b64_len] = '\0';

                    cJSON *ack = cJSON_CreateObject();
                    if (req_id && cJSON_IsString(req_id)) {
                        cJSON_AddStringToObject(ack, "requestId", req_id->valuestring);
                    } else if (req_id && cJSON_IsNumber(req_id)) {
                        cJSON_AddNumberToObject(ack, "requestId", req_id->valueint);
                    }
                    if (target_id && cJSON_IsString(target_id)) {
                        cJSON_AddStringToObject(ack, "targetDeviceId", target_id->valuestring);
                    }
                    cJSON_AddStringToObject(ack, "newLocalKey", b64_key);
                    cJSON_AddNumberToObject(ack, "keyVersion", next_ver);
                    cJSON_AddStringToObject(ack, "result", "ok");

                    char *ack_str = cJSON_PrintUnformatted(ack);
                    if (ack_str != NULL) {
                        ESP_LOGI(TAG, "[PUBLISH KEYROT ACK] %s", ack_str);
                        esp_mqtt_client_publish(client, s_pub_keyrot_ack_topic, ack_str, 0, 1, 0);
                        free(ack_str);
                    }
                    cJSON_Delete(ack);
                    cJSON_Delete(root);
                }
            }
            
            free(payload);
            break;
        }
        
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "Publish complete (Msg ID: %d)", event->msg_id);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Event error occurred.");
            break;

        default:
            break;
    }
}

// Background task: reads scan queue and publishes as JSON to MQTT
static void mqtt_publisher_task(void *pvParameters)
{
    ble_scan_report_t report;
    ESP_LOGI(TAG, "MQTT Publisher Task started.");

    while (1) {
        if (xQueueReceive(ble_scan_queue, &report, portMAX_DELAY) == pdTRUE) {
            if (!s_mqtt_connected) {
                ESP_LOGD(TAG, "MQTT not connected. Discarding scanned packet.");
                continue;
            }

            // Create JSON document
            cJSON *root = cJSON_CreateObject();
            if (root == NULL) {
                ESP_LOGE(TAG, "Failed to create cJSON scan object.");
                continue;
            }

            char mac_str[18];
            sprintf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                    report.mac[0], report.mac[1], report.mac[2],
                    report.mac[3], report.mac[4], report.mac[5]);
            
            char hex_data[64] = {0};
            bytes_to_hex(report.data, report.data_len, hex_data);

            cJSON_AddStringToObject(root, "mac", mac_str);
            cJSON_AddNumberToObject(root, "rssi", report.rssi);
            cJSON_AddStringToObject(root, "data", hex_data);
            cJSON_AddBoolToObject(root, "is_onloi", report.is_onloi_beacon);
            if (report.is_onloi_beacon) {
                cJSON_AddStringToObject(root, "deviceCode", report.device_code);
                cJSON_AddBoolToObject(root, "claimed", report.claimed);
            }

            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str != NULL) {
                ESP_LOGD(TAG, "[PUBLISH] Sending scan report: %s", json_str);
                esp_mqtt_client_publish(client, s_pub_scan_topic, json_str, 0, 1, 0);
                free(json_str);
            }
            
            cJSON_Delete(root);
        }
    }
}

esp_err_t mqtt_manager_init(const char *uri, uint16_t port, const char *device_id, const char *username, const char *password, uint16_t session_epoch)
{
    ESP_LOGI(TAG, "Initializing MQTT Client...");
    
    s_session_epoch = session_epoch;
    strncpy(s_device_id, device_id, sizeof(s_device_id));
    sprintf(s_pub_scan_topic, "/gateway/%s/scan_report", s_device_id);
    sprintf(s_pub_status_topic, "/gateway/%s/status", s_device_id);
    sprintf(s_pub_notify_topic, "/gateway/%s/notify", s_device_id);
    sprintf(s_pub_ota_status_topic, "/gateway/%s/ota/status", s_device_id);
    sprintf(s_sub_cmd_topic, "/gateway/%s/command", s_device_id);
    sprintf(s_sub_ota_topic, "/gateway/%s/ota", s_device_id);
    sprintf(s_sub_sub_ota_topic, "/gateway/%s/sub_ota", s_device_id);
    sprintf(s_pub_sub_ota_ack_topic, "/gateway/%s/sub_ota/ack", s_device_id);
    sprintf(s_sub_claim_topic, "/gateway/%s/device_claim", s_device_id);
    sprintf(s_pub_claim_ack_topic, "/gateway/%s/device_claim/ack", s_device_id);
    sprintf(s_sub_unclaim_topic, "/gateway/%s/device_unclaim", s_device_id);
    sprintf(s_pub_unclaim_ack_topic, "/gateway/%s/device_unclaim/ack", s_device_id);
    sprintf(s_sub_keyrot_topic, "/gateway/%s/key_rotate", s_device_id);
    sprintf(s_pub_keyrot_ack_topic, "/gateway/%s/key_rotate/ack", s_device_id);

    // Register OTA status callback
    ota_manager_set_status_cb(mqtt_manager_publish_ota_status);

    // Manually extract hostname to avoid esp-tls URI parser bug
    static char s_mqtt_host[64] = {0};
    memset(s_mqtt_host, 0, sizeof(s_mqtt_host));
    const char *p = strstr(uri, "://");
    if (p != NULL) {
        p += 3;
        const char *colon = strchr(p, ':');
        if (colon != NULL) {
            strncpy(s_mqtt_host, p, colon - p);
        } else {
            const char *slash = strchr(p, '/');
            if (slash != NULL) {
                strncpy(s_mqtt_host, p, slash - p);
            } else {
                strncpy(s_mqtt_host, p, sizeof(s_mqtt_host) - 1);
            }
        }
    } else {
        strncpy(s_mqtt_host, uri, sizeof(s_mqtt_host) - 1);
    }
    static char s_mqtt_client_id[64] = {0};
    snprintf(s_mqtt_client_id, sizeof(s_mqtt_client_id), "gw-%s", s_device_id); // Emulate backend's expected Gateway client_id

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = s_mqtt_host,
        .broker.address.port = port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP, // TEMPORARY BYPASS: Backend misconfigured port 8883 as plain TCP!
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = "mqttuser", // Force mqttuser since backend MQTT hook expects it
        .credentials.authentication.password = password,
        .credentials.client_id = s_mqtt_client_id, // Force onloi_d_ prefix
        /*
        .session.last_will = {
            .topic = s_pub_status_topic,
            .msg = "{\"status\":\"offline\"}",
            .msg_len = 0,
            .qos = 1,
            .retain = 1
        }
        */
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize ESP MQTT Client.");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    if (client == NULL) {
        ESP_LOGE(TAG, "Cannot start MQTT client (not initialized).");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting MQTT connection to broker...");
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    // Spawn publisher background task
    xTaskCreate(&mqtt_publisher_task, "mqtt_pub_task", 4096, NULL, 5, NULL);
    
    return ESP_OK;
}

esp_err_t mqtt_manager_stop(void)
{
    if (client == NULL) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Stopping MQTT Client...");
    esp_err_t err = esp_mqtt_client_stop(client);
    if (err == ESP_OK) {
        s_mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT Client stopped.");
    }
    return err;
}

bool mqtt_manager_is_connected(void)
{
    return s_mqtt_connected;
}

void mqtt_manager_publish_notification(const uint8_t *mac, const uint8_t *data, size_t data_len)
{
    if (!s_mqtt_connected || client == NULL) return;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;

    char mac_str[18];
    sprintf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    char hex_data[128] = {0};
    size_t limit = data_len > 60 ? 60 : data_len;
    for (size_t i = 0; i < limit; i++) {
        sprintf(hex_data + (i * 2), "%02X", data[i]);
    }

    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "data", hex_data);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str != NULL) {
        ESP_LOGI(TAG, "[PUBLISH NOTIFY] %s", json_str);
        esp_mqtt_client_publish(client, s_pub_notify_topic, json_str, 0, 1, 0);
        free(json_str);
    }
    cJSON_Delete(root);
}

void mqtt_manager_publish_ota_status(const char *status, const char *reason)
{
    if (!s_mqtt_connected || client == NULL) return;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;

    cJSON_AddStringToObject(root, "status", status);
    if (reason != NULL) {
        cJSON_AddStringToObject(root, "reason", reason);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str != NULL) {
        ESP_LOGI(TAG, "[OTA STATUS] %s", json_str);
        esp_mqtt_client_publish(client, s_pub_ota_status_topic, json_str, 0, 1, 0);
        free(json_str);
    }
    cJSON_Delete(root);
}
