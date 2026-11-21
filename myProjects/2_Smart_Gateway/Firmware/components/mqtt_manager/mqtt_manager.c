#include "mqtt_manager.h"
#include "ble_frame_parser.h"
#include "ble_manager.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "gateway_version.h"
#include "config_manager.h"
#include "crypto_manager.h"
#include "mbedtls/base64.h"
#include "mqtt_client.h"
#include "ota_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

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
static char s_pub_cmd_ack_topic[128] = {0};
static char s_sub_ota_topic[128] = {0};
static char s_sub_sub_ota_topic[128] = {0};
static char s_pub_sub_ota_ack_topic[128] = {0};
static char s_sub_claim_topic[128] = {0};
static char s_pub_claim_ack_topic[128] = {0};
static char s_sub_unclaim_topic[128] = {0};
static char s_pub_unclaim_ack_topic[128] = {0};
static char s_sub_keyrot_topic[128] = {0};
static char s_pub_keyrot_ack_topic[128] = {0};
static char s_pub_subkey_req_topic[128] = {0};
static char s_sub_subkey_resp_topic[128] = {0};

static char s_raw_device_id[32] = {0};
static char s_alt_pub_scan_topic[128] = {0};
static char s_alt_pub_status_topic[128] = {0};
static char s_alt_pub_notify_topic[128] = {0};
static char s_alt_pub_cmd_ack_topic[128] = {0};
static char s_alt_pub_sub_ota_ack_topic[128] = {0};
static char s_alt_pub_claim_ack_topic[128] = {0};
static char s_alt_pub_unclaim_ack_topic[128] = {0};
static char s_alt_pub_keyrot_ack_topic[128] = {0};
static char s_alt_proto_lifecycle_topic[128] = {0};
static char s_alt_sub_cmd_topic[128] = {0};
static char s_alt_sub_ota_topic[128] = {0};
static char s_alt_sub_sub_ota_topic[128] = {0};
static char s_alt_sub_claim_topic[128] = {0};
static char s_alt_sub_unclaim_topic[128] = {0};
static char s_alt_sub_keyrot_topic[128] = {0};
static char s_alt_sub_proto_cmd_topic[128] = {0};
static char s_alt_pub_subkey_req_topic[128] = {0};
static char s_alt_sub_subkey_resp_topic[128] = {0};

// Protocol v2 topics (onloi/v1/d/<deviceId>/...)
static char s_proto_lifecycle_topic[128] = {0};
static char s_proto_event_topic[128] = {0};
static char s_alt_proto_event_topic[128] = {0};
static char s_sub_proto_cmd_topic[128] = {0};
static char s_sub_proto_reply_topic[128] = {0};
static char s_sub_proto_config_topic[128] = {0};

static SemaphoreHandle_t s_subkey_req_sem = NULL;

#define MQTT_KEEPALIVE_SEC 30 // Protocol Step 10

// Helper: exact topic match (event topics are not null-terminated)
static bool topic_equals(const char *topic, int topic_len,
                         const char *expected) {
  return topic_len == (int)strlen(expected) &&
         memcmp(topic, expected, topic_len) == 0;
}

// Helper: Convert byte array to hex string
static void bytes_to_hex(const uint8_t *src, size_t src_len, char *dst) {
  for (size_t i = 0; i < src_len; i++) {
    sprintf(dst + (i * 2), "%02X", src[i]);
  }
  dst[src_len * 2] = '\0';
}

// Helper: Convert hex string to byte array
static size_t hex_to_bytes(const char *src, uint8_t *dst, size_t dst_max_len) {
  size_t len = strlen(src);
  size_t bytes_count = 0;

  for (size_t i = 0; i < len && bytes_count < dst_max_len; i += 2) {
    unsigned int byte;
    if (sscanf(src + i, "%2x", &byte) != 1) break;
    dst[bytes_count++] = (uint8_t)byte;
  }
  return bytes_count;
}

// Helper: Parse colon-separated MAC string to bytes
static void mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes) {
  unsigned int mac[6];
  if (sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", &mac[0], &mac[1], &mac[2],
             &mac[3], &mac[4], &mac[5]) == 6) {
    for (int i = 0; i < 6; i++) {
      mac_bytes[i] = (uint8_t)mac[i];
    }
  }
}

static uint16_t s_proto_tx_seq = 10;

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t read_be16(const uint8_t *p) {
  return ((uint16_t)p[0] << 8) | p[1];
}

static uint64_t read_be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

static void write_be32(uint8_t *p, uint32_t v) {
  p[0] = (v >> 24) & 0xFF;
  p[1] = (v >> 16) & 0xFF;
  p[2] = (v >> 8) & 0xFF;
  p[3] = v & 0xFF;
}

static void write_be16(uint8_t *p, uint16_t v) {
  p[0] = (v >> 8) & 0xFF;
  p[1] = v & 0xFF;
}

static bool verify_frame_auth_tag(const uint8_t *raw_frame, uint16_t payload_len, const uint8_t *auth_tag) {
  gateway_config_t gw_cfg;
  if (config_manager_load(&gw_cfg) != ESP_OK) return false;
  bool key_nonzero = false;
  for (int i = 0; i < 32; i++) {
    if (gw_cfg.local_key[i] != 0) { key_nonzero = true; break; }
  }
  if (!key_nonzero) {
    ESP_LOGW(TAG, "Gateway local_key is all zero; skipping AUTH_TAG check");
    return true;
  }
  uint8_t hmac[32];
  if (!crypto_hmac_sha256(gw_cfg.local_key, 32, raw_frame, 17U + payload_len, hmac)) {
    return false;
  }
  return memcmp(hmac, auth_tag, 8) == 0;
}

static void handle_proto_v2_binary_cmd(const uint8_t *data, uint16_t len) {
  ble_frame_t frame;
  const uint8_t *auth_tag = NULL;
  bool has_auth = false;

  if (!ble_frame_parse_with_auth(data, len, &frame, &auth_tag, &has_auth)) {
    ESP_LOGW(TAG, "Invalid protocol v2 binary frame (len=%u)", len);
    return;
  }

  ESP_LOGI(TAG, "Protocol v2 frame: type=0x%02X cmd=0x%02X seq=%u status=0x%02X len=%u auth=%d",
           frame.type, frame.cmd, frame.seq, frame.status, frame.len, has_auth);

  if (frame.cmd == BLE_CMD_FIRMWARE_UPDATE ||
      frame.cmd == BLE_CMD_FIRMWARE_ABORT ||
      frame.cmd == BLE_CMD_GET_FIRMWARE_INFO) {
    if (has_auth && auth_tag != NULL && !verify_frame_auth_tag(data, frame.len, auth_tag)) {
      ESP_LOGE(TAG, "AUTH_TAG verification FAILED for cmd 0x%02X!", frame.cmd);
      return;
    }
  }

  if (frame.cmd == BLE_CMD_FIRMWARE_UPDATE) {
    if (frame.len < 85) {
      ESP_LOGE(TAG, "FIRMWARE_UPDATE (0x53) payload too short (%u)", frame.len);
      return;
    }
    const uint8_t *pl = frame.payload;
    uint16_t url_len = read_be16(pl + 83);
    if (frame.len < 85U + url_len || url_len >= 512) {
      ESP_LOGE(TAG, "FIRMWARE_UPDATE (0x53) invalid urlLen=%u (frame.len=%u)", url_len, frame.len);
      return;
    }

    ota_v2_params_t p;
    memset(&p, 0, sizeof(p));
    p.request_id = read_be32(pl + 0);
    p.firmware_target = pl[4];
    p.product_id = read_be16(pl + 5);
    p.hw_rev = pl[7];
    memcpy(p.sub_device_id, pl + 8, 16);
    p.sub_device_id[16] = '\0';
    memcpy(p.sub_mac, pl + 24, 6);
    p.sub_addr_type = pl[30];
    p.fw_version = read_be32(pl + 31);
    p.plain_fw_length = read_be32(pl + 35);
    memcpy(p.plain_fw_sha256, pl + 39, 32);
    p.plain_fw_crc32 = read_be32(pl + 71);
    p.url_expires_at = read_be64(pl + 75);
    memcpy(p.url, pl + 85, url_len);
    p.url[url_len] = '\0';

    ESP_LOGI(TAG, "FIRMWARE_UPDATE (0x53): reqId=%lu target=0x%02X sub='%s' fwVer=0x%08lX len=%lu",
             (unsigned long)p.request_id, p.firmware_target, p.sub_device_id,
             (unsigned long)p.fw_version, (unsigned long)p.plain_fw_length);

    ota_manager_start_v2_update(&p);
  } else if (frame.cmd == BLE_CMD_FIRMWARE_ABORT) {
    if (frame.len < 5) {
      ESP_LOGE(TAG, "FIRMWARE_ABORT (0x5A) payload too short (%u)", frame.len);
      return;
    }
    uint32_t req_id = read_be32(frame.payload);
    uint8_t reason = frame.payload[4];
    ota_manager_abort(req_id, reason);
  } else if (frame.cmd == BLE_CMD_GET_FIRMWARE_INFO) {
    if (frame.len < 21) {
      ESP_LOGE(TAG, "GET_FIRMWARE_INFO (0x5C) payload too short (%u)", frame.len);
      return;
    }
    uint32_t req_id = read_be32(frame.payload);
    uint8_t target = frame.payload[4];
    char q_dev_id[17] = {0};
    memcpy(q_dev_id, frame.payload + 5, 16);
    q_dev_id[16] = '\0';

    if (target == FW_TARGET_LOCAL_ALL || target == FW_TARGET_UNIT1_ESP) {
      uint8_t info_pl[29];
      memset(info_pl, 0, sizeof(info_pl));
      write_be32(info_pl + 0, req_id);
      info_pl[4] = 1; // count = 1 (GW ESP32)
      info_pl[5] = FW_TARGET_UNIT1_ESP;
      strncpy((char *)(info_pl + 6), s_raw_device_id, 16);
      uint32_t gw_ver = ((uint32_t)GATEWAY_VERSION_MAJOR << 16) |
                        ((uint32_t)GATEWAY_VERSION_MINOR << 8) |
                        (uint32_t)GATEWAY_VERSION_PATCH;
      write_be32(info_pl + 22, gw_ver);
      write_be16(info_pl + 26, 0x0610); // OG0610 product ID
      info_pl[28] = 0x01; // HW rev 1

      uint8_t tx_buf[64];
      uint16_t tx_len = ble_frame_build_event(BLE_CMD_FIRMWARE_INFO, ++s_proto_tx_seq,
                                              info_pl, sizeof(info_pl), tx_buf);
      mqtt_manager_publish_proto_event(tx_buf, tx_len);
    } else if (target == FW_TARGET_UNIT3_BLE_SUB_ESP || target == FW_TARGET_UNIT4_BLE_SUB_STM) {
      if (q_dev_id[0] == '\0') {
        mqtt_manager_publish_firmware_status(req_id, target, FW_STATE_FAILED, 0, BLE_STATUS_SUB_ADDR_REQUIRED, 0, 0);
        return;
      }
      subkey_entry_t sk;
      if (subkey_get_by_device_id(q_dev_id, &sk) != ESP_OK) {
        ESP_LOGE(TAG, "GET_FIRMWARE_INFO: sub-device '%s' not found in NVS", q_dev_id);
        mqtt_manager_publish_firmware_status(req_id, target, FW_STATE_FAILED, 0, BLE_STATUS_SUB_NOT_FOUND, 0, 0);
        return;
      }
      ble_lock_cmd_t ble_cmd;
      memset(&ble_cmd, 0, sizeof(ble_cmd));
      ble_cmd.op_type = BLE_OP_GET_FW_INFO;
      memcpy(ble_cmd.mac, sk.mac, 6);
      ble_cmd.addr_type = sk.addr_type;
      strncpy(ble_cmd.target_device_id, sk.device_id, sizeof(ble_cmd.target_device_id) - 1);
      snprintf(ble_cmd.service_uuid, sizeof(ble_cmd.service_uuid), "%s", ONLOI_NUS_SVC_UUID);
      snprintf(ble_cmd.char_uuid, sizeof(ble_cmd.char_uuid), "%s", ONLOI_NUS_RX_UUID);
      snprintf(ble_cmd.tx_char_uuid, sizeof(ble_cmd.tx_char_uuid), "%s", ONLOI_NUS_TX_UUID);
      snprintf(ble_cmd.request_id, sizeof(ble_cmd.request_id), "%lu", (unsigned long)req_id);
      ble_cmd.sub_ota.ota_request_id = req_id;
      ble_cmd.sub_ota.firmware_target = target;
      ble_cmd.timeout_ms = 8000;
      if (ble_cmd_queue != NULL) {
        xQueueSend(ble_cmd_queue, &ble_cmd, pdMS_TO_TICKS(1000));
      }
    } else {
      mqtt_manager_publish_firmware_status(req_id, target, FW_STATE_FAILED, 0, BLE_STATUS_TARGET_INVALID, 0, 0);
    }
  }
}

void mqtt_manager_publish_proto_event(const uint8_t *frame_data, size_t frame_len) {
  if (!s_mqtt_connected || client == NULL || !frame_data || frame_len == 0) return;
  esp_mqtt_client_publish(client, s_proto_event_topic, (const char *)frame_data, (int)frame_len, 1, 0);
  if (strlen(s_alt_proto_event_topic) > 0 && strcmp(s_proto_event_topic, s_alt_proto_event_topic) != 0) {
    esp_mqtt_client_publish(client, s_alt_proto_event_topic, (const char *)frame_data, (int)frame_len, 1, 0);
  }
}

void mqtt_manager_publish_firmware_status(uint32_t request_id, uint8_t firmware_target,
                                          uint8_t state, uint8_t progress_pct,
                                          uint8_t error_code, uint32_t bytes_transferred,
                                          uint32_t reported_version) {
  uint8_t pl[16];
  write_be32(pl + 0, request_id);
  pl[4] = firmware_target;
  pl[5] = state;
  pl[6] = progress_pct;
  pl[7] = error_code;
  write_be32(pl + 8, bytes_transferred);
  write_be32(pl + 12, reported_version);

  uint8_t tx_buf[48];
  uint16_t tx_len = ble_frame_build_event(BLE_CMD_FIRMWARE_STATUS, ++s_proto_tx_seq, pl, sizeof(pl), tx_buf);
  ESP_LOGI(TAG, "Publishing FIRMWARE_STATUS (0x39): req=%lu target=0x%02X state=0x%02X pct=%u err=0x%02X",
           (unsigned long)request_id, firmware_target, state, progress_pct, error_code);
  mqtt_manager_publish_proto_event(tx_buf, tx_len);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = event_data;

  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT connected to broker!");
    s_mqtt_connected = true;

    {
      // Protocol Step 12: REGISTRATION (0x34) EVENT, QoS 1, retain=true
      uint8_t payload[3];
      payload[0] = (s_session_epoch >> 8) & 0xFF; // sessionEpoch MSB
      payload[1] = s_session_epoch & 0xFF;        // sessionEpoch LSB
      payload[2] = 0x00;                          // status

      uint8_t frame_buf[64];
      uint16_t frame_len =
          ble_frame_build_event(0x34, 1, payload, 3, frame_buf); // seq=1

      esp_mqtt_client_publish(client, s_proto_lifecycle_topic,
                              (const char *)frame_buf, frame_len, 1, 1);
      if (strlen(s_alt_proto_lifecycle_topic) > 0 &&
          strcmp(s_proto_lifecycle_topic, s_alt_proto_lifecycle_topic) != 0) {
        esp_mqtt_client_publish(client, s_alt_proto_lifecycle_topic,
                                (const char *)frame_buf, frame_len, 1, 1);
      }
    }

    // Protocol Step 11: subscribe to server topics (QoS 1)
    esp_mqtt_client_subscribe(client, s_sub_proto_cmd_topic, 1);
    if (strlen(s_alt_sub_proto_cmd_topic) > 0 &&
        strcmp(s_sub_proto_cmd_topic, s_alt_sub_proto_cmd_topic) != 0) {
      esp_mqtt_client_subscribe(client, s_alt_sub_proto_cmd_topic, 1);
    }
    esp_mqtt_client_subscribe(client, s_sub_proto_reply_topic, 1);
    esp_mqtt_client_subscribe(client, s_sub_proto_config_topic, 1);

    // Subscribe to Gateway command topics
    esp_mqtt_client_subscribe(client, s_sub_cmd_topic, 1);
    if (strlen(s_alt_sub_cmd_topic) > 0 &&
        strcmp(s_sub_cmd_topic, s_alt_sub_cmd_topic) != 0) {
      esp_mqtt_client_subscribe(client, s_alt_sub_cmd_topic, 1);
    }
    esp_mqtt_client_subscribe(client, s_sub_ota_topic, 1);
    if (strlen(s_alt_sub_ota_topic) > 0 &&
        strcmp(s_sub_ota_topic, s_alt_sub_ota_topic) != 0) {
      esp_mqtt_client_subscribe(client, s_alt_sub_ota_topic, 1);
    }
    esp_mqtt_client_subscribe(client, s_sub_sub_ota_topic, 1);
    if (strlen(s_alt_sub_sub_ota_topic) > 0 &&
        strcmp(s_sub_sub_ota_topic, s_alt_sub_sub_ota_topic) != 0) {
      esp_mqtt_client_subscribe(client, s_alt_sub_sub_ota_topic, 1);
    }
    esp_mqtt_client_subscribe(client, s_sub_claim_topic, 1);
    if (strlen(s_alt_sub_claim_topic) > 0 &&
        strcmp(s_sub_claim_topic, s_alt_sub_claim_topic) != 0) {
      esp_mqtt_client_subscribe(client, s_alt_sub_claim_topic, 1);
    }
    esp_mqtt_client_subscribe(client, s_sub_unclaim_topic, 1);
    if (strlen(s_alt_sub_unclaim_topic) > 0 &&
        strcmp(s_sub_unclaim_topic, s_alt_sub_unclaim_topic) != 0) {
      esp_mqtt_client_subscribe(client, s_alt_sub_unclaim_topic, 1);
    }
    esp_mqtt_client_subscribe(client, s_sub_keyrot_topic, 1);
    if (strlen(s_alt_sub_keyrot_topic) > 0 &&
        strcmp(s_sub_keyrot_topic, s_alt_sub_keyrot_topic) != 0) {
      esp_mqtt_client_subscribe(client, s_alt_sub_keyrot_topic, 1);
    }
    esp_mqtt_client_subscribe(client, s_sub_subkey_resp_topic, 1);
    if (strlen(s_alt_sub_subkey_resp_topic) > 0 &&
        strcmp(s_sub_subkey_resp_topic, s_alt_sub_subkey_resp_topic) != 0) {
      esp_mqtt_client_subscribe(client, s_alt_sub_subkey_resp_topic, 1);
    }
    ESP_LOGI(TAG, "Subscribed to all Gateway topics");

    // Publish online status (dual topic: colon and raw format)
    char status_payload[192];
    const esp_app_desc_t *app_desc = esp_app_get_description();
    snprintf(status_payload, sizeof(status_payload),
             "{\"status\":\"online\", \"version\":\"%s\", "
             "\"deviceCode\":\"%s\", \"localKeyVersion\":1}",
             app_desc->version, GATEWAY_DEVICE_CODE);
    esp_mqtt_client_publish(client, s_pub_status_topic, status_payload, 0, 1,
                            1); // retained
    if (strlen(s_alt_pub_status_topic) > 0 &&
        strcmp(s_pub_status_topic, s_alt_pub_status_topic) != 0) {
      esp_mqtt_client_publish(client, s_alt_pub_status_topic, status_payload, 0,
                              1, 1);
    }
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT disconnected from broker.");
    s_mqtt_connected = false;
    break;

  case MQTT_EVENT_DATA: {
    ESP_LOGI(TAG, "MQTT incoming message received. Topic: %.*s",
             event->topic_len, event->topic);

    // Protocol v2 binary frame topics (and binary SOF=0x4F on /ota)
    if (topic_equals(event->topic, event->topic_len, s_sub_proto_cmd_topic) ||
        (strlen(s_alt_sub_proto_cmd_topic) > 0 &&
         topic_equals(event->topic, event->topic_len, s_alt_sub_proto_cmd_topic)) ||
        topic_equals(event->topic, event->topic_len, s_sub_proto_reply_topic) ||
        topic_equals(event->topic, event->topic_len, s_sub_proto_config_topic) ||
        ((topic_equals(event->topic, event->topic_len, s_sub_ota_topic) ||
          (strlen(s_alt_sub_ota_topic) > 0 &&
           topic_equals(event->topic, event->topic_len, s_alt_sub_ota_topic))) &&
         event->data_len >= 19 && (uint8_t)event->data[0] == BLE_FRAME_SOF)) {
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, event->data, event->data_len, ESP_LOG_DEBUG);
      handle_proto_v2_binary_cmd((const uint8_t *)event->data, (uint16_t)event->data_len);
      break;
    }

    char *payload = malloc(event->data_len + 1);
    if (payload == NULL) {
      ESP_LOGE(TAG, "Out of memory to allocate payload buffer.");
      break;
    }
    memcpy(payload, event->data, event->data_len);
    payload[event->data_len] = '\0';
    ESP_LOGI(TAG, "Payload: %s", payload);

    // 1. /gateway/{id}/subkey_response
    if (topic_equals(event->topic, event->topic_len, s_sub_subkey_resp_topic) ||
        (strlen(s_alt_sub_subkey_resp_topic) > 0 &&
         topic_equals(event->topic, event->topic_len, s_alt_sub_subkey_resp_topic))) {
      cJSON *root = cJSON_Parse(payload);
      if (root != NULL) {
        cJSON *dev_id_item = cJSON_GetObjectItem(root, "deviceId");
        cJSON *key_b64_item = cJSON_GetObjectItem(root, "localKey");
        cJSON *ver_item = cJSON_GetObjectItem(root, "keyVersion");
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
        cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");

        if (cJSON_IsString(dev_id_item) && cJSON_IsString(key_b64_item)) {
          uint8_t raw_key[32];
          size_t out_len = 0;
          int ret = mbedtls_base64_decode(raw_key, sizeof(raw_key), &out_len,
                                          (const unsigned char *)key_b64_item->valuestring,
                                          strlen(key_b64_item->valuestring));
          if (ret == 0 && out_len == 32) {
            subkey_entry_t entry;
            memset(&entry, 0, sizeof(entry));
            strncpy(entry.device_id, dev_id_item->valuestring, sizeof(entry.device_id) - 1);
            entry.key_version = (ver_item && cJSON_IsNumber(ver_item)) ? ver_item->valueint : 1;
            entry.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item)) ? addr_type_item->valueint : 0;
            if (cJSON_IsString(mac_item)) {
              mac_str_to_bytes(mac_item->valuestring, entry.mac);
            }
            memcpy(entry.local_key, raw_key, 32);
            subkey_put(&entry);
            ESP_LOGI(TAG, "Subkey for device '%s' saved to NVS via subkey_response", entry.device_id);

            if (s_subkey_req_sem != NULL) {
              xSemaphoreGive(s_subkey_req_sem);
            }
          } else {
            ESP_LOGE(TAG, "Failed to base64-decode localKey (ret=%d, len=%u)", ret, (unsigned)out_len);
          }
        }
        cJSON_Delete(root);
      }
    }
    // 2. /gateway/{id}/command
    else if (topic_equals(event->topic, event->topic_len, s_sub_cmd_topic) ||
        (strlen(s_alt_sub_cmd_topic) > 0 &&
         topic_equals(event->topic, event->topic_len, s_alt_sub_cmd_topic))) {
      cJSON *root = cJSON_Parse(payload);
      if (root != NULL) {
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
        cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");
        cJSON *cmd_item = cJSON_GetObjectItem(root, "command");
        cJSON *srv_uuid_item = cJSON_GetObjectItem(root, "service_uuid");
        cJSON *chr_uuid_item = cJSON_GetObjectItem(root, "char_uuid");
        cJSON *tx_chr_uuid_item = cJSON_GetObjectItem(root, "tx_char_uuid");
        cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
        cJSON *target_id = cJSON_GetObjectItem(root, "targetDeviceId");
        cJSON *timeout_item = cJSON_GetObjectItem(root, "timeout_ms");
        cJSON *enc_item = cJSON_GetObjectItem(root, "is_encrypted");

        if (cJSON_IsString(cmd_item) && (cJSON_IsString(mac_item) || cJSON_IsString(target_id))) {
          ble_lock_cmd_t ble_cmd;
          memset(&ble_cmd, 0, sizeof(ble_lock_cmd_t));
          ble_cmd.op_type = BLE_OP_PASSTHROUGH_CMD;

          if (cJSON_IsString(mac_item)) {
            mac_str_to_bytes(mac_item->valuestring, ble_cmd.mac);
          }
          if (cJSON_IsString(target_id)) {
            snprintf(ble_cmd.target_device_id, sizeof(ble_cmd.target_device_id), "%s", target_id->valuestring);
          }

          // If MAC is all zeroes, resolve from subkey store by targetDeviceId
          if (ble_cmd.mac[0] == 0 && ble_cmd.mac[1] == 0 && ble_cmd.mac[2] == 0 &&
              ble_cmd.mac[3] == 0 && ble_cmd.mac[4] == 0 && ble_cmd.mac[5] == 0) {
            subkey_entry_t sk;
            if (subkey_get_by_device_id(ble_cmd.target_device_id, &sk) == ESP_OK) {
              memcpy(ble_cmd.mac, sk.mac, 6);
              ble_cmd.addr_type = sk.addr_type;
            }
          } else {
            ble_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item))
                                    ? addr_type_item->valueint : 0;
          }

          if (cJSON_IsString(srv_uuid_item) && strlen(srv_uuid_item->valuestring) > 0) {
            snprintf(ble_cmd.service_uuid, sizeof(ble_cmd.service_uuid), "%s", srv_uuid_item->valuestring);
          } else {
            snprintf(ble_cmd.service_uuid, sizeof(ble_cmd.service_uuid), "%s", ONLOI_NUS_SVC_UUID);
          }

          if (cJSON_IsString(chr_uuid_item) && strlen(chr_uuid_item->valuestring) > 0) {
            snprintf(ble_cmd.char_uuid, sizeof(ble_cmd.char_uuid), "%s", chr_uuid_item->valuestring);
          } else {
            snprintf(ble_cmd.char_uuid, sizeof(ble_cmd.char_uuid), "%s", ONLOI_NUS_RX_UUID);
          }

          if (cJSON_IsString(tx_chr_uuid_item) && strlen(tx_chr_uuid_item->valuestring) > 0) {
            snprintf(ble_cmd.tx_char_uuid, sizeof(ble_cmd.tx_char_uuid), "%s", tx_chr_uuid_item->valuestring);
          } else {
            snprintf(ble_cmd.tx_char_uuid, sizeof(ble_cmd.tx_char_uuid), "%s", ONLOI_NUS_TX_UUID);
          }

          if (req_id && cJSON_IsString(req_id)) {
            snprintf(ble_cmd.request_id, sizeof(ble_cmd.request_id), "%s", req_id->valuestring);
          } else if (req_id && cJSON_IsNumber(req_id)) {
            snprintf(ble_cmd.request_id, sizeof(ble_cmd.request_id), "%d", req_id->valueint);
          }

          ble_cmd.timeout_ms = (timeout_item && cJSON_IsNumber(timeout_item)) ? timeout_item->valueint : 8000;
          ble_cmd.is_encrypted = (enc_item && cJSON_IsBool(enc_item)) ? cJSON_IsTrue(enc_item) : false;
          ble_cmd.cmd_len = hex_to_bytes(cmd_item->valuestring, ble_cmd.cmd_data, sizeof(ble_cmd.cmd_data));

          ESP_LOGI(TAG, "Decoded incoming passthrough command for ReqID: %s, target: %s, len: %u",
                   ble_cmd.request_id, ble_cmd.target_device_id, ble_cmd.cmd_len);

          if (ble_cmd_queue != NULL) {
            if (xQueueSend(ble_cmd_queue, &ble_cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
              ESP_LOGE(TAG, "Failed to push command to ble_cmd_queue (queue full).");
              mqtt_manager_publish_command_ack(ble_cmd.request_id, ble_cmd.target_device_id, ble_cmd.mac, "error", "Queue full");
            }
          }
        } else {
          ESP_LOGE(TAG, "Invalid fields in JSON command payload (mac/targetDeviceId or command missing).");
        }
        cJSON_Delete(root);
      }
    }
    // 3. /gateway/{id}/ota
    else if (topic_equals(event->topic, event->topic_len, s_sub_ota_topic) ||
               (strlen(s_alt_sub_ota_topic) > 0 &&
                topic_equals(event->topic, event->topic_len, s_alt_sub_ota_topic))) {
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
          if (cJSON_IsString(ver_item))
            strncpy(ota_params.version, ver_item->valuestring, sizeof(ota_params.version) - 1);
          if (cJSON_IsString(sha_item))
            strncpy(ota_params.sha256, sha_item->valuestring, sizeof(ota_params.sha256) - 1);
          if (cJSON_IsString(sig_item))
            strncpy(ota_params.signature, sig_item->valuestring, sizeof(ota_params.signature) - 1);

          ESP_LOGI(TAG, "Received OTA update request: URL=%s, Ver=%s, SHA=%s",
                   ota_params.url, ota_params.version, ota_params.sha256);
          ota_manager_start_update(&ota_params);
        } else {
          ESP_LOGE(TAG, "Missing 'url' field in OTA JSON payload.");
        }
        cJSON_Delete(root);
      }
    }
    // 4. /gateway/{id}/sub_ota
    else if (topic_equals(event->topic, event->topic_len, s_sub_sub_ota_topic) ||
               (strlen(s_alt_sub_sub_ota_topic) > 0 &&
                topic_equals(event->topic, event->topic_len, s_alt_sub_sub_ota_topic))) {
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
          ble_lock_cmd_t sub_ota_cmd;
          memset(&sub_ota_cmd, 0, sizeof(ble_lock_cmd_t));
          sub_ota_cmd.op_type = BLE_OP_PASSTHROUGH_CMD;

          mac_str_to_bytes(mac_item->valuestring, sub_ota_cmd.mac);
          sub_ota_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item)) ? addr_type_item->valueint : 0;
          if (cJSON_IsString(target_id)) {
            snprintf(sub_ota_cmd.target_device_id, sizeof(sub_ota_cmd.target_device_id), "%s", target_id->valuestring);
          }
          if (cJSON_IsString(srv_uuid_item) && strlen(srv_uuid_item->valuestring) > 0) {
            snprintf(sub_ota_cmd.service_uuid, sizeof(sub_ota_cmd.service_uuid), "%s", srv_uuid_item->valuestring);
          } else {
            snprintf(sub_ota_cmd.service_uuid, sizeof(sub_ota_cmd.service_uuid), "%s", ONLOI_NUS_SVC_UUID);
          }
          if (cJSON_IsString(chr_uuid_item) && strlen(chr_uuid_item->valuestring) > 0) {
            snprintf(sub_ota_cmd.char_uuid, sizeof(sub_ota_cmd.char_uuid), "%s", chr_uuid_item->valuestring);
          } else {
            snprintf(sub_ota_cmd.char_uuid, sizeof(sub_ota_cmd.char_uuid), "%s", ONLOI_NUS_RX_UUID);
          }
          snprintf(sub_ota_cmd.tx_char_uuid, sizeof(sub_ota_cmd.tx_char_uuid), "%s", ONLOI_NUS_TX_UUID);

          if (req_id && cJSON_IsString(req_id)) {
            snprintf(sub_ota_cmd.request_id, sizeof(sub_ota_cmd.request_id), "%s", req_id->valuestring);
          } else if (req_id && cJSON_IsNumber(req_id)) {
            snprintf(sub_ota_cmd.request_id, sizeof(sub_ota_cmd.request_id), "%d", req_id->valueint);
          }

          sub_ota_cmd.timeout_ms = 8000;
          sub_ota_cmd.cmd_len = hex_to_bytes(cmd_item->valuestring, sub_ota_cmd.cmd_data, sizeof(sub_ota_cmd.cmd_data));

          if (ble_cmd_queue != NULL) {
            xQueueSend(ble_cmd_queue, &sub_ota_cmd, pdMS_TO_TICKS(1000));
          }

          cJSON *ack = cJSON_CreateObject();
          cJSON_AddStringToObject(ack, "result", "relayed");
          if (strlen(sub_ota_cmd.target_device_id) > 0) {
            cJSON_AddStringToObject(ack, "targetDeviceId", sub_ota_cmd.target_device_id);
          }
          if (req_id) {
            if (cJSON_IsString(req_id))
              cJSON_AddStringToObject(ack, "requestId", req_id->valuestring);
            else if (cJSON_IsNumber(req_id))
              cJSON_AddNumberToObject(ack, "requestId", req_id->valueint);
          }
          char *ack_str = cJSON_PrintUnformatted(ack);
          if (ack_str != NULL) {
            esp_mqtt_client_publish(client, s_pub_sub_ota_ack_topic, ack_str, 0, 1, 0);
            if (strlen(s_alt_pub_sub_ota_ack_topic) > 0 && strcmp(s_pub_sub_ota_ack_topic, s_alt_pub_sub_ota_ack_topic) != 0) {
              esp_mqtt_client_publish(client, s_alt_pub_sub_ota_ack_topic, ack_str, 0, 1, 0);
            }
            free(ack_str);
          }
          cJSON_Delete(ack);
        }
        cJSON_Delete(root);
      }
    }
    // 5. /gateway/{id}/device_claim
    else if (topic_equals(event->topic, event->topic_len, s_sub_claim_topic) ||
               (strlen(s_alt_sub_claim_topic) > 0 &&
                topic_equals(event->topic, event->topic_len, s_alt_sub_claim_topic))) {
      cJSON *root = cJSON_Parse(payload);
      if (root != NULL) {
        cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
        cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");
        cJSON *srv_uuid_item = cJSON_GetObjectItem(root, "service_uuid");

        if (cJSON_IsString(mac_item)) {
          ESP_LOGI(TAG, "Enqueuing BLE device_claim for MAC: %s", mac_item->valuestring);

          ble_lock_cmd_t claim_cmd;
          memset(&claim_cmd, 0, sizeof(ble_lock_cmd_t));
          claim_cmd.op_type = BLE_OP_DEVICE_CLAIM;
          mac_str_to_bytes(mac_item->valuestring, claim_cmd.mac);
          claim_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item)) ? addr_type_item->valueint : 0;
          if (cJSON_IsString(srv_uuid_item) && strlen(srv_uuid_item->valuestring) > 0) {
            snprintf(claim_cmd.service_uuid, sizeof(claim_cmd.service_uuid), "%s", srv_uuid_item->valuestring);
          } else {
            snprintf(claim_cmd.service_uuid, sizeof(claim_cmd.service_uuid), "%s", ONLOI_NUS_SVC_UUID);
          }
          snprintf(claim_cmd.char_uuid, sizeof(claim_cmd.char_uuid), "%s", ONLOI_NUS_RX_UUID);
          snprintf(claim_cmd.tx_char_uuid, sizeof(claim_cmd.tx_char_uuid), "%s", ONLOI_NUS_TX_UUID);

          if (req_id && cJSON_IsString(req_id)) {
            snprintf(claim_cmd.request_id, sizeof(claim_cmd.request_id), "%s", req_id->valuestring);
          } else if (req_id && cJSON_IsNumber(req_id)) {
            snprintf(claim_cmd.request_id, sizeof(claim_cmd.request_id), "%d", req_id->valueint);
          }
          claim_cmd.timeout_ms = 15000;

          if (ble_cmd_queue != NULL) {
            if (xQueueSend(ble_cmd_queue, &claim_cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
              ESP_LOGE(TAG, "Failed to push claim request to ble_cmd_queue (queue full).");
              mqtt_manager_publish_claim_ack(claim_cmd.request_id, mac_item->valuestring, NULL, NULL, NULL, 0, "error", "Queue full");
            }
          }
        } else {
          ESP_LOGE(TAG, "Missing 'mac' in device_claim payload");
        }
        cJSON_Delete(root);
      }
    }
    // 6. /gateway/{id}/device_unclaim
    else if (topic_equals(event->topic, event->topic_len, s_sub_unclaim_topic) ||
               (strlen(s_alt_sub_unclaim_topic) > 0 &&
                topic_equals(event->topic, event->topic_len, s_alt_sub_unclaim_topic))) {
      cJSON *root = cJSON_Parse(payload);
      if (root != NULL) {
        cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
        cJSON *target_id = cJSON_GetObjectItem(root, "targetDeviceId");
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
        cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");

        char req_str[64] = {0};
        if (req_id && cJSON_IsString(req_id)) {
          strncpy(req_str, req_id->valuestring, sizeof(req_str) - 1);
        } else if (req_id && cJSON_IsNumber(req_id)) {
          snprintf(req_str, sizeof(req_str), "%d", req_id->valueint);
        }
        char target_dev[32] = {0};
        if (target_id && cJSON_IsString(target_id)) {
          strncpy(target_dev, target_id->valuestring, sizeof(target_dev) - 1);
        }

        ble_lock_cmd_t unclaim_cmd;
        memset(&unclaim_cmd, 0, sizeof(ble_lock_cmd_t));
        unclaim_cmd.op_type = BLE_OP_DEVICE_UNCLAIM;
        strncpy(unclaim_cmd.request_id, req_str, sizeof(unclaim_cmd.request_id) - 1);
        strncpy(unclaim_cmd.target_device_id, target_dev, sizeof(unclaim_cmd.target_device_id) - 1);
        unclaim_cmd.timeout_ms = 10000;

        bool found = false;
        if (cJSON_IsString(mac_item)) {
          mac_str_to_bytes(mac_item->valuestring, unclaim_cmd.mac);
          unclaim_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item)) ? addr_type_item->valueint : 0;
          found = true;
        } else if (strlen(target_dev) > 0) {
          subkey_entry_t sk;
          if (subkey_get_by_device_id(target_dev, &sk) == ESP_OK) {
            memcpy(unclaim_cmd.mac, sk.mac, 6);
            unclaim_cmd.addr_type = sk.addr_type;
            found = true;
          }
        }

        if (!found) {
          ESP_LOGE(TAG, "Unclaim failed: device '%s' not found in local key store", target_dev);
          mqtt_manager_publish_unclaim_ack(req_str, target_dev, "error", "Device not found in NVS");
        } else {
          snprintf(unclaim_cmd.service_uuid, sizeof(unclaim_cmd.service_uuid), "%s", ONLOI_NUS_SVC_UUID);
          snprintf(unclaim_cmd.char_uuid, sizeof(unclaim_cmd.char_uuid), "%s", ONLOI_NUS_RX_UUID);
          snprintf(unclaim_cmd.tx_char_uuid, sizeof(unclaim_cmd.tx_char_uuid), "%s", ONLOI_NUS_TX_UUID);

          if (ble_cmd_queue != NULL) {
            if (xQueueSend(ble_cmd_queue, &unclaim_cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
              ESP_LOGE(TAG, "Failed to push unclaim to ble_cmd_queue (queue full)");
              mqtt_manager_publish_unclaim_ack(req_str, target_dev, "error", "Queue full");
            }
          }
        }
        cJSON_Delete(root);
      }
    }
    // 7. /gateway/{id}/key_rotate
    else if (topic_equals(event->topic, event->topic_len, s_sub_keyrot_topic) ||
               (strlen(s_alt_sub_keyrot_topic) > 0 &&
                topic_equals(event->topic, event->topic_len, s_alt_sub_keyrot_topic))) {
      cJSON *root = cJSON_Parse(payload);
      if (root != NULL) {
        cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
        cJSON *target_id = cJSON_GetObjectItem(root, "targetDeviceId");
        cJSON *ver_item = cJSON_GetObjectItem(root, "currentKeyVersion");
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
        cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");

        char req_str[64] = {0};
        if (req_id && cJSON_IsString(req_id)) {
          strncpy(req_str, req_id->valuestring, sizeof(req_str) - 1);
        } else if (req_id && cJSON_IsNumber(req_id)) {
          snprintf(req_str, sizeof(req_str), "%d", req_id->valueint);
        }
        char target_dev[32] = {0};
        if (target_id && cJSON_IsString(target_id)) {
          strncpy(target_dev, target_id->valuestring, sizeof(target_dev) - 1);
        }

        ble_lock_cmd_t rot_cmd;
        memset(&rot_cmd, 0, sizeof(ble_lock_cmd_t));
        rot_cmd.op_type = BLE_OP_KEY_ROTATE;
        strncpy(rot_cmd.request_id, req_str, sizeof(rot_cmd.request_id) - 1);
        strncpy(rot_cmd.target_device_id, target_dev, sizeof(rot_cmd.target_device_id) - 1);
        rot_cmd.current_key_version = (ver_item && cJSON_IsNumber(ver_item)) ? ver_item->valueint : 1;
        rot_cmd.timeout_ms = 10000;

        bool found = false;
        if (cJSON_IsString(mac_item)) {
          mac_str_to_bytes(mac_item->valuestring, rot_cmd.mac);
          rot_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item)) ? addr_type_item->valueint : 0;
          found = true;
        } else if (strlen(target_dev) > 0) {
          subkey_entry_t sk;
          if (subkey_get_by_device_id(target_dev, &sk) == ESP_OK) {
            memcpy(rot_cmd.mac, sk.mac, 6);
            rot_cmd.addr_type = sk.addr_type;
            found = true;
          }
        }

        if (!found) {
          ESP_LOGE(TAG, "Key rotate failed: device '%s' not found in local key store", target_dev);
          mqtt_manager_publish_keyrot_ack(req_str, target_dev, NULL, 0, "error", "Device not found in NVS");
        } else {
          snprintf(rot_cmd.service_uuid, sizeof(rot_cmd.service_uuid), "%s", ONLOI_NUS_SVC_UUID);
          snprintf(rot_cmd.char_uuid, sizeof(rot_cmd.char_uuid), "%s", ONLOI_NUS_RX_UUID);
          snprintf(rot_cmd.tx_char_uuid, sizeof(rot_cmd.tx_char_uuid), "%s", ONLOI_NUS_TX_UUID);

          if (ble_cmd_queue != NULL) {
            if (xQueueSend(ble_cmd_queue, &rot_cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
              ESP_LOGE(TAG, "Failed to push key rotate to ble_cmd_queue (queue full)");
              mqtt_manager_publish_keyrot_ack(req_str, target_dev, NULL, 0, "error", "Queue full");
            }
          }
        }
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
    ESP_LOGE(TAG, "MQTT event error: %d", event->error_handle->error_type);
    break;

  default:
    break;
  }
}

static void mqtt_publisher_task(void *pvParameters) {
  ble_scan_report_t report;
  ESP_LOGI(TAG, "MQTT Publisher Task started.");

  while (1) {
    if (xQueueReceive(ble_scan_queue, &report, portMAX_DELAY) == pdTRUE) {
      if (!s_mqtt_connected || client == NULL) {
        continue;
      }

      cJSON *root = cJSON_CreateObject();
      if (root == NULL) {
        continue;
      }

      char mac_str[18];
      snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
               report.mac[0], report.mac[1], report.mac[2], report.mac[3],
               report.mac[4], report.mac[5]);

      char hex_data[64] = {0};
      bytes_to_hex(report.data, report.data_len, hex_data);

      cJSON_AddStringToObject(root, "mac", mac_str);
      cJSON_AddNumberToObject(root, "rssi", report.rssi);
      cJSON_AddStringToObject(root, "data", hex_data);
      if (strlen(report.device_code) > 0) {
        cJSON_AddStringToObject(root, "deviceCode", report.device_code);
      }
      cJSON_AddBoolToObject(root, "is_onloi", report.is_onloi_beacon);
      cJSON_AddBoolToObject(root, "claimed", report.claimed);

      char *json_str = cJSON_PrintUnformatted(root);
      if (json_str != NULL) {
        esp_mqtt_client_publish(client, s_pub_scan_topic, json_str, 0, 1, 0);
        if (strlen(s_alt_pub_scan_topic) > 0 &&
            strcmp(s_pub_scan_topic, s_alt_pub_scan_topic) != 0) {
          esp_mqtt_client_publish(client, s_alt_pub_scan_topic, json_str, 0, 1, 0);
        }
        free(json_str);
      }
      cJSON_Delete(root);
    }
  }
}

esp_err_t mqtt_manager_init(const char *uri, uint16_t port,
                            const char *device_id, const char *client_id,
                            const char *username, const char *password,
                            uint16_t session_epoch) {
  ESP_LOGI(TAG, "Initializing MQTT Manager (URI: %s, Port: %d, Device ID: %s)",
           uri, port, device_id);

  if (s_subkey_req_sem == NULL) {
    s_subkey_req_sem = xSemaphoreCreateBinary();
  }

  strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
  s_session_epoch = session_epoch;

  int raw_len = 0;
  for (size_t i = 0; i < strlen(device_id) && raw_len < (int)sizeof(s_raw_device_id) - 1; i++) {
    if (device_id[i] != ':') {
      s_raw_device_id[raw_len++] = device_id[i];
    }
  }
  s_raw_device_id[raw_len] = '\0';

  sprintf(s_pub_scan_topic, "/gateway/%s/scan_report", s_device_id);
  sprintf(s_alt_pub_scan_topic, "/gateway/%s/scan_report", s_raw_device_id);
  sprintf(s_pub_status_topic, "/gateway/%s/status", s_device_id);
  sprintf(s_alt_pub_status_topic, "/gateway/%s/status", s_raw_device_id);
  sprintf(s_pub_notify_topic, "/gateway/%s/notify", s_device_id);
  sprintf(s_alt_pub_notify_topic, "/gateway/%s/notify", s_raw_device_id);
  sprintf(s_pub_ota_status_topic, "/gateway/%s/ota/status", s_device_id);
  sprintf(s_sub_cmd_topic, "/gateway/%s/command", s_device_id);
  sprintf(s_alt_sub_cmd_topic, "/gateway/%s/command", s_raw_device_id);
  sprintf(s_pub_cmd_ack_topic, "/gateway/%s/command/ack", s_device_id);
  sprintf(s_alt_pub_cmd_ack_topic, "/gateway/%s/command/ack", s_raw_device_id);
  sprintf(s_sub_ota_topic, "/gateway/%s/ota", s_device_id);
  sprintf(s_alt_sub_ota_topic, "/gateway/%s/ota", s_raw_device_id);
  sprintf(s_sub_sub_ota_topic, "/gateway/%s/sub_ota", s_device_id);
  sprintf(s_alt_sub_sub_ota_topic, "/gateway/%s/sub_ota", s_raw_device_id);
  sprintf(s_pub_sub_ota_ack_topic, "/gateway/%s/sub_ota/ack", s_device_id);
  sprintf(s_alt_pub_sub_ota_ack_topic, "/gateway/%s/sub_ota/ack", s_raw_device_id);
  sprintf(s_sub_claim_topic, "/gateway/%s/device_claim", s_device_id);
  sprintf(s_alt_sub_claim_topic, "/gateway/%s/device_claim", s_raw_device_id);
  sprintf(s_pub_claim_ack_topic, "/gateway/%s/device_claim/ack", s_device_id);
  sprintf(s_alt_pub_claim_ack_topic, "/gateway/%s/device_claim/ack", s_raw_device_id);
  sprintf(s_sub_unclaim_topic, "/gateway/%s/device_unclaim", s_device_id);
  sprintf(s_alt_sub_unclaim_topic, "/gateway/%s/device_unclaim", s_raw_device_id);
  sprintf(s_pub_unclaim_ack_topic, "/gateway/%s/device_unclaim/ack", s_device_id);
  sprintf(s_alt_pub_unclaim_ack_topic, "/gateway/%s/device_unclaim/ack", s_raw_device_id);
  sprintf(s_sub_keyrot_topic, "/gateway/%s/key_rotate", s_device_id);
  sprintf(s_alt_sub_keyrot_topic, "/gateway/%s/key_rotate", s_raw_device_id);
  sprintf(s_pub_keyrot_ack_topic, "/gateway/%s/key_rotate/ack", s_device_id);
  sprintf(s_alt_pub_keyrot_ack_topic, "/gateway/%s/key_rotate/ack", s_raw_device_id);
  sprintf(s_pub_subkey_req_topic, "/gateway/%s/subkey_request", s_device_id);
  sprintf(s_alt_pub_subkey_req_topic, "/gateway/%s/subkey_request", s_raw_device_id);
  sprintf(s_sub_subkey_resp_topic, "/gateway/%s/subkey_response", s_device_id);
  sprintf(s_alt_sub_subkey_resp_topic, "/gateway/%s/subkey_response", s_raw_device_id);

  snprintf(s_proto_lifecycle_topic, sizeof(s_proto_lifecycle_topic), "onloi/v1/d/%s/lifecycle", s_device_id);
  snprintf(s_alt_proto_lifecycle_topic, sizeof(s_alt_proto_lifecycle_topic), "onloi/v1/d/%s/lifecycle", s_raw_device_id);
  snprintf(s_proto_event_topic, sizeof(s_proto_event_topic), "onloi/v1/d/%s/event", s_device_id);
  snprintf(s_alt_proto_event_topic, sizeof(s_alt_proto_event_topic), "onloi/v1/d/%s/event", s_raw_device_id);
  snprintf(s_sub_proto_cmd_topic, sizeof(s_sub_proto_cmd_topic), "onloi/v1/d/%s/cmd", s_device_id);
  snprintf(s_alt_sub_proto_cmd_topic, sizeof(s_alt_sub_proto_cmd_topic), "onloi/v1/d/%s/cmd", s_raw_device_id);
  snprintf(s_sub_proto_reply_topic, sizeof(s_sub_proto_reply_topic), "onloi/v1/d/%s/reply", s_device_id);
  snprintf(s_sub_proto_config_topic, sizeof(s_sub_proto_config_topic), "onloi/v1/d/%s/config", s_device_id);

  ota_manager_set_status_cb(mqtt_manager_publish_ota_status);
  ota_manager_set_fw_status_cb(mqtt_manager_publish_firmware_status);

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
  if (client_id != NULL && strlen(client_id) > 0) {
    strncpy(s_mqtt_client_id, client_id, sizeof(s_mqtt_client_id) - 1);
  } else {
    snprintf(s_mqtt_client_id, sizeof(s_mqtt_client_id), "gw-%s", s_device_id);
  }

  static char s_mqtt_username[64] = {0};
  if (username != NULL) {
    strncpy(s_mqtt_username, username, sizeof(s_mqtt_username) - 1);
  }
  static char s_mqtt_password[64] = {0};
  if (password != NULL) {
    strncpy(s_mqtt_password, password, sizeof(s_mqtt_password) - 1);
  }

  esp_mqtt_transport_t transport = (port == 8883) ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;

  esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.hostname = s_mqtt_host,
      .broker.address.port = port,
      .broker.address.transport = transport,
      .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
      .credentials.username = s_mqtt_username,
      .credentials.authentication.password = s_mqtt_password,
      .credentials.client_id = s_mqtt_client_id,
      .session = {.disable_clean_session = true,
                  .keepalive = MQTT_KEEPALIVE_SEC,
                  .last_will = {.topic = s_pub_status_topic,
                                .msg = "{\"status\":\"offline\"}",
                                .msg_len = 0,
                                .qos = 1,
                                .retain = 1}}};

  client = esp_mqtt_client_init(&mqtt_cfg);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize ESP MQTT Client.");
    return ESP_FAIL;
  }

  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
  return ESP_OK;
}

esp_err_t mqtt_manager_start(void) {
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

  xTaskCreate(&mqtt_publisher_task, "mqtt_pub_task", 4096, NULL, 5, NULL);
  return ESP_OK;
}

esp_err_t mqtt_manager_stop(void) {
  if (client == NULL) return ESP_OK;
  ESP_LOGI(TAG, "Stopping MQTT Client...");
  esp_err_t err = esp_mqtt_client_stop(client);
  if (err == ESP_OK) {
    s_mqtt_connected = false;
    ESP_LOGI(TAG, "MQTT Client stopped.");
  }
  return err;
}

bool mqtt_manager_is_connected(void) { return s_mqtt_connected; }

void mqtt_manager_publish_notification(const uint8_t *mac, const uint8_t *data, size_t data_len) {
  mqtt_manager_publish_notification_ext(mac, NULL, NULL, data, data_len);
}

void mqtt_manager_publish_notification_ext(const uint8_t *mac,
                                           const char *request_id,
                                           const char *target_device_id,
                                           const uint8_t *data,
                                           size_t data_len) {
  mqtt_notify_relay_t relay;
  memset(&relay, 0, sizeof(relay));
  if (mac) memcpy(relay.mac, mac, 6);
  if (request_id) strncpy(relay.request_id, request_id, sizeof(relay.request_id) - 1);
  if (target_device_id) strncpy(relay.target_device_id, target_device_id, sizeof(relay.target_device_id) - 1);
  relay.frame_data = data;
  relay.frame_len = data_len;
  relay.result = "ok";
  mqtt_manager_publish_notify_relay(&relay);
}

void mqtt_manager_publish_notify_relay(const mqtt_notify_relay_t *relay) {
  if (!s_mqtt_connected || client == NULL || relay == NULL) return;

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) return;

  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           relay->mac[0], relay->mac[1], relay->mac[2],
           relay->mac[3], relay->mac[4], relay->mac[5]);
  cJSON_AddStringToObject(root, "mac", mac_str);

  if (strlen(relay->target_device_id) > 0) {
    cJSON_AddStringToObject(root, "targetDeviceId", relay->target_device_id);
  }
  if (strlen(relay->request_id) > 0) {
    cJSON_AddStringToObject(root, "requestId", relay->request_id);
  }

  // Section 7.2: sessionEphemeralPubKeys: {peer: <hex>, device: <hex>}
  if (relay->gw_eph_pub != NULL && relay->dev_eph_pub != NULL) {
    cJSON *eph_obj = cJSON_CreateObject();
    char peer_hex[65] = {0};
    char dev_hex[65] = {0};
    bytes_to_hex(relay->gw_eph_pub, 32, peer_hex);
    bytes_to_hex(relay->dev_eph_pub, 32, dev_hex);
    cJSON_AddStringToObject(eph_obj, "peer", peer_hex);
    cJSON_AddStringToObject(eph_obj, "device", dev_hex);
    cJSON_AddItemToObject(root, "sessionEphemeralPubKeys", eph_obj);
  }

  // Section 7.2: connNonce: <hex>
  if (relay->conn_nonce != NULL) {
    char nonce_hex[17] = {0};
    bytes_to_hex(relay->conn_nonce, 8, nonce_hex);
    cJSON_AddStringToObject(root, "connNonce", nonce_hex);
  }

  // Raw encrypted frame hex
  if (relay->frame_data != NULL && relay->frame_len > 0) {
    char *data_hex = malloc(relay->frame_len * 2 + 1);
    if (data_hex != NULL) {
      bytes_to_hex(relay->frame_data, relay->frame_len, data_hex);
      cJSON_AddStringToObject(root, "data", data_hex);
      free(data_hex);
    }
  }

  // Decrypted plaintext v2.19 Onloi frame hex & CMD code (Automatic Gateway Codec)
  if (relay->has_cmd) {
    cJSON_AddNumberToObject(root, "cmd", relay->cmd_code);
  }
  if (relay->dec_frame_data != NULL && relay->dec_frame_len > 0) {
    char *dec_hex = malloc(relay->dec_frame_len * 2 + 1);
    if (dec_hex != NULL) {
      bytes_to_hex(relay->dec_frame_data, relay->dec_frame_len, dec_hex);
      cJSON_AddStringToObject(root, "decrypted_data", dec_hex);
      free(dec_hex);
    }
  }

  cJSON_AddStringToObject(root, "result", (relay->result != NULL) ? relay->result : "ok");
  if (relay->reason != NULL && strlen(relay->reason) > 0) {
    cJSON_AddStringToObject(root, "reason", relay->reason);
  }

  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str != NULL) {
    ESP_LOGI(TAG, "[NOTIFY RELAY] %s", json_str);
    esp_mqtt_client_publish(client, s_pub_notify_topic, json_str, 0, 1, 0);
    if (strlen(s_alt_pub_notify_topic) > 0 &&
        strcmp(s_pub_notify_topic, s_alt_pub_notify_topic) != 0) {
      esp_mqtt_client_publish(client, s_alt_pub_notify_topic, json_str, 0, 1, 0);
    }
    free(json_str);
  }
  cJSON_Delete(root);
}

void mqtt_manager_publish_command_ack(const char *request_id,
                                      const char *target_device_id,
                                      const uint8_t *mac,
                                      const char *result,
                                      const char *reason) {
  if (!s_mqtt_connected || client == NULL) return;

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) return;

  if (request_id && strlen(request_id) > 0) {
    cJSON_AddStringToObject(root, "requestId", request_id);
  }
  if (target_device_id && strlen(target_device_id) > 0) {
    cJSON_AddStringToObject(root, "targetDeviceId", target_device_id);
  }
  if (mac != NULL) {
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);
  }
  cJSON_AddStringToObject(root, "result", result ? result : "ok");
  if (reason && strlen(reason) > 0) {
    cJSON_AddStringToObject(root, "reason", reason);
  }

  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str != NULL) {
    ESP_LOGI(TAG, "[COMMAND ACK] %s", json_str);
    esp_mqtt_client_publish(client, s_pub_cmd_ack_topic, json_str, 0, 1, 0);
    esp_mqtt_client_publish(client, s_pub_notify_topic, json_str, 0, 1, 0);
    if (strlen(s_alt_pub_notify_topic) > 0 &&
        strcmp(s_pub_notify_topic, s_alt_pub_notify_topic) != 0) {
      esp_mqtt_client_publish(client, s_alt_pub_notify_topic, json_str, 0, 1, 0);
    }
    free(json_str);
  }
  cJSON_Delete(root);
}

void mqtt_manager_publish_ota_status(const char *status, const char *reason) {
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

void mqtt_manager_publish_claim_ack(const char *request_id, const char *mac,
                                    const char *device_id,
                                    const char *device_code,
                                    const char *local_key_b64, int key_version,
                                    const char *result, const char *error_msg) {
  if (!s_mqtt_connected || client == NULL) return;

  cJSON *ack = cJSON_CreateObject();
  if (ack == NULL) return;

  if (request_id && strlen(request_id) > 0) {
    cJSON_AddStringToObject(ack, "requestId", request_id);
  }
  if (mac && strlen(mac) > 0) {
    cJSON_AddStringToObject(ack, "mac", mac);
  }
  if (device_id && strlen(device_id) > 0) {
    cJSON_AddStringToObject(ack, "deviceId", device_id);
  }
  if (device_code && strlen(device_code) > 0) {
    cJSON_AddStringToObject(ack, "deviceCode", device_code);
  }
  if (local_key_b64 && strlen(local_key_b64) > 0) {
    cJSON_AddStringToObject(ack, "localKey", local_key_b64);
    cJSON_AddNumberToObject(ack, "keyVersion", key_version > 0 ? key_version : 1);
  }
  cJSON_AddStringToObject(ack, "result", result ? result : "ok");
  if (error_msg && strlen(error_msg) > 0) {
    cJSON_AddStringToObject(ack, "error", error_msg);
  }

  char *ack_str = cJSON_PrintUnformatted(ack);
  if (ack_str != NULL) {
    ESP_LOGI(TAG, "[PUBLISH CLAIM ACK] %s", ack_str);
    esp_mqtt_client_publish(client, s_pub_claim_ack_topic, ack_str, 0, 1, 0);
    if (strlen(s_alt_pub_claim_ack_topic) > 0 &&
        strcmp(s_pub_claim_ack_topic, s_alt_pub_claim_ack_topic) != 0) {
      esp_mqtt_client_publish(client, s_alt_pub_claim_ack_topic, ack_str, 0, 1, 0);
    }
    free(ack_str);
  }
  cJSON_Delete(ack);
}

void mqtt_manager_publish_unclaim_ack(const char *request_id,
                                      const char *target_device_id,
                                      const char *result,
                                      const char *reason) {
  if (!s_mqtt_connected || client == NULL) return;

  cJSON *ack = cJSON_CreateObject();
  if (ack == NULL) return;

  if (request_id && strlen(request_id) > 0) {
    cJSON_AddStringToObject(ack, "requestId", request_id);
  }
  if (target_device_id && strlen(target_device_id) > 0) {
    cJSON_AddStringToObject(ack, "targetDeviceId", target_device_id);
  }
  cJSON_AddStringToObject(ack, "result", result ? result : "ok");
  if (reason && strlen(reason) > 0) {
    cJSON_AddStringToObject(ack, "reason", reason);
  }

  char *ack_str = cJSON_PrintUnformatted(ack);
  if (ack_str != NULL) {
    ESP_LOGI(TAG, "[PUBLISH UNCLAIM ACK] %s", ack_str);
    esp_mqtt_client_publish(client, s_pub_unclaim_ack_topic, ack_str, 0, 1, 0);
    if (strlen(s_alt_pub_unclaim_ack_topic) > 0 &&
        strcmp(s_pub_unclaim_ack_topic, s_alt_pub_unclaim_ack_topic) != 0) {
      esp_mqtt_client_publish(client, s_alt_pub_unclaim_ack_topic, ack_str, 0, 1, 0);
    }
    free(ack_str);
  }
  cJSON_Delete(ack);
}

void mqtt_manager_publish_keyrot_ack(const char *request_id,
                                     const char *target_device_id,
                                     const char *new_local_key_b64,
                                     int key_version,
                                     const char *result,
                                     const char *reason) {
  if (!s_mqtt_connected || client == NULL) return;

  cJSON *ack = cJSON_CreateObject();
  if (ack == NULL) return;

  if (request_id && strlen(request_id) > 0) {
    cJSON_AddStringToObject(ack, "requestId", request_id);
  }
  if (target_device_id && strlen(target_device_id) > 0) {
    cJSON_AddStringToObject(ack, "targetDeviceId", target_device_id);
  }
  if (new_local_key_b64 && strlen(new_local_key_b64) > 0) {
    cJSON_AddStringToObject(ack, "newLocalKey", new_local_key_b64);
    cJSON_AddNumberToObject(ack, "keyVersion", key_version);
  }
  cJSON_AddStringToObject(ack, "result", result ? result : "ok");
  if (reason && strlen(reason) > 0) {
    cJSON_AddStringToObject(ack, "reason", reason);
  }

  char *ack_str = cJSON_PrintUnformatted(ack);
  if (ack_str != NULL) {
    ESP_LOGI(TAG, "[PUBLISH KEYROT ACK] %s", ack_str);
    esp_mqtt_client_publish(client, s_pub_keyrot_ack_topic, ack_str, 0, 1, 0);
    if (strlen(s_alt_pub_keyrot_ack_topic) > 0 &&
        strcmp(s_pub_keyrot_ack_topic, s_alt_pub_keyrot_ack_topic) != 0) {
      esp_mqtt_client_publish(client, s_alt_pub_keyrot_ack_topic, ack_str, 0, 1, 0);
    }
    free(ack_str);
  }
  cJSON_Delete(ack);
}

esp_err_t mqtt_manager_request_subkey(const char *device_id, uint32_t timeout_ms) {
  if (!device_id || strlen(device_id) == 0 || !s_mqtt_connected || client == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_subkey_req_sem != NULL) {
    while (xSemaphoreTake(s_subkey_req_sem, 0) == pdTRUE) {}
  }

  cJSON *req = cJSON_CreateObject();
  if (req == NULL) return ESP_ERR_NO_MEM;
  cJSON_AddStringToObject(req, "deviceId", device_id);

  char *req_str = cJSON_PrintUnformatted(req);
  if (req_str == NULL) {
    cJSON_Delete(req);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "[SUBKEY REQUEST] Publishing to %s: %s", s_pub_subkey_req_topic, req_str);
  esp_mqtt_client_publish(client, s_pub_subkey_req_topic, req_str, 0, 1, 0);
  if (strlen(s_alt_pub_subkey_req_topic) > 0 &&
      strcmp(s_pub_subkey_req_topic, s_alt_pub_subkey_req_topic) != 0) {
    esp_mqtt_client_publish(client, s_alt_pub_subkey_req_topic, req_str, 0, 1, 0);
  }
  free(req_str);
  cJSON_Delete(req);

  if (s_subkey_req_sem != NULL) {
    if (xSemaphoreTake(s_subkey_req_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
      subkey_entry_t sk;
      if (subkey_get_by_device_id(device_id, &sk) == ESP_OK) {
        return ESP_OK;
      }
    }
  }
  return ESP_ERR_TIMEOUT;
}
