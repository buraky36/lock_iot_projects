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
#include "hal_io.h"
#include "../app_state_machine/include/app_state_machine.h"
#include "ota_manager.h"
#include <string.h>
#include <strings.h>
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
#define CMD_TELEMETRY 0x32
#define CMD_REGISTRATION 0x34

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static uint16_t mqtt_seq = 0;
static char device_id_str[32] = {0};
static onloi_mqtt_config_t s_mqtt_cfg;
static uint64_t time_offset_ms = 0;
static uint32_t current_enroll_req_id = 0;
static char s_ota_active_dev_id[64] = {0};
static char s_current_cmd_dev_id[64] = {0};
static uint16_t s_current_cmd_seq = 0;
static bool s_is_remote_reset = false;

// Command deduplication state
static uint16_t s_last_handled_seq = 0xFFFF;
static uint8_t  s_last_handled_cmd = 0xFF;
static int64_t  s_last_handled_time_ms = 0;

static char topic_cmd[128];
static char topic_reply[128];
static char topic_config[128];
static char topic_lifecycle[128];
static char topic_event[128];
static char topic_ack[128];
static char topic_telemetry[128];
static int64_t s_last_session_keepalive = 0;

static void generate_device_id(void) {
    if (strlen(device_id_str) == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        uint32_t low = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
        const char *code = (strlen(device_code_str) > 0 && strcmp(device_code_str, "UNKNOWN") != 0) ? device_code_str : "OE0113";
        snprintf(device_id_str, sizeof(device_id_str), "%.8s_%06lX", code, (unsigned long)low);
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

static void publish_mqtt_frame_with_seq(uint8_t msg_type, uint8_t cmd, uint16_t forced_seq, const uint8_t *payload, uint16_t payload_len, const char *topic, bool retain) {
    if (!mqtt_connected || mqtt_client == NULL || topic == NULL || strlen(topic) == 0) return;

    size_t total_len = 19 + payload_len;
    uint8_t frame[256];
    if (total_len > sizeof(frame)) {
        ESP_LOGE(TAG, "MQTT frame too large (%zu bytes, max %zu)", total_len, sizeof(frame));
        return;
    }

    frame[0] = SOF_BYTE;
    frame[1] = VER_BYTE;
    frame[2] = msg_type;
    frame[3] = cmd;

    uint16_t seq = (forced_seq != 0xFFFF) ? forced_seq : mqtt_seq++;
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

    esp_mqtt_client_publish(mqtt_client, topic, (const char*)frame, total_len, 1, retain);
    ESP_LOGI(TAG, "MQTT TX CMD: 0x%02X, Seq: %u, Len: %zu, Topic: %s", cmd, seq, total_len, topic);
}

static void publish_mqtt_frame(uint8_t msg_type, uint8_t cmd, const uint8_t *payload, uint16_t payload_len, const char *topic, bool retain) {
    publish_mqtt_frame_with_seq(msg_type, cmd, 0xFFFF, payload, payload_len, topic, retain);
}

static void publish_lifecycle_event(uint8_t cmd, const uint8_t *payload, uint16_t len, bool retain) {
    publish_mqtt_frame(MSG_TYPE_EVENT, cmd, payload, len, topic_lifecycle, retain);
}

void api_client_send_telemetry(void) {
    if (!mqtt_connected || mqtt_client == NULL) return;
    uint8_t payload[1] = { 0x64 }; // 0x64 = 100% battery
    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_TELEMETRY, payload, 1, topic_telemetry, false);
    ESP_LOGI(TAG, "Sent TELEMETRY (0x32, battery=100%%) to %s", topic_telemetry);
}

static void send_mqtt_ack_exact(uint16_t req_seq, uint32_t requestId, uint8_t ackPhase, uint8_t cmdEcho, uint8_t detailCode, const char *alt_dev_id) {
    uint8_t payload[7];
    payload[0] = (requestId >> 24) & 0xFF;
    payload[1] = (requestId >> 16) & 0xFF;
    payload[2] = (requestId >> 8) & 0xFF;
    payload[3] = requestId & 0xFF;
    payload[4] = ackPhase;
    payload[5] = cmdEcho;
    payload[6] = detailCode;

    // Header CMD field is cmdEcho (e.g. 0x59 for SERVER_DISCONNECT, 0x58 for WIFI_RESET, 0x40 for REMOTE_OPEN)
    // per Sections 37.4, 37.5, 38.4, 38.5, and Step 22 of the protocol specification.
    // For FACTORY_RESET (0x50), Section 39.3 allows CMD_ACK (0x05) or 0x50.
    uint8_t header_cmd = (cmdEcho == CMD_FACTORY_RESET) ? CMD_ACK : cmdEcho;

    // 1. Publish to main topic_ack (onloi/v1/d/{active_id}/ack)
    publish_mqtt_frame_with_seq(MSG_TYPE_ACK, header_cmd, req_seq, payload, 7, topic_ack, false);

    // 2. Also publish to uppercase ONLOI/V1/D/{active_id}/ACK
    const char *active = get_cloud_device_id();
    char up_ack[128];
    snprintf(up_ack, sizeof(up_ack), "ONLOI/V1/D/%s/ACK", active);
    publish_mqtt_frame_with_seq(MSG_TYPE_ACK, header_cmd, req_seq, payload, 7, up_ack, false);

    // 3. If alt_dev_id is provided, also publish to both lowercase and uppercase alt ack topics
    if (alt_dev_id != NULL && strlen(alt_dev_id) > 0 && strcmp(alt_dev_id, active) != 0) {
        char alt_ack[128];
        snprintf(alt_ack, sizeof(alt_ack), "onloi/v1/d/%s/ack", alt_dev_id);
        publish_mqtt_frame_with_seq(MSG_TYPE_ACK, header_cmd, req_seq, payload, 7, alt_ack, false);
        snprintf(alt_ack, sizeof(alt_ack), "ONLOI/V1/D/%s/ACK", alt_dev_id);
        publish_mqtt_frame_with_seq(MSG_TYPE_ACK, header_cmd, req_seq, payload, 7, alt_ack, false);
    }
}

static void send_mqtt_ack_to(uint32_t requestId, uint8_t ackPhase, uint8_t cmdEcho, uint8_t detailCode, const char *alt_dev_id) {
    send_mqtt_ack_exact(s_current_cmd_seq, requestId, ackPhase, cmdEcho, detailCode, alt_dev_id);
}

static void send_mqtt_ack(uint32_t requestId, uint8_t ackPhase, uint8_t cmdEcho, uint8_t detailCode) {
    send_mqtt_ack_exact(s_current_cmd_seq, requestId, ackPhase, cmdEcho, detailCode, s_current_cmd_dev_id);
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
    if (strlen(s_ota_active_dev_id) > 0) {
        char alt_event[128];
        snprintf(alt_event, sizeof(alt_event), "onloi/v1/d/%s/event", s_ota_active_dev_id);
        if (strcmp(alt_event, topic_event) != 0) {
            publish_mqtt_frame(MSG_TYPE_EVENT, CMD_FIRMWARE_STATUS, payload, 16, alt_event, false);
        }
    }
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

    // Also send on alt event topic if req_device_id is provided and different from active topic_event
    if (req_device_id != NULL && strlen(req_device_id) > 0) {
        char alt_event[128];
        snprintf(alt_event, sizeof(alt_event), "onloi/v1/d/%s/event", req_device_id);
        if (strcmp(alt_event, topic_event) != 0) {
            publish_mqtt_frame(MSG_TYPE_EVENT, CMD_FIRMWARE_INFO, payload, payload_len, alt_event, false);
        }
    }

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

    if (state == OTA_STATE_FAILED || state == OTA_STATE_ABORTED) {
        send_mqtt_ack_to(request_id, 0x04, CMD_FIRMWARE_UPDATE, error_code, s_ota_active_dev_id);
        nv_storage_clear_active_ota_job();
    } else if (state == OTA_STATE_COMPLETED) {
        send_mqtt_ack_to(request_id, 0x03, CMD_FIRMWARE_UPDATE, 0x00, s_ota_active_dev_id);
    }
}

static bool is_topic_for_this_device(const char *topic_dev_id) {
    if (topic_dev_id == NULL || strlen(topic_dev_id) == 0) return false;

    const char *active = get_cloud_device_id();
    if (strcasecmp(topic_dev_id, active) == 0) return true;
    if (strcasecmp(topic_dev_id, device_id_str) == 0) return true;
    if (strlen(s_mqtt_cfg.client_id) > 0 && strcasecmp(topic_dev_id, s_mqtt_cfg.client_id) == 0) return true;

    char smart_id[32];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    uint32_t low = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    snprintf(smart_id, sizeof(smart_id), "SMART_%06lX", (unsigned long)low);
    if (strcasecmp(topic_dev_id, smart_id) == 0) return true;

    char raw_mac[16];
    snprintf(raw_mac, sizeof(raw_mac), "%06lX", (unsigned long)low);
    if (strcasecmp(topic_dev_id, raw_mac) == 0) return true;

    return false;
}

static void execute_server_disconnect(const char *reason) {
    ESP_LOGW(TAG, "=== [EXECUTE SERVER DISCONNECT] === Reason: %s", reason ? reason : "Unknown");
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_manager_clear_credentials();
    nv_storage_clear_onloi_mqtt_config();
    nv_storage_clear_active_ota_job();
    sys_config_t cfg;
    nv_storage_get_config(&cfg);
    cfg.is_online = false;
    nv_storage_set_config(&cfg);
    hal_io_led_set(LED_COLOR_ORANGE, LED_MODE_SOLID);
    hal_io_buzzer_beep(2700, 300, 2);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "SERVER_DISCONNECT complete. Restarting into BLE setup mode...");
    esp_restart();
}

static void process_mqtt_message(const char *topic, size_t topic_len, const char *data, size_t data_len) {
    if (data == NULL || data_len < 4) {
        return;
    }

    const uint8_t *frame = (const uint8_t *)data;
    if (frame[0] != SOF_BYTE || frame[1] != VER_BYTE) {
        return;
    }

    if (data_len < 19) {
        return;
    }

    uint8_t msg_type = frame[2];
    uint8_t cmd = frame[3];
    uint16_t req_seq = (frame[4] << 8) | frame[5];
    uint16_t payload_len = (frame[6] << 8) | frame[7];

    // Silently ignore our own echoes to keep log screen clean and avoid clutter
    if (msg_type == MSG_TYPE_ACK || msg_type == MSG_TYPE_EVENT) {
        return;
    }

    if (msg_type == MSG_TYPE_REQUEST && cmd == CMD_TIME_SYNC) {
        return;
    }

    // Extract device ID from incoming topic if present (e.g. onloi/v1/d/{devId}/cmd or ONLOI/V1/D/{devId}/CMD)
    char topic_dev_id[64] = {0};
    if (topic && topic_len > 11 && strncasecmp(topic, "onloi/v1/d/", 11) == 0) {
        const char *p = topic + 11;
        size_t rem = topic_len - 11;
        const char *slash = memchr(p, '/', rem);
        if (slash && (size_t)(slash - p) < sizeof(topic_dev_id)) {
            memcpy(topic_dev_id, p, slash - p);
            topic_dev_id[slash - p] = '\0';
        }
    }

    // Strictly ensure message is intended for this device
    if (!is_topic_for_this_device(topic_dev_id)) {
        return;
    }

    // Command Deduplication: ignore identical (cmd, req_seq) arriving within 1000ms
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (cmd == s_last_handled_cmd && req_seq == s_last_handled_seq && (now_ms - s_last_handled_time_ms) < 1000) {
        ESP_LOGW(TAG, "Duplicate command ignored: CMD=0x%02X, Seq=%u (within %lld ms)",
                 cmd, req_seq, (long long)(now_ms - s_last_handled_time_ms));
        return;
    }
    s_last_handled_seq = req_seq;
    s_last_handled_cmd = cmd;
    s_last_handled_time_ms = now_ms;

    char safe_topic[128] = {0};
    if (topic && topic_len > 0) {
        size_t cplen = (topic_len < sizeof(safe_topic) - 1) ? topic_len : (sizeof(safe_topic) - 1);
        memcpy(safe_topic, topic, cplen);
        safe_topic[cplen] = '\0';
    }

    // Log genuine incoming COMMAND (0x04) or RESPONSE (0x02) packets with hexdump
    ESP_LOGI(TAG, "=== [MQTT RX] === Topic='%s', CMD=0x%02X, MsgType=0x%02X, Seq=%u, PayLen=%u, DataLen=%zu",
             safe_topic, cmd, msg_type, req_seq, payload_len, data_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);

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

    s_current_cmd_seq = req_seq;
    strncpy(s_current_cmd_dev_id, topic_dev_id, sizeof(s_current_cmd_dev_id) - 1);
    s_current_cmd_dev_id[sizeof(s_current_cmd_dev_id) - 1] = '\0';

    char reply_topic[128] = {0};
    if (strlen(topic_dev_id) > 0) {
        snprintf(reply_topic, sizeof(reply_topic), "onloi/v1/d/%s/reply", topic_dev_id);
    } else {
        snprintf(reply_topic, sizeof(reply_topic), "%s", topic_reply);
    }

    // Accept both COMMAND (0x04) and REQUEST (0x01)
    if (msg_type == MSG_TYPE_COMMAND || msg_type == MSG_TYPE_REQUEST) {
        const uint8_t *payload = frame + 17;
        uint32_t requestId = 0;
        if (payload_len >= 4) {
            requestId = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
        }

        // Specific handler for SERVER_DISCONNECT (0x59) — Step 38 of protocol specification
        // Triggered when user deletes device from mobile app.
        // Clears Wi-Fi credentials + MQTT config + is_online = false.
        // PRESERVES local NVC users (PIN, NFC, FP, QR) and deviceCode!
        if (cmd == CMD_DISCONNECTED) {
            ESP_LOGW(TAG, "SERVER_DISCONNECT (0x59) received! Req=0x%08lX, Seq=%u, TopicDev=%s",
                     (unsigned long)requestId, req_seq, topic_dev_id);
            // 1. Send ACK RECEIVED (phase 0x01, cmdEcho 0x59, matching req_seq)
            send_mqtt_ack_exact(req_seq, requestId, 0x01, CMD_DISCONNECTED, 0x00, topic_dev_id);
            // 2. Send ACK COMPLETED (phase 0x03, cmdEcho 0x59, matching req_seq)
            send_mqtt_ack_exact(req_seq, requestId, 0x03, CMD_DISCONNECTED, 0x00, topic_dev_id);
            // 3. Execute server disconnect common routine
            execute_server_disconnect("Received CMD_DISCONNECTED (0x59) from cloud");
            return;
        }

        // Specific handler for WIFI_RESET (0x58) — Step 37 of protocol specification
        // Clears Wi-Fi credentials only. Keeps MQTT config and local NVC users.
        if (cmd == CMD_WIFI_RESET) {
            ESP_LOGW(TAG, "WIFI_RESET (0x58) received! Req=0x%08lX, Seq=%u", (unsigned long)requestId, req_seq);
            send_mqtt_ack_exact(req_seq, requestId, 0x01, CMD_WIFI_RESET, 0x00, topic_dev_id);
            send_mqtt_ack_exact(req_seq, requestId, 0x03, CMD_WIFI_RESET, 0x00, topic_dev_id);
            vTaskDelay(pdMS_TO_TICKS(200));
            wifi_manager_clear_credentials();
            sys_config_t cfg;
            nv_storage_get_config(&cfg);
            cfg.is_online = false;
            nv_storage_set_config(&cfg);
            hal_io_buzzer_beep(2700, 200, 2);
            vTaskDelay(pdMS_TO_TICKS(200));
            ESP_LOGW(TAG, "WIFI_RESET complete. Restarting into BLE setup mode...");
            esp_restart();
            return;
        }

        // Remote Factory Reset (0x50 or 0x73) — Step 39 of protocol specification
        // Clears ALL data including NVC users.
        if (cmd == CMD_FACTORY_RESET || cmd == 0x73) {
            ESP_LOGW(TAG, "Remote FACTORY_RESET (0x50) received! Req=0x%08lX, Seq=%u", (unsigned long)requestId, req_seq);
            s_is_remote_reset = true;
            send_mqtt_ack_exact(req_seq, requestId, 0x01, CMD_FACTORY_RESET, 0x00, topic_dev_id);
            send_mqtt_ack_exact(req_seq, requestId, 0x03, CMD_FACTORY_RESET, 0x00, topic_dev_id);
            vTaskDelay(pdMS_TO_TICKS(200));
            app_set_state(STATE_FACTORY_RESET);
            return;
        }

        if (cmd == CMD_REMOTE_OPEN) {
            send_mqtt_ack(requestId, 0x01, CMD_REMOTE_OPEN, 0x00); // RECEIVED
            app_trigger_door_open();
            send_mqtt_ack(requestId, 0x03, CMD_REMOTE_OPEN, 0x00); // COMPLETED
        } else if (cmd == CMD_REMOTE_CLOSE) {
            send_mqtt_ack(requestId, 0x01, CMD_REMOTE_CLOSE, 0x00); // RECEIVED
            app_trigger_door_close();
            send_mqtt_ack(requestId, 0x03, CMD_REMOTE_CLOSE, 0x00); // COMPLETED
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
            if (strlen(topic_dev_id) > 0) {
                strncpy(s_ota_active_dev_id, topic_dev_id, sizeof(s_ota_active_dev_id) - 1);
            } else {
                strncpy(s_ota_active_dev_id, get_cloud_device_id(), sizeof(s_ota_active_dev_id) - 1);
            }
            s_ota_active_dev_id[sizeof(s_ota_active_dev_id) - 1] = '\0';

            send_mqtt_ack(requestId, 0x01, CMD_FIRMWARE_UPDATE, 0x00); // RECEIVED
            if (payload_len < 85) {
                ESP_LOGE(TAG, "FIRMWARE_UPDATE payload too small: %d", payload_len);
                api_client_send_firmware_status(requestId, OTA_TARGET_UNIT1_ESP, OTA_STATE_FAILED, 0, OTA_ERR_PAYLOAD_INVALID, 0, 0);
                send_mqtt_ack(requestId, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_PAYLOAD_INVALID);
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
                    send_mqtt_ack(requestId, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_PAYLOAD_INVALID);
                } else if (target != OTA_TARGET_UNIT1_ESP) {
                    // Protocol Section 2.2: Unsupported target -> TARGET_INVALID (0x04)
                    ESP_LOGE(TAG, "FIRMWARE_UPDATE rejected: target 0x%02X unsupported (device is single ESP32)", target);
                    api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_TARGET_INVALID, 0, 0);
                    send_mqtt_ack(requestId, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_TARGET_INVALID);
                } else if (product_id != 0 && product_id != app_get_product_id()) {
                    ESP_LOGE(TAG, "FIRMWARE_UPDATE rejected: product_id mismatch (got 0x%04X, expected 0x%04X)",
                             product_id, app_get_product_id());
                    api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_PRODUCT_ID_MISMATCH, 0, 0);
                    send_mqtt_ack(requestId, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_PRODUCT_ID_MISMATCH);
                } else if (hw_rev != 0 && hw_rev != app_get_hardware_revision()) {
                    ESP_LOGE(TAG, "FIRMWARE_UPDATE rejected: hw_rev mismatch (got 0x%02X, expected 0x%02X)",
                             hw_rev, app_get_hardware_revision());
                    api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_HW_REV_MISMATCH, 0, 0);
                    send_mqtt_ack(requestId, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_HW_REV_MISMATCH);
                } else {
                    // Check URL expiration
                    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL) + time_offset_ms;
                    if (url_expires_at != 0 && time_offset_ms != 0 && now_ms > url_expires_at) {
                        ESP_LOGE(TAG, "FIRMWARE_UPDATE rejected: URL expired (now: %llu, exp: %llu)", now_ms, url_expires_at);
                        api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_URL_EXPIRED, 0, 0);
                        send_mqtt_ack(requestId, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_URL_EXPIRED);
                    } else {
                        char *url_copy = malloc(url_len + 1);
                        if (!url_copy) {
                            api_client_send_firmware_status(requestId, target, OTA_STATE_FAILED, 0, OTA_ERR_FLASH_WRITE, 0, 0);
                            send_mqtt_ack(requestId, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_FLASH_WRITE);
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
                                send_mqtt_ack(requestId, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_ALREADY_IN_PROGRESS);
                            } else {
                                // Persist active OTA job in NVS to recover and notify cloud/app if power is lost during download/flash
                                nv_storage_save_active_ota_job(requestId, target, fw_ver, s_ota_active_dev_id);
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
        } else if (msg_type == MSG_TYPE_COMMAND) {
            ESP_LOGW(TAG, "Unhandled MQTT Command 0x%02X, sending ACK to unblock server", cmd);
            send_mqtt_ack(requestId, 0x01, cmd, 0x00); // Send RECEIVED
            send_mqtt_ack(requestId, 0x03, cmd, 0x00); // Send COMPLETED to unblock
        }
    } else if (msg_type == MSG_TYPE_RESPONSE) {
        uint8_t status = frame[8]; // STATUS byte
        if (cmd == CMD_TIME_SYNC) {
            if (status != 0x00) {
                ESP_LOGW(TAG, "TIME_SYNC returned status 0x%02X", status);
                return;
            }
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

            int mid;
            // 1. Device specific topics (QoS 1) - Lowercase
            mid = esp_mqtt_client_subscribe(mqtt_client, topic_cmd, 1);
            ESP_LOGI(TAG, "SUB: '%s' (mid=%d)", topic_cmd, mid);
            mid = esp_mqtt_client_subscribe(mqtt_client, topic_reply, 1);
            ESP_LOGI(TAG, "SUB: '%s' (mid=%d)", topic_reply, mid);
            mid = esp_mqtt_client_subscribe(mqtt_client, topic_config, 1);
            ESP_LOGI(TAG, "SUB: '%s' (mid=%d)", topic_config, mid);

            // 2. Protocol spec uppercase subscriptions (Sections 37.3, 38.3, 39.2)
            char up_topic[128];
            snprintf(up_topic, sizeof(up_topic), "ONLOI/V1/D/%s/CMD", active_id);
            mid = esp_mqtt_client_subscribe(mqtt_client, up_topic, 1);
            ESP_LOGI(TAG, "SUB: '%s' (mid=%d)", up_topic, mid);

            snprintf(up_topic, sizeof(up_topic), "ONLOI/V1/D/%s/REPLY", active_id);
            mid = esp_mqtt_client_subscribe(mqtt_client, up_topic, 1);
            ESP_LOGI(TAG, "SUB: '%s' (mid=%d)", up_topic, mid);

            snprintf(up_topic, sizeof(up_topic), "ONLOI/V1/D/%s/CONFIG", active_id);
            mid = esp_mqtt_client_subscribe(mqtt_client, up_topic, 1);
            ESP_LOGI(TAG, "SUB: '%s' (mid=%d)", up_topic, mid);

            // 3. Hardware MAC ID if different from active_id
            if (strcmp(active_id, device_id_str) != 0) {
                char alt_topic[128];
                snprintf(alt_topic, sizeof(alt_topic), "onloi/v1/d/%s/cmd", device_id_str);
                esp_mqtt_client_subscribe(mqtt_client, alt_topic, 1);
                snprintf(alt_topic, sizeof(alt_topic), "ONLOI/V1/D/%s/CMD", device_id_str);
                esp_mqtt_client_subscribe(mqtt_client, alt_topic, 1);

                snprintf(alt_topic, sizeof(alt_topic), "onloi/v1/d/%s/reply", device_id_str);
                esp_mqtt_client_subscribe(mqtt_client, alt_topic, 1);
                snprintf(alt_topic, sizeof(alt_topic), "ONLOI/V1/D/%s/REPLY", device_id_str);
                esp_mqtt_client_subscribe(mqtt_client, alt_topic, 1);

                snprintf(alt_topic, sizeof(alt_topic), "onloi/v1/d/%s/config", device_id_str);
                esp_mqtt_client_subscribe(mqtt_client, alt_topic, 1);
                snprintf(alt_topic, sizeof(alt_topic), "ONLOI/V1/D/%s/CONFIG", device_id_str);
                esp_mqtt_client_subscribe(mqtt_client, alt_topic, 1);
            }

            // NOTE: Wildcards ('#' and '+') are completely removed to prevent adopting foreign IDs
            // and 3x duplicate command reception.

            // 5-Step Battery Device Wake-up Sequence on connect:
            // 1. REGISTRATION (0x34) -> onloi/v1/d/{deviceId}/lifecycle (retain=false)
            uint8_t reg_payload[3] = { (s_mqtt_cfg.session_epoch >> 8) & 0xFF, s_mqtt_cfg.session_epoch & 0xFF, 0x00 };
            publish_lifecycle_event(CMD_REGISTRATION, reg_payload, 3, false);

            // 2. SESSION_START (0x30) -> onloi/v1/d/{deviceId}/lifecycle (wakeReason=0x01 [Button], battery=100%, flags=0x00)
            uint8_t sess_payload[3] = { 0x01, 100, 0x00 };
            publish_lifecycle_event(CMD_SESSION_START, sess_payload, 3, false);
            s_last_session_keepalive = esp_timer_get_time() / 1000;
            
            // 3. TIME_SYNC (0x33) -> onloi/v1/d/{deviceId}/cmd (MSG_TYPE_REQUEST, payload len=0)
            api_client_send_get_time();
            
            // 4. FIRMWARE_INFO (0x3A) -> onloi/v1/d/{deviceId}/event (MSG_TYPE_EVENT, requestId=0)
            api_client_send_firmware_info(0, 1);

            // 5. TELEMETRY (0x32) -> onloi/v1/d/{deviceId}/telemetry (MSG_TYPE_EVENT, payload={100})
            api_client_send_telemetry();

            // Check for interrupted OTA update or pending OTA job verification across reboot/power-loss
            ota_job_record_t ota_job;
            if (nv_storage_get_active_ota_job(&ota_job)) {
                if (strlen(ota_job.active_dev_id) > 0) {
                    strncpy(s_ota_active_dev_id, ota_job.active_dev_id, sizeof(s_ota_active_dev_id) - 1);
                    s_ota_active_dev_id[sizeof(s_ota_active_dev_id) - 1] = '\0';
                }
                uint32_t current_ver = app_get_firmware_version();
                ESP_LOGI(TAG, "Boot OTA check: Req=0x%08lX, TargetVer=0x%08lX, CurrentVer=0x%08lX, DevId=%s",
                         (unsigned long)ota_job.request_id, (unsigned long)ota_job.target_version,
                         (unsigned long)current_ver, s_ota_active_dev_id);

                if (current_ver == ota_job.target_version) {
                    // OTA succeeded! The new firmware booted properly.
                    ESP_LOGI(TAG, "OTA succeeded! New firmware booted (0x%08lX)", (unsigned long)current_ver);
                    api_client_send_firmware_status(ota_job.request_id, ota_job.target, OTA_STATE_COMPLETED, 100, OTA_ERR_NONE, 0, current_ver);
                    send_mqtt_ack_to(ota_job.request_id, 0x03, CMD_FIRMWARE_UPDATE, 0x00, s_ota_active_dev_id);
                } else {
                    // OTA was interrupted! (Power cut, download failure, flash rollback, or crash)
                    ESP_LOGW(TAG, "OTA interrupted! Expected 0x%08lX, still on 0x%08lX. Reporting failure to app.",
                             (unsigned long)ota_job.target_version, (unsigned long)current_ver);
                    api_client_send_firmware_status(ota_job.request_id, ota_job.target, OTA_STATE_FAILED, 0, OTA_ERR_NETWORK, 0, current_ver);
                    send_mqtt_ack_to(ota_job.request_id, 0x04, CMD_FIRMWARE_UPDATE, OTA_ERR_NETWORK, s_ota_active_dev_id);
                }
                nv_storage_clear_active_ota_job();
            }
            break;
        }
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT SUBACK received! Subscribed successfully, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT ERROR event! msg_id=%d", event->msg_id);
            if (event->error_handle) {
                ESP_LOGE(TAG, "  -> Error type: %d", event->error_handle->error_type);
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_ESP_TLS) {
                    ESP_LOGE(TAG, "  -> ESP-TLS last err: 0x%x, stack err: 0x%x",
                             event->error_handle->esp_tls_last_esp_err,
                             event->error_handle->esp_tls_stack_err);
                } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    ESP_LOGE(TAG, "  -> Connection refused code: 0x%x",
                             event->error_handle->connect_return_code);
                }
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected.");
            mqtt_connected = false;
            s_last_session_keepalive = 0;
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
    snprintf(topic_telemetry, sizeof(topic_telemetry), "onloi/v1/d/%s/telemetry", active_id);

    if (strlen(s_mqtt_cfg.client_id) == 0) {
        snprintf(s_mqtt_cfg.client_id, sizeof(s_mqtt_cfg.client_id), "onloi_d_%s", active_id);
    }

    bool use_tls = (s_mqtt_cfg.flags & 0x01) || (s_mqtt_cfg.port == 8883);

    // Requirement 4: Giriş Bilgilerini (Credentials) Logla (Password & Secret Masked)
    ESP_LOGW(TAG, "================ [MQTT CREDENTIALS DUMP] ================");
    ESP_LOGW(TAG, "Host     : %s", s_mqtt_cfg.host);
    ESP_LOGW(TAG, "Port     : %d (TLS=%d)", s_mqtt_cfg.port, use_tls);
    ESP_LOGW(TAG, "ClientId : %s", s_mqtt_cfg.client_id);
    ESP_LOGW(TAG, "Username : %s", s_mqtt_cfg.username);
    ESP_LOGW(TAG, "Password : *** (len=%zu)", strlen(s_mqtt_cfg.password));
    ESP_LOGW(TAG, "Secret   : *** (32-byte HMAC key loaded)");
    ESP_LOGW(TAG, "Epoch    : %u", s_mqtt_cfg.session_epoch);
    ESP_LOGW(TAG, "ActiveId : %s (HW=%s)", active_id, device_id_str);
    ESP_LOGW(TAG, "=========================================================");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = s_mqtt_cfg.host,
        .broker.address.port = s_mqtt_cfg.port,
        .broker.address.transport = use_tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
        .broker.verification.crt_bundle_attach = use_tls ? esp_crt_bundle_attach : NULL,
        .credentials.client_id = s_mqtt_cfg.client_id,
        .credentials.username = s_mqtt_cfg.username,
        .credentials.authentication.password = s_mqtt_cfg.password,
        .session.disable_clean_session = 0, // Clean Session = 1 to discard stale subscriptions
        .session.keepalive = 60,
        .session.last_will.topic = topic_lifecycle,
        .session.last_will.msg = "",
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .task.stack_size = 8192
    };

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
        if (mqtt_client != NULL) {
            ESP_LOGI(TAG, "Stopping previous MQTT client to apply reloaded config...");
            esp_mqtt_client_stop(mqtt_client);
            esp_mqtt_client_destroy(mqtt_client);
            mqtt_client = NULL;
            mqtt_connected = false;
        }
        if (wifi_manager_is_connected()) {
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
    nv_storage_clear_active_ota_job();
    ESP_LOGI(TAG, "Cloud config reset. Ready for new provision.");
}

void api_client_tick(void) {
    static int64_t last_check = 0;
    int64_t now = esp_timer_get_time() / 1000;

    if (now - last_check > 1500) {
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

    // Periodic wake-up simulation for battery lock simulation
    // Every 15 seconds, simulate button wake-up so Onloi Cloud immediately flushes queued management commands
    // (such as SERVER_DISCONNECT 0x59, WIFI_RESET 0x58, FACTORY_RESET 0x50, REMOTE_OPEN 0x40, or GET_FIRMWARE_INFO 0x5C).
    if (mqtt_connected && mqtt_client != NULL && !ota_manager_is_running()) {
        if (s_last_session_keepalive == 0) {
            s_last_session_keepalive = now;
        } else if (now - s_last_session_keepalive >= 15000) {
            api_client_send_session_keepalive();
        }
    }
}

void api_client_send_session_start(uint8_t wake_reason) {
    if (!mqtt_connected || mqtt_client == NULL || ota_manager_is_running()) return;
    s_last_session_keepalive = esp_timer_get_time() / 1000;
    // wakeReason: 0x01 = physical button / user wake, 0x02 = timer
    // 100% battery (12V wired terminal), 0x00 = flags
    uint8_t sess_payload[3] = { wake_reason, 100, 0x00 };
    publish_lifecycle_event(CMD_SESSION_START, sess_payload, 3, false);
    ESP_LOGI(TAG, "Sent SESSION_START (wakeReason: 0x%02X) to %s", wake_reason, topic_lifecycle);
}

void api_client_send_session_keepalive(void) {
    if (!mqtt_connected || mqtt_client == NULL || ota_manager_is_running()) return;
    s_last_session_keepalive = esp_timer_get_time() / 1000;

    // 1. CMD_REGISTRATION (0x34) -> onloi/v1/d/{active_id}/lifecycle (retain=false)
    uint8_t reg_payload[3] = { (s_mqtt_cfg.session_epoch >> 8) & 0xFF, s_mqtt_cfg.session_epoch & 0xFF, 0x00 };
    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_REGISTRATION, reg_payload, 3, topic_lifecycle, false);

    // 2. CMD_SESSION_START (0x30) -> onloi/v1/d/{active_id}/lifecycle (retain=false)
    uint8_t sess_payload[3] = { 0x01, 100, 0x00 };
    publish_mqtt_frame(MSG_TYPE_EVENT, CMD_SESSION_START, sess_payload, 3, topic_lifecycle, false);

    // 3. CMD_TIME_SYNC (0x33) -> onloi/v1/d/{active_id}/cmd
    api_client_send_get_time();

    // 4. CMD_TELEMETRY (0x32) -> onloi/v1/d/{active_id}/telemetry
    api_client_send_telemetry();

    ESP_LOGI(TAG, "Sent periodic wake-up sequence: 0x34 -> 0x30 -> 0x33 -> 0x32");
}

int api_client_send_pass_event(const char* data_val) {
    if (!mqtt_connected) {
        ESP_LOGW(TAG, "Failed to send MQTT Event! Code: -1");
        return -1;
    }

    // Ensure SESSION_START (0x30, wakeReason=0x01) precedes credential transmission
    api_client_send_session_start(0x01);

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
    ESP_LOGI(TAG, "Sending TIME_SYNC request (0x33)");
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
    if (!mqtt_connected || s_is_remote_reset) return;
    uint8_t payload[1] = { 0x01 }; // 0x01 = tuş kombinasyonu / yerel müdahale
    publish_lifecycle_event(CMD_LOCAL_FACTORY_RESET, payload, 1, false);
    ESP_LOGI(TAG, "Published LOCAL_FACTORY_RESET event to %s", topic_lifecycle);
}
