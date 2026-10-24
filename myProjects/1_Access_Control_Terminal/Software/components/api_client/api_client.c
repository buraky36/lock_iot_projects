#include "api_client.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nv_storage.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "wifi_manager.h"
#include "hw_config.h"
#include "../app_state_machine/include/app_state_machine.h"
#include "ota_manager.h"
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "API_CLIENT";

#define SOF_BYTE 0x4F
#define VER_BYTE 0x02

#define MSG_TYPE_REQUEST 0x01
#define MSG_TYPE_RESPONSE 0x02
#define MSG_TYPE_EVENT 0x03
#define MSG_TYPE_COMMAND 0x04
#define MSG_TYPE_ACK 0x05

#define CMD_TIME_SYNC 0x33
#define CMD_REMOTE_OPEN 0x40
#define CMD_FACTORY_RESET 0x50
#define CMD_WIFI_RESET 0x58
#define CMD_DISCONNECTED 0x59
#define CMD_ACK 0x05
#define CMD_PIN_ENTERED 0x12
#define CMD_NFC_SCANNED 0x13
#define CMD_QR_SCANNED 0x14
#define CMD_LOCK_OPENED 0x10
#define CMD_SESSION_START 0x30
#define CMD_REGISTRATION 0x34

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static uint16_t mqtt_seq = 0;
static char device_id_str[32] = {0};
static onloi_mqtt_config_t s_mqtt_cfg;
static uint64_t time_offset_ms = 0;
static uint32_t current_enroll_req_id = 0;

static char topic_cmd[128];
static char topic_reply[128];
static char topic_config[128];
static char topic_lifecycle[128];
static char topic_event[128];
static char topic_ack[128];

static void generate_device_id(void) {
    if (strlen(device_id_str) == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        uint32_t low = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
        snprintf(device_id_str, sizeof(device_id_str), "SMART_%06lX", (unsigned long)low);
    }
}

static const char* get_cloud_device_id(void) {
    if (strncmp(s_mqtt_cfg.client_id, "onloi_d_", 8) == 0 && strlen(s_mqtt_cfg.client_id) > 8) {
        return s_mqtt_cfg.client_id + 8;
    }
    if (strlen(s_mqtt_cfg.username) > 0 && strcmp(s_mqtt_cfg.username, "admin") != 0) {
        return s_mqtt_cfg.username;
    }
    if (strlen(s_mqtt_cfg.client_id) > 0) {
        return s_mqtt_cfg.client_id;
    }
    return device_id_str;
}

static uint16_t calculate_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

static void publish_mqtt_frame(uint8_t msg_type, uint8_t cmd, const uint8_t *payload, uint16_t payload_len, const char *topic, bool retain) {
    if (!mqtt_connected || mqtt_client == NULL) return;

    uint8_t frame[512];
    frame[0] = SOF_BYTE;
    frame[1] = VER_BYTE;
    frame[2] = msg_type;
    frame[3] = cmd;

    uint16_t seq = mqtt_seq++;
    frame[4] = (seq >> 8) & 0xFF;
    frame[5] = seq & 0xFF;
    frame[6] = (payload_len >> 8) & 0xFF;
    frame[7] = payload_len & 0xFF;
    frame[8] = 0x00; // STATUS

    uint64_t ts = 0;
    if (time_offset_ms != 0) {
        // Backend expects Local Time (UTC+3) instead of standard UTC 0
        ts = (uint64_t)(esp_timer_get_time() / 1000ULL) + time_offset_ms + 10800000ULL;
    }
    for (int i = 0; i < 8; i++) {
        frame[9 + i] = (ts >> (56 - i * 8)) & 0xFF;
    }

    if (payload_len > 0 && payload != NULL) {
        memcpy(frame + 17, payload, payload_len);
    }

    uint16_t crc = calculate_crc16(frame, 17 + payload_len);
    frame[17 + payload_len] = (crc >> 8) & 0xFF;
    frame[17 + payload_len + 1] = crc & 0xFF;

    size_t total_len = 19 + payload_len;
    esp_mqtt_client_publish(mqtt_client, topic, (const char*)frame, total_len, 1, retain);
    ESP_LOGI(TAG, "MQTT TX CMD: 0x%02X, Seq: %d, Len: %zu, Topic: %s", cmd, seq, total_len, topic);
}

static void send_mqtt_ack(uint32_t requestId, uint8_t ackPhase, uint8_t cmdEcho, uint8_t detailCode) {
    uint8_t payload[7];
    payload[0] = (requestId >> 24) & 0xFF;
    payload[1] = (requestId >> 16) & 0xFF;
    payload[2] = (requestId >> 8) & 0xFF;
    payload[3] = requestId & 0xFF;
    payload[4] = ackPhase;
    payload[5] = cmdEcho;
    payload[6] = detailCode;
    publish_mqtt_frame(MSG_TYPE_ACK, CMD_ACK, payload, 7, topic_ack, false);
}

void api_client_send_firmware_status(uint32_t requestId, uint8_t target, uint8_t state, uint8_t progressPct, uint8_t errorCode, uint32_t bytesTransferred, uint32_t reportedVersion) {
    if (!mqtt_connected || mqtt_client == NULL) return;

    uint8_t payload[16];
    payload[0] = (requestId >> 24) & 0xFF;
    payload[1] = (requestId >> 16) & 0xFF;
    payload[2] = (requestId >> 8) & 0xFF;
    payload[3] = requestId & 0xFF;
    payload[4] = target;
    payload[5] = state;
    payload[6] = progressPct;
    payload[7] = errorCode;
    payload[8] = (bytesTransferred >> 24) & 0xFF;
    payload[9] = (bytesTransferred >> 16) & 0xFF;
    payload[10] = (bytesTransferred >> 8) & 0xFF;
    payload[11] = bytesTransferred & 0xFF;
    payload[12] = (reportedVersion >> 24) & 0xFF;
    payload[13] = (reportedVersion >> 16) & 0xFF;
    payload[14] = (reportedVersion >> 8) & 0xFF;
    payload[15] = reportedVersion & 0xFF;

    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_FIRMWARE_STATUS, payload, 16, topic_event, false);
    ESP_LOGI(TAG, "Sent FIRMWARE_STATUS: Req=0x%08lX, Target=0x%02X, State=0x%02X, Pct=%d%%, Err=0x%02X, Bytes=%lu, Ver=0x%08lX",
             (unsigned long)requestId, target, state, progressPct, errorCode, (unsigned long)bytesTransferred, (unsigned long)reportedVersion);
}

void api_client_send_firmware_info_ex(uint32_t requestId, uint8_t count, const char *req_device_id, const char *custom_topic, uint8_t custom_msg_type) {
    if (!mqtt_connected || mqtt_client == NULL) return;

    size_t payload_len = 5 + (count * 24);
    uint8_t payload[64];
    if (payload_len > sizeof(payload)) return;

    payload[0] = (requestId >> 24) & 0xFF;
    payload[1] = (requestId >> 16) & 0xFF;
    payload[2] = (requestId >> 8) & 0xFF;
    payload[3] = requestId & 0xFF;
    payload[4] = count;

    if (count > 0) {
        payload[5] = OTA_TARGET_UNIT1_ESP;
        memset(&payload[6], 0, 16);
        
        // Report queried device ID if provided, otherwise active cloud device ID
        const char *dev_id_to_report = (req_device_id && strlen(req_device_id) > 0) ? req_device_id : get_cloud_device_id();
        strncpy((char *)&payload[6], dev_id_to_report, 16);

        uint32_t ver = app_get_firmware_version();
        payload[22] = (ver >> 24) & 0xFF;
        payload[23] = (ver >> 16) & 0xFF;
        payload[24] = (ver >> 8) & 0xFF;
        payload[25] = ver & 0xFF;

        uint16_t prod_id = app_get_product_id();
        payload[26] = (prod_id >> 8) & 0xFF;
        payload[27] = prod_id & 0xFF;

        payload[28] = app_get_hardware_revision();
    }

    // Protocol Step 42 Section 2.5: response sent on onloi/v1/d/{deviceId}/event with MSG_TYPE_EVENT (0x03)
    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_FIRMWARE_INFO, payload, payload_len, topic_event, false);

    // In case the client requested via REQUEST (0x01) on /cmd or /reply and expects RESPONSE (0x02) on /reply
    if (custom_topic != NULL && strlen(custom_topic) > 0 && strcmp(custom_topic, topic_event) != 0) {
        publish_mqtt_frame(custom_msg_type, CMD_FIRMWARE_INFO, payload, payload_len, custom_topic, false);
    }

    ESP_LOGI(TAG, "Sent FIRMWARE_INFO: Req=0x%08lX, Count=%d, DevId=%.16s, Ver=0x%08lX, Prod=0x%04X, Rev=0x%02X",
             (unsigned long)requestId, count, (char *)&payload[6],
             (unsigned long)app_get_firmware_version(), app_get_product_id(), app_get_hardware_revision());
}

void api_client_send_firmware_info(uint32_t requestId, uint8_t count) {
    api_client_send_firmware_info_ex(requestId, count, NULL, NULL, MSG_TYPE_EVENT);
}

void api_client_send_firmware_update_request(void) {
    if (!mqtt_connected || mqtt_client == NULL) {
        ESP_LOGW(TAG, "Cannot send FIRMWARE_UPDATE_REQUEST: MQTT not connected");
        return;
    }

    uint8_t payload[12];
    uint32_t req_id = mqtt_seq;
    payload[0] = (req_id >> 24) & 0xFF;
    payload[1] = (req_id >> 16) & 0xFF;
    payload[2] = (req_id >> 8) & 0xFF;
    payload[3] = req_id & 0xFF;

    payload[4] = OTA_TARGET_UNIT1_ESP;

    uint32_t ver = app_get_firmware_version();
    payload[5] = (ver >> 24) & 0xFF;
    payload[6] = (ver >> 16) & 0xFF;
    payload[7] = (ver >> 8) & 0xFF;
    payload[8] = ver & 0xFF;

    uint16_t prod_id = app_get_product_id();
    payload[9] = (prod_id >> 8) & 0xFF;
    payload[10] = prod_id & 0xFF;

    payload[11] = app_get_hardware_revision();

    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_FIRMWARE_UPDATE_REQUEST, payload, 12, topic_event, false);
    ESP_LOGI(TAG, "Sent FIRMWARE_UPDATE_REQUEST: Req=0x%08lX, Ver=0x%08lX, Prod=0x%04X, Rev=0x%02X",
             (unsigned long)req_id, (unsigned long)ver, prod_id, payload[11]);
}

static void api_client_on_ota_status(uint32_t request_id, uint8_t target, uint8_t state,
                                    uint8_t progress_pct, uint8_t error_code,
                                    uint32_t bytes_transferred, uint32_t reported_version) {
    api_client_send_firmware_status(request_id, target, state, progress_pct, error_code, bytes_transferred, reported_version);
}

static void process_mqtt_message(const char *topic, size_t topic_len, const char *data, size_t data_len) {
    if (data_len < 19) return;
    const uint8_t *frame = (const uint8_t *)data;
    if (frame[0] != SOF_BYTE || frame[1] != VER_BYTE) return;

    uint16_t payload_len = (frame[6] << 8) | frame[7];
    uint8_t msg_type = frame[2];
    uint8_t cmd = frame[3];

    // Check minimum frame size (17 header + payload + 2 CRC = 19 + payload_len)
    size_t min_len = 19 + payload_len;
    if (data_len < min_len) {
        ESP_LOGE(TAG, "MQTT RX frame too short: got %zu, expected at least %zu", data_len, min_len);
        return;
    }

    // Determine if AUTH_TAG (8 bytes HMAC) is appended before CRC16
    bool has_auth_tag = (data_len >= (size_t)(19 + payload_len + 8));

    // CRC is at the very end of the frame
    uint16_t received = (frame[data_len - 2] << 8) | frame[data_len - 1];
    uint16_t calc_all = calculate_crc16(frame, data_len - 2);
    uint16_t calc_no_auth = has_auth_tag ? calculate_crc16(frame, 17 + payload_len) : calc_all;

    if (received != calc_all && received != calc_no_auth && received != 0x0000) {
        ESP_LOGW(TAG, "MQTT RX CRC Warning: Calc=0x%04X/0x%04X, Rx=0x%04X (cmd=0x%02X)",
                 calc_all, calc_no_auth, received, cmd);
    }

    // Extract device ID from incoming topic if present (e.g. onloi/v1/d/78356203936/cmd)
    char topic_dev_id[64] = {0};
    if (topic && strncmp(topic, "onloi/v1/d/", 11) == 0) {
        const char *p = topic + 11;
        const char *slash = strchr(p, '/');
        if (slash && (size_t)(slash - p) < sizeof(topic_dev_id)) {
            memcpy(topic_dev_id, p, slash - p);
            topic_dev_id[slash - p] = '\0';
        }
    }

    char reply_topic[128] = {0};
    if (strlen(topic_dev_id) > 0) {
        snprintf(reply_topic, sizeof(reply_topic), "onloi/v1/d/%s/reply", topic_dev_id);
    } else {
        snprintf(reply_topic, sizeof(reply_topic), "%s", topic_reply);
    }

    ESP_LOGI(TAG, "MQTT RX CMD: 0x%02X, MsgType: 0x%02X, PayloadLen: %d, DataLen: %zu, TopicDev: %s",
             cmd, msg_type, payload_len, data_len, topic_dev_id);

    // Accept both COMMAND (0x04) and REQUEST (0x01)
    if (msg_type == MSG_TYPE_COMMAND || msg_type == MSG_TYPE_REQUEST) {
        const uint8_t *payload = frame + 17;
        uint32_t requestId = 0;
        if (payload_len >= 4) {
            requestId = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
        }

        if (cmd == CMD_REMOTE_OPEN) {
            send_mqtt_ack(requestId, 0x01, CMD_REMOTE_OPEN, 0x00); // RECEIVED
            app_trigger_door_open();
            send_mqtt_ack(requestId, 0x03, CMD_REMOTE_OPEN, 0x00); // COMPLETED
        } else if (cmd == CMD_REMOTE_CLOSE) {
            send_mqtt_ack(requestId, 0x01, CMD_REMOTE_CLOSE, 0x00); // RECEIVED
            app_trigger_door_close();
            send_mqtt_ack(requestId, 0x03, CMD_REMOTE_CLOSE, 0x00); // COMPLETED
        } else if (cmd == CMD_FACTORY_RESET) {
            send_mqtt_ack(requestId, 0x01, CMD_FACTORY_RESET, 0x00);
            send_mqtt_ack(requestId, 0x03, CMD_FACTORY_RESET, 0x00);
            app_set_state(STATE_FACTORY_RESET);
        } else if (cmd == CMD_WIFI_RESET) {
            send_mqtt_ack(requestId, 0x01, CMD_WIFI_RESET, 0x00);
            wifi_manager_clear_credentials();
            send_mqtt_ack(requestId, 0x03, CMD_WIFI_RESET, 0x00);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else if (cmd == CMD_DISCONNECTED) {
            send_mqtt_ack(requestId, 0x01, CMD_DISCONNECTED, 0x00);
            wifi_manager_clear_credentials();
            api_client_reset_claim_status();
            send_mqtt_ack(requestId, 0x03, CMD_DISCONNECTED, 0x00);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else if (cmd == CMD_NFC_ENROLL_START) {
            current_enroll_req_id = requestId;
            send_mqtt_ack(requestId, 0x01, CMD_NFC_ENROLL_START, 0x00); // RECEIVED
            app_set_state(STATE_ONLINE_CARD_ENROLL);
            send_mqtt_ack(requestId, 0x03, CMD_NFC_ENROLL_START, 0x00); // COMPLETED
            api_client_send_nfc_enroll_ready_event();
        } else if (cmd == CMD_GET_FIRMWARE_INFO) {
            char req_dev_id[17] = {0};
            uint8_t target = 0x00;
            if (payload_len >= 5) {
                target = payload[4];
            }
            if (payload_len >= 21) {
                memcpy(req_dev_id, payload + 5, 16);
                req_dev_id[16] = '\0';
                for (int i = 15; i >= 0; i--) {
                    if (req_dev_id[i] == ' ' || req_dev_id[i] == '\0') req_dev_id[i] = '\0';
                    else break;
                }
            }
            if (strlen(req_dev_id) == 0) {
                if (strlen(topic_dev_id) > 0) {
                    strncpy(req_dev_id, topic_dev_id, 16);
                } else {
                    strncpy(req_dev_id, get_cloud_device_id(), 16);
                }
            }

            ESP_LOGI(TAG, "GET_FIRMWARE_INFO: Req=0x%08lX, Target=0x%02X, DevId='%.16s'",
                     (unsigned long)requestId, target, req_dev_id);

            // Send ACK RECEIVED
            send_mqtt_ack(requestId, 0x01, CMD_GET_FIRMWARE_INFO, 0x00);

            if (target == OTA_TARGET_ALL_LOCAL || target == OTA_TARGET_UNIT1_ESP || target == 0x00) {
                api_client_send_firmware_info_ex(requestId, 1, req_dev_id, reply_topic, MSG_TYPE_RESPONSE);
            } else {
                ESP_LOGW(TAG, "GET_FIRMWARE_INFO: Target 0x%02X unsupported (device is single ESP32)", target);
                api_client_send_firmware_info_ex(requestId, 0, req_dev_id, reply_topic, MSG_TYPE_RESPONSE);
                api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_TARGET_INVALID, 0, 0);
            }

            // Send ACK COMPLETED
            send_mqtt_ack(requestId, 0x03, CMD_GET_FIRMWARE_INFO, 0x00);
        } else if (cmd == CMD_FIRMWARE_UPDATE) {
            send_mqtt_ack(requestId, 0x01, CMD_FIRMWARE_UPDATE, 0x00); // RECEIVED
            if (payload_len < 85) {
                ESP_LOGE(TAG, "FIRMWARE_UPDATE payload too small: %d", payload_len);
                api_client_send_firmware_status(requestId, OTA_TARGET_UNIT1_ESP, OTA_STATE_FAILED, 0, OTA_ERR_PAYLOAD_INVALID, 0, 0);
            } else {
                uint8_t target = payload[4];
                uint16_t product_id = ((uint16_t)payload[5] << 8) | payload[6];
                uint8_t hw_rev = payload[7];
                uint32_t fw_ver = ((uint32_t)payload[31] << 24) | ((uint32_t)payload[32] << 16) | ((uint32_t)payload[33] << 8) | payload[34];
                uint32_t fw_len = ((uint32_t)payload[35] << 24) | ((uint32_t)payload[36] << 16) | ((uint32_t)payload[37] << 8) | payload[38];
                const uint8_t *sha256 = &payload[39];
                uint32_t fw_crc = ((uint32_t)payload[71] << 24) | ((uint32_t)payload[72] << 16) | ((uint32_t)payload[73] << 8) | payload[74];

                uint64_t url_expires_at = 0;
                for (int i = 0; i < 8; i++) {
                    url_expires_at = (url_expires_at << 8) | payload[75 + i];
                }

                uint16_t url_len = ((uint16_t)payload[83] << 8) | payload[84];
                if (payload_len < 85 + url_len || url_len == 0) {
                    ESP_LOGE(TAG, "FIRMWARE_UPDATE invalid url_len %d (payload_len %d)", url_len, payload_len);
                    api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_PAYLOAD_INVALID, 0, 0);
                } else if (target != OTA_TARGET_UNIT1_ESP) {
                    // Protocol Section 2.2: Unsupported target -> TARGET_INVALID (0x04)
                    ESP_LOGE(TAG, "FIRMWARE_UPDATE rejected: target 0x%02X unsupported (device is single ESP32)", target);
                    api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_TARGET_INVALID, 0, 0);
                } else if (product_id != 0 && product_id != app_get_product_id()) {
                    ESP_LOGE(TAG, "FIRMWARE_UPDATE rejected: product_id mismatch (got 0x%04X, expected 0x%04X)",
                             product_id, app_get_product_id());
                    api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_PRODUCT_ID_MISMATCH, 0, 0);
                } else if (hw_rev != 0 && hw_rev != app_get_hardware_revision()) {
                    ESP_LOGE(TAG, "FIRMWARE_UPDATE rejected: hw_rev mismatch (got 0x%02X, expected 0x%02X)",
                             hw_rev, app_get_hardware_revision());
                    api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_HW_REV_MISMATCH, 0, 0);
                } else {
                    // Check URL expiration
                    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL) + time_offset_ms;
                    if (url_expires_at != 0 && time_offset_ms != 0 && now_ms > url_expires_at) {
                        ESP_LOGE(TAG, "FIRMWARE_UPDATE rejected: URL expired (now: %llu, exp: %llu)", now_ms, url_expires_at);
                        api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_URL_EXPIRED, 0, 0);
                    } else {
                        char *url_copy = malloc(url_len + 1);
                        if (!url_copy) {
                            api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_FLASH_WRITE, 0, 0);
                        } else {
                            memcpy(url_copy, &payload[85], url_len);
                            url_copy[url_len] = '\0';

                            // Acknowledge acceptance: State ACCEPTED (0x01)
                            api_client_send_firmware_status(requestId, target, OTA_STATE_ACCEPTED, 0, OTA_ERR_NONE, 0, 0);

                            ota_update_params_t ota_params = {
                                .request_id = requestId,
                                .firmware_target = target,
                                .firmware_version = fw_ver,
                                .plain_fw_length = fw_len,
                                .plain_fw_crc32 = fw_crc,
                                .has_crc32 = (fw_crc != 0),
                                .url = url_copy,
                            };
                            memcpy(ota_params.plain_fw_sha256, sha256, 32);
                            ota_params.has_sha256 = false;
                            for (int i = 0; i < 32; i++) {
                                if (sha256[i] != 0) {
                                    ota_params.has_sha256 = true;
                                    break;
                                }
                            }

                            if (!ota_manager_start_update(&ota_params)) {
                                free(url_copy);
                                api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_ALREADY_IN_PROGRESS, 0, 0);
                            }
                        }
                    }
                }
            }
        } else if (cmd == CMD_FIRMWARE_ABORT) {
            send_mqtt_ack(requestId, 0x01, CMD_FIRMWARE_ABORT, 0x00);
            uint8_t reason = (payload_len >= 5) ? payload[4] : 0x01;
            ESP_LOGI(TAG, "FIRMWARE_ABORT received for Req: 0x%08lX (reason: 0x%02X)", (unsigned long)requestId, reason);
            ota_manager_abort(requestId, reason);
            send_mqtt_ack(requestId, 0x03, CMD_FIRMWARE_ABORT, 0x00);
        } else if (cmd == CMD_OTA_START) {
            send_mqtt_ack(requestId, 0x01, CMD_OTA_START, 0x00);
            
            if (payload_len >= 5) {
                char url[256] = {0};
                size_t url_len = payload_len - 4;
                if (url_len > sizeof(url) - 1) url_len = sizeof(url) - 1;
                memcpy(url, &payload[4], url_len);
                
                ESP_LOGI(TAG, "Legacy OTA Start command received. URL: %s", url);
                if (!ota_manager_start(url)) {
                    send_mqtt_ack(requestId, 0x03, CMD_OTA_START, 0x01); // Failed to start
                }
            } else {
                ESP_LOGE(TAG, "OTA Start command has no URL payload");
                send_mqtt_ack(requestId, 0x03, CMD_OTA_START, 0x02); // Error
            }
        } else {
            ESP_LOGW(TAG, "Unhandled MQTT Command 0x%02X, sending ACK to unblock server", cmd);
            send_mqtt_ack(requestId, 0x01, cmd, 0x00); // Send RECEIVED
            send_mqtt_ack(requestId, 0x03, cmd, 0x00); // Send COMPLETED to unblock
        }
    } else if (msg_type == MSG_TYPE_RESPONSE) {
        if (cmd == CMD_OFFLINE_LOG_BATCH) {
            if (payload_len >= 3) {
                const uint8_t *payload = frame + 17;
                uint16_t accepted = (payload[0] << 8) | payload[1];
                uint8_t errCode = payload[2];
                if (errCode == 0x00) {
                    nv_storage_clear_offline_logs((uint8_t)accepted);
                    ESP_LOGI(TAG, "Offline logs successfully uploaded: %d", accepted);
                    
                    uint8_t remaining = 0;
                    nv_storage_get_offline_logs(NULL, &remaining);
                    if (remaining > 0) {
                        api_client_send_offline_logs();
                    }
                } else {
                    ESP_LOGW(TAG, "Server rejected offline logs, err: %02X", errCode);
                }
            }
        } else if (cmd == CMD_TIME_SYNC) {
            if (payload_len >= 8) {
                const uint8_t *payload = frame + 17;
                uint64_t server_ts = 0;
                for (int i = 0; i < 8; i++) {
                    server_ts = (server_ts << 8) | payload[i];
                }
                uint64_t local_ts = (uint64_t)(esp_timer_get_time() / 1000ULL);
                time_offset_ms = server_ts - local_ts;
                ESP_LOGI(TAG, "Time synced! Offset: %llu ms, Server Time: %llu", time_offset_ms, server_ts);
            }
        } else if (cmd == CMD_PIN_ENTERED || cmd == CMD_NFC_SCANNED || cmd == CMD_QR_SCANNED) {
            uint8_t status = frame[8]; // STATUS byte
            if (status == 0x00) {
                ESP_LOGI(TAG, "Server Approved Access via CMD 0x%02X! Opening door...", cmd);
                app_trigger_door_open();
            } else {
                ESP_LOGW(TAG, "Server Denied Access via CMD 0x%02X! Status: 0x%02X", cmd, status);
                app_trigger_access_denied();
            }
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            const char *active_id = get_cloud_device_id();
            ESP_LOGI(TAG, "MQTT Connected! Active Cloud DeviceId: %s", active_id);
            mqtt_connected = true;
            esp_mqtt_client_subscribe(mqtt_client, topic_cmd, 1);
            esp_mqtt_client_subscribe(mqtt_client, topic_reply, 1);
            esp_mqtt_client_subscribe(mqtt_client, topic_config, 1);

            // Also subscribe to hardware MAC ID if different from active_id
            if (strcmp(active_id, device_id_str) != 0) {
                char alt_cmd[128], alt_reply[128], alt_config[128];
                snprintf(alt_cmd, sizeof(alt_cmd), "onloi/v1/d/%s/cmd", device_id_str);
                snprintf(alt_reply, sizeof(alt_reply), "onloi/v1/d/%s/reply", device_id_str);
                snprintf(alt_config, sizeof(alt_config), "onloi/v1/d/%s/config", device_id_str);
                esp_mqtt_client_subscribe(mqtt_client, alt_cmd, 1);
                esp_mqtt_client_subscribe(mqtt_client, alt_reply, 1);
                esp_mqtt_client_subscribe(mqtt_client, alt_config, 1);
                ESP_LOGI(TAG, "Subscribed to dual topics: active=%s, hw=%s", active_id, device_id_str);
            }
            
            // Send Lifecycle events
            uint8_t reg_payload[3] = { (s_mqtt_cfg.session_epoch >> 8) & 0xFF, s_mqtt_cfg.session_epoch & 0xFF, 0x00 };
            publish_mqtt_frame(MSG_TYPE_EVENT, CMD_REGISTRATION, reg_payload, 3, topic_lifecycle, true);

            uint8_t sess_payload[3] = { 0x01, 85, 0x00 };
            publish_mqtt_frame(MSG_TYPE_EVENT, CMD_SESSION_START, sess_payload, 3, topic_lifecycle, true);
            
            api_client_send_get_time();
            
            // Send unsolicited FIRMWARE_INFO (0x3A) with requestId = 0 on connect
            api_client_send_firmware_info(0, 1);
            
            uint8_t offline_log_count = 0;
            nv_storage_get_offline_logs(NULL, &offline_log_count);
            if (offline_log_count > 0) {
                api_client_send_offline_logs();
            }
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected.");
            mqtt_connected = false;
            break;
        case MQTT_EVENT_DATA:
            process_mqtt_message(event->topic, event->topic_len, event->data, event->data_len);
            break;
        default:
            break;
    }
}

static void start_mqtt(void) {
    if (mqtt_client != NULL) return;
    if (strlen(s_mqtt_cfg.host) == 0) {
        ESP_LOGW(TAG, "Cannot start MQTT: broker host is empty");
        return;
    }

    const char *active_id = get_cloud_device_id();
    snprintf(topic_cmd, sizeof(topic_cmd), "onloi/v1/d/%s/cmd", active_id);
    snprintf(topic_reply, sizeof(topic_reply), "onloi/v1/d/%s/reply", active_id);
    snprintf(topic_config, sizeof(topic_config), "onloi/v1/d/%s/config", active_id);
    snprintf(topic_lifecycle, sizeof(topic_lifecycle), "onloi/v1/d/%s/lifecycle", active_id);
    snprintf(topic_event, sizeof(topic_event), "onloi/v1/d/%s/event", active_id);
    snprintf(topic_ack, sizeof(topic_ack), "onloi/v1/d/%s/ack", active_id);

    if (strlen(s_mqtt_cfg.client_id) == 0) {
        snprintf(s_mqtt_cfg.client_id, sizeof(s_mqtt_cfg.client_id), "onloi_d_%s", active_id);
    }

    bool use_tls = (s_mqtt_cfg.flags & 0x01) || (s_mqtt_cfg.port == 8883);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = s_mqtt_cfg.host,
        .broker.address.port = s_mqtt_cfg.port,
        .broker.address.transport = use_tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
        .broker.verification.crt_bundle_attach = use_tls ? esp_crt_bundle_attach : NULL,
        .credentials.client_id = s_mqtt_cfg.client_id,
        .credentials.username = s_mqtt_cfg.username,
        .credentials.authentication.password = s_mqtt_cfg.password,
        .session.disable_clean_session = 1,
        .session.keepalive = 30,
        .session.last_will.topic = topic_lifecycle,
        .session.last_will.msg = "",
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1
    };

    ESP_LOGI(TAG, "Starting MQTT: Host: %s, Port: %d, TLS: %d, ClientId: %s, User: %s, ActiveId: %s",
             s_mqtt_cfg.host, s_mqtt_cfg.port, use_tls, s_mqtt_cfg.client_id, s_mqtt_cfg.username, active_id);
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void api_client_init(void) {
    generate_device_id();
    ota_manager_set_status_cb(api_client_on_ota_status);
    nv_storage_get_onloi_mqtt_config(&s_mqtt_cfg);
}

void api_client_reload_config(void) {
    if (nv_storage_get_onloi_mqtt_config(&s_mqtt_cfg) && s_mqtt_cfg.is_claimed) {
        ESP_LOGI(TAG, "Cloud config reloaded: Host=%s, Port=%d, TLS=%d",
                 s_mqtt_cfg.host, s_mqtt_cfg.port, (s_mqtt_cfg.flags & 0x01));
        if (wifi_manager_is_connected() && mqtt_client == NULL) {
            start_mqtt();
        }
    }
}

void api_client_reset_claim_status(void) {
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }
    s_mqtt_cfg.is_claimed = false;
    memset(&s_mqtt_cfg, 0, sizeof(s_mqtt_cfg));
    mqtt_connected = false;
    nv_storage_clear_onloi_mqtt_config();
    ESP_LOGI(TAG, "Cloud config reset. Ready for new provision.");
}

void api_client_tick(void) {
    static int64_t last_check = 0;
    int64_t now = esp_timer_get_time() / 1000;

    if (now - last_check > 5000) {
        last_check = now;
        if (wifi_manager_is_connected()) {
            if (!s_mqtt_cfg.is_claimed) {
                if (nv_storage_get_onloi_mqtt_config(&s_mqtt_cfg) && s_mqtt_cfg.is_claimed && strlen(s_mqtt_cfg.host) > 0) {
                    start_mqtt();
                }
                // If NVS is empty, stay in BLE setup mode. No hardcoded URL or HTTPS claim.
            } else if (mqtt_client == NULL) {
                start_mqtt();
            }
        }
    }
}

int api_client_send_pass_event(const char* data_val) {
    if (!mqtt_connected) {
        ESP_LOGW(TAG, "Failed to send MQTT Event! Code: -1");
        return -1;
    }

    uint8_t cmd = 0;
    const char *payload_data = NULL;

    if (strncmp(data_val, "KEYPAD:", 7) == 0) {
        cmd = CMD_PIN_ENTERED;
        payload_data = data_val + 7;
    } else if (strncmp(data_val, "RFID:", 5) == 0) {
        cmd = CMD_NFC_SCANNED;
        payload_data = data_val + 5;
    } else if (strncmp(data_val, "QR:", 3) == 0) {
        cmd = CMD_QR_SCANNED;
        payload_data = data_val + 3;
    } else {
        return -2; // Unknown
    }

    uint8_t payload[128];
    uint8_t len = strlen(payload_data);
    if (len > 120) len = 120;
    
    if (cmd == CMD_QR_SCANNED) {
        // Protocol states QR uses 2-byte BE length + data, NO reqId
        payload[0] = 0x00;
        payload[1] = len;
        memcpy(payload + 2, payload_data, len);
        publish_mqtt_frame(MSG_TYPE_EVENT, cmd, payload, 2 + len, topic_event, false);
    } else if (cmd == CMD_NFC_SCANNED) {
        // NFC payload is raw bytes representing the UID + 4-byte reqId
        uint32_t card_id = strtoul(payload_data, NULL, 10);
        payload[0] = 4; // uidLen is 4 bytes
        payload[1] = (card_id >> 24) & 0xFF;
        payload[2] = (card_id >> 16) & 0xFF;
        payload[3] = (card_id >> 8) & 0xFF;
        payload[4] = card_id & 0xFF;
        
        uint32_t reqId = mqtt_seq;
        payload[5] = (reqId >> 24) & 0xFF;
        payload[6] = (reqId >> 16) & 0xFF;
        payload[7] = (reqId >> 8) & 0xFF;
        payload[8] = reqId & 0xFF;

        publish_mqtt_frame(MSG_TYPE_EVENT, cmd, payload, 1 + 4 + 4, topic_event, false);
    } else {
        // PIN uses 1-byte length + ASCII data + 4-byte reqId
        payload[0] = len;
        memcpy(payload + 1, payload_data, len);
        
        uint32_t reqId = mqtt_seq;
        payload[1 + len] = (reqId >> 24) & 0xFF;
        payload[1 + len + 1] = (reqId >> 16) & 0xFF;
        payload[1 + len + 2] = (reqId >> 8) & 0xFF;
        payload[1 + len + 3] = reqId & 0xFF;

        publish_mqtt_frame(MSG_TYPE_EVENT, cmd, payload, 1 + len + 4, topic_event, false);
    }
    return 200;
}

void api_client_send_nfc_enroll_ready_event(void) {
    if (!mqtt_connected) return;
    
    uint8_t payload[4];
    payload[0] = (current_enroll_req_id >> 24) & 0xFF;
    payload[1] = (current_enroll_req_id >> 16) & 0xFF;
    payload[2] = (current_enroll_req_id >> 8) & 0xFF;
    payload[3] = current_enroll_req_id & 0xFF;
    
    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_NFC_ENROLL_READY, payload, 4, topic_event, false);
}

void api_client_send_nfc_enrolled_event(uint32_t card_id) {
    if (!mqtt_connected) return;
    
    uint8_t payload[9];
    
    // Echo the requestId received from CMD_NFC_ENROLL_START (0x44)
    payload[0] = (current_enroll_req_id >> 24) & 0xFF;
    payload[1] = (current_enroll_req_id >> 16) & 0xFF;
    payload[2] = (current_enroll_req_id >> 8) & 0xFF;
    payload[3] = current_enroll_req_id & 0xFF;
    
    // uidLen
    payload[4] = 0x04;
    
    // UID bytes
    payload[5] = (card_id >> 24) & 0xFF;
    payload[6] = (card_id >> 16) & 0xFF;
    payload[7] = (card_id >> 8) & 0xFF;
    payload[8] = card_id & 0xFF;
    
    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_NFC_ENROLLED, payload, 9, topic_event, false);
}

int api_client_send_get_time(void) {
    if (!mqtt_connected) return -1;
    publish_mqtt_frame(MSG_TYPE_REQUEST, CMD_TIME_SYNC, NULL, 0, topic_cmd, false);
    return 200;
}

void api_client_send_lock_opened_event(void) {
    if (!mqtt_connected) return;
    
    // Send a MSG_TYPE_EVENT (0x03) with CMD_LOCK_OPENED (0x10)
    // The payload can be empty or a simple 1-byte marker. We'll send an empty payload (len=0).
    // If the server requires reqId in the payload like pass events, we can send a 4-byte reqId payload.
    // For now, let's send just the reqId as payload to be safe (4 bytes total).
    uint8_t payload[4];
    uint32_t reqId = mqtt_seq;
    payload[0] = (reqId >> 24) & 0xFF;
    payload[1] = (reqId >> 16) & 0xFF;
    payload[2] = (reqId >> 8) & 0xFF;
    payload[3] = reqId & 0xFF;

    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_LOCK_OPENED, payload, 4, topic_event, false);
}

void api_client_send_local_factory_reset_event(void) {
    if (!mqtt_connected) return;
    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_LOCAL_FACTORY_RESET, NULL, 0, topic_event, true);
    ESP_LOGI(TAG, "Published LOCAL_FACTORY_RESET event.");
}

void api_client_send_offline_logs(void) {
    if (!mqtt_connected) return;
    uint8_t count = 0;
    nv_storage_get_offline_logs(NULL, &count);
    if (count == 0) return;
    
    offline_log_t *logs = malloc(count * sizeof(offline_log_t));
    if (!logs) return;
    nv_storage_get_offline_logs(logs, &count);
    
    size_t payload_len = 1 + (count * 12);
    uint8_t *payload = malloc(payload_len);
    if (!payload) {
        free(logs);
        return;
    }
    
    payload[0] = count;
    for (int i=0; i<count; i++) {
        uint8_t *entry = &payload[1 + (i * 12)];
        uint64_t ts = logs[i].timestamp;
        for (int j=0; j<8; j++) {
            entry[j] = (ts >> (56 - (j * 8))) & 0xFF;
        }
        entry[8] = logs[i].accessMethod;
        entry[9] = (logs[i].localId >> 8) & 0xFF;
        entry[10] = logs[i].localId & 0xFF;
        entry[11] = logs[i].result;
    }
    
    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_OFFLINE_LOG_BATCH, payload, payload_len, topic_event, true);
    ESP_LOGI(TAG, "Published OFFLINE_LOG_BATCH event with %d logs.", count);
    
    free(payload);
    free(logs);
}
