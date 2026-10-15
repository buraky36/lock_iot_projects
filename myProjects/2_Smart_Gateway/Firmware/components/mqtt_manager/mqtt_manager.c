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
#include "mbedtls/base64.h"
#include "mqtt_client.h"
#include "ota_manager.h"
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
static char s_sub_ota_topic[128] = {0};
static char s_sub_sub_ota_topic[128] = {0};
static char s_pub_sub_ota_ack_topic[128] = {0};
static char s_sub_claim_topic[128] = {0};
static char s_pub_claim_ack_topic[128] = {0};
static char s_sub_unclaim_topic[128] = {0};
static char s_pub_unclaim_ack_topic[128] = {0};
static char s_sub_keyrot_topic[128] = {0};
static char s_pub_keyrot_ack_topic[128] = {0};

static char s_raw_device_id[32] = {0};
static char s_alt_pub_scan_topic[128] = {0};
static char s_alt_pub_status_topic[128] = {0};
static char s_alt_pub_notify_topic[128] = {0};
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

// Protocol v2 topics (onloi/v1/d/<deviceId>/...)
static char s_proto_lifecycle_topic[128] = {0};
static char s_sub_proto_cmd_topic[128] = {0};
static char s_sub_proto_reply_topic[128] = {0};
static char s_sub_proto_config_topic[128] = {0};

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
}

// Helper: Convert hex string to byte array
static size_t hex_to_bytes(const char *src, uint8_t *dst, size_t dst_max_len) {
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
static void mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes) {
  unsigned int mac[6];
  sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", &mac[0], &mac[1], &mac[2],
         &mac[3], &mac[4], &mac[5]);
  for (int i = 0; i < 6; i++) {
    mac_bytes[i] = (uint8_t)mac[i];
  }
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

    // Gateway-specific (JSON) topics
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
                              1, 1); // retained
    }
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT disconnected from broker.");
    s_mqtt_connected = false;
    break;

  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "Subscription acknowledged (Msg ID: %d)", event->msg_id);
    break;

  case MQTT_EVENT_DATA: {
    ESP_LOGI(TAG, "MQTT incoming message received. Topic: %.*s",
             event->topic_len, event->topic);

    // Protocol v2 topics carry binary frames (0x4F ...), not JSON
    if (topic_equals(event->topic, event->topic_len, s_sub_proto_cmd_topic) ||
        topic_equals(event->topic, event->topic_len, s_sub_proto_reply_topic) ||
        topic_equals(event->topic, event->topic_len,
                     s_sub_proto_config_topic)) {
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, event->data, event->data_len, ESP_LOG_INFO);
      ble_frame_t frame;
      if (ble_frame_parse((const uint8_t *)event->data,
                          (uint16_t)event->data_len, &frame)) {
        ESP_LOGI(TAG,
                 "Protocol frame: type=0x%02X cmd=0x%02X seq=%u status=0x%02X "
                 "len=%u",
                 frame.type, frame.cmd, frame.seq, frame.status, frame.len);
        // TODO: dispatch protocol commands (REMOTE_OPEN, FIRMWARE_UPDATE,
        // TIME_SYNC reply, ...)
      } else {
        ESP_LOGW(TAG, "Invalid protocol frame on %.*s", event->topic_len,
                 event->topic);
      }
      break;
    }

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
    if (topic_equals(event->topic, event->topic_len, s_sub_cmd_topic) ||
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

        if (cJSON_IsString(mac_item) && cJSON_IsString(cmd_item)) {
          ble_lock_cmd_t ble_cmd;
          memset(&ble_cmd, 0, sizeof(ble_lock_cmd_t));

          mac_str_to_bytes(mac_item->valuestring, ble_cmd.mac);
          ble_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item))
                                  ? addr_type_item->valueint
                                  : 0;

          // Default to Onloi Nordic UART Service if not specified
          if (cJSON_IsString(srv_uuid_item) &&
              strlen(srv_uuid_item->valuestring) > 0) {
            snprintf(ble_cmd.service_uuid, sizeof(ble_cmd.service_uuid), "%s",
                     srv_uuid_item->valuestring);
          } else {
            snprintf(ble_cmd.service_uuid, sizeof(ble_cmd.service_uuid), "%s",
                     ONLOI_NUS_SVC_UUID);
          }

          if (cJSON_IsString(chr_uuid_item) &&
              strlen(chr_uuid_item->valuestring) > 0) {
            snprintf(ble_cmd.char_uuid, sizeof(ble_cmd.char_uuid), "%s",
                     chr_uuid_item->valuestring);
          } else {
            snprintf(ble_cmd.char_uuid, sizeof(ble_cmd.char_uuid), "%s",
                     ONLOI_NUS_RX_UUID);
          }

          if (cJSON_IsString(tx_chr_uuid_item) &&
              strlen(tx_chr_uuid_item->valuestring) > 0) {
            snprintf(ble_cmd.tx_char_uuid, sizeof(ble_cmd.tx_char_uuid), "%s",
                     tx_chr_uuid_item->valuestring);
          } else {
            snprintf(ble_cmd.tx_char_uuid, sizeof(ble_cmd.tx_char_uuid), "%s",
                     ONLOI_NUS_TX_UUID);
          }

          if (req_id && cJSON_IsString(req_id)) {
            snprintf(ble_cmd.request_id, sizeof(ble_cmd.request_id), "%s",
                     req_id->valuestring);
          } else if (req_id && cJSON_IsNumber(req_id)) {
            snprintf(ble_cmd.request_id, sizeof(ble_cmd.request_id), "%d",
                     req_id->valueint);
          }

          if (target_id && cJSON_IsString(target_id)) {
            snprintf(ble_cmd.target_device_id, sizeof(ble_cmd.target_device_id),
                     "%s", target_id->valuestring);
          }

          ble_cmd.timeout_ms = (timeout_item && cJSON_IsNumber(timeout_item))
                                   ? timeout_item->valueint
                                   : 8000;
          ble_cmd.cmd_len =
              hex_to_bytes(cmd_item->valuestring, ble_cmd.cmd_data,
                           sizeof(ble_cmd.cmd_data));

          ESP_LOGI(
              TAG,
              "Decoded incoming lock command for MAC %s, ReqID: %s, len: %u",
              mac_item->valuestring, ble_cmd.request_id, ble_cmd.cmd_len);

          // Push command request to BLE Manager queue
          if (ble_cmd_queue != NULL) {
            if (xQueueSend(ble_cmd_queue, &ble_cmd, pdMS_TO_TICKS(1000)) !=
                pdTRUE) {
              ESP_LOGE(TAG,
                       "Failed to push command to ble_cmd_queue (queue full).");
            }
          }
        } else {
          ESP_LOGE(TAG, "Invalid fields in JSON command payload (mac or "
                        "command missing).");
        }
        cJSON_Delete(root);
      }
    } else if (topic_equals(event->topic, event->topic_len, s_sub_ota_topic) ||
               (strlen(s_alt_sub_ota_topic) > 0 &&
                topic_equals(event->topic, event->topic_len,
                             s_alt_sub_ota_topic))) {
      cJSON *root = cJSON_Parse(payload);
      if (root != NULL) {
        cJSON *url_item = cJSON_GetObjectItem(root, "url");
        cJSON *ver_item = cJSON_GetObjectItem(root, "version");
        cJSON *sha_item = cJSON_GetObjectItem(root, "sha256");
        cJSON *sig_item = cJSON_GetObjectItem(root, "signature");

        if (cJSON_IsString(url_item)) {
          ota_update_params_t ota_params;
          memset(&ota_params, 0, sizeof(ota_params));
          strncpy(ota_params.url, url_item->valuestring,
                  sizeof(ota_params.url) - 1);
          if (cJSON_IsString(ver_item))
            strncpy(ota_params.version, ver_item->valuestring,
                    sizeof(ota_params.version) - 1);
          if (cJSON_IsString(sha_item))
            strncpy(ota_params.sha256, sha_item->valuestring,
                    sizeof(ota_params.sha256) - 1);
          if (cJSON_IsString(sig_item))
            strncpy(ota_params.signature, sig_item->valuestring,
                    sizeof(ota_params.signature) - 1);

          ESP_LOGI(TAG, "Received OTA update request: URL=%s, Ver=%s, SHA=%s",
                   ota_params.url, ota_params.version, ota_params.sha256);
          ota_manager_start_update(&ota_params);
        } else {
          ESP_LOGE(TAG, "Missing 'url' field in OTA JSON payload.");
        }
        cJSON_Delete(root);
      }
    } else if (topic_equals(event->topic, event->topic_len,
                            s_sub_sub_ota_topic) ||
               (strlen(s_alt_sub_sub_ota_topic) > 0 &&
                topic_equals(event->topic, event->topic_len,
                             s_alt_sub_sub_ota_topic))) {
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
          ble_cmd.addr_type = (addr_type_item && cJSON_IsNumber(addr_type_item))
                                  ? addr_type_item->valueint
                                  : 0;
          if (cJSON_IsString(srv_uuid_item))
            snprintf(ble_cmd.service_uuid, sizeof(ble_cmd.service_uuid), "%s",
                     srv_uuid_item->valuestring);
          else
            snprintf(ble_cmd.service_uuid, sizeof(ble_cmd.service_uuid), "%s",
                     ONLOI_NUS_SVC_UUID);
          if (cJSON_IsString(chr_uuid_item))
            snprintf(ble_cmd.char_uuid, sizeof(ble_cmd.char_uuid), "%s",
                     chr_uuid_item->valuestring);
          else
            snprintf(ble_cmd.char_uuid, sizeof(ble_cmd.char_uuid), "%s",
                     ONLOI_NUS_RX_UUID);
          snprintf(ble_cmd.tx_char_uuid, sizeof(ble_cmd.tx_char_uuid), "%s",
                   ONLOI_NUS_TX_UUID);
          if (req_id && cJSON_IsString(req_id))
            snprintf(ble_cmd.request_id, sizeof(ble_cmd.request_id), "%s",
                     req_id->valuestring);
          if (target_id && cJSON_IsString(target_id))
            snprintf(ble_cmd.target_device_id, sizeof(ble_cmd.target_device_id),
                     "%s", target_id->valuestring);
          ble_cmd.timeout_ms = 10000;
          ble_cmd.cmd_len =
              hex_to_bytes(cmd_item->valuestring, ble_cmd.cmd_data,
                           sizeof(ble_cmd.cmd_data));

          ESP_LOGI(TAG, "Relaying sub_ota command to BLE node MAC: %s",
                   mac_item->valuestring);
          if (ble_cmd_queue != NULL) {
            xQueueSend(ble_cmd_queue, &ble_cmd, pdMS_TO_TICKS(1000));
          }

          // Send ack back to /sub_ota/ack
          cJSON *ack = cJSON_CreateObject();
          if (target_id && cJSON_IsString(target_id))
            cJSON_AddStringToObject(ack, "targetDeviceId",
                                    target_id->valuestring);
          if (req_id && cJSON_IsNumber(req_id))
            cJSON_AddNumberToObject(ack, "requestId", req_id->valueint);
          else if (req_id && cJSON_IsString(req_id))
            cJSON_AddStringToObject(ack, "requestId", req_id->valuestring);
          cJSON_AddStringToObject(ack, "result", "relayed");
          char *ack_str = cJSON_PrintUnformatted(ack);
          if (ack_str != NULL) {
            esp_mqtt_client_publish(client, s_pub_sub_ota_ack_topic, ack_str, 0,
                                    1, 0);
            if (strlen(s_alt_pub_sub_ota_ack_topic) > 0 &&
                strcmp(s_pub_sub_ota_ack_topic, s_alt_pub_sub_ota_ack_topic) !=
                    0) {
              esp_mqtt_client_publish(client, s_alt_pub_sub_ota_ack_topic,
                                      ack_str, 0, 1, 0);
            }
            free(ack_str);
          }
          cJSON_Delete(ack);
        }
        cJSON_Delete(root);
      }
    } else if (topic_equals(event->topic, event->topic_len,
                            s_sub_claim_topic) ||
               (strlen(s_alt_sub_claim_topic) > 0 &&
                topic_equals(event->topic, event->topic_len,
                             s_alt_sub_claim_topic))) {
      cJSON *root = cJSON_Parse(payload);
      if (root != NULL) {
        cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
        cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");
        cJSON *srv_uuid_item = cJSON_GetObjectItem(root, "service_uuid");

        if (cJSON_IsString(mac_item)) {
          ESP_LOGI(TAG, "Enqueuing real BLE device_claim for MAC: %s",
                   mac_item->valuestring);

          ble_lock_cmd_t claim_cmd;
          memset(&claim_cmd, 0, sizeof(ble_lock_cmd_t));
          claim_cmd.op_type = BLE_OP_DEVICE_CLAIM;
          mac_str_to_bytes(mac_item->valuestring, claim_cmd.mac);
          claim_cmd.addr_type =
              (addr_type_item && cJSON_IsNumber(addr_type_item))
                  ? addr_type_item->valueint
                  : 0;
          if (cJSON_IsString(srv_uuid_item) &&
              strlen(srv_uuid_item->valuestring) > 0) {
            snprintf(claim_cmd.service_uuid, sizeof(claim_cmd.service_uuid),
                     "%s", srv_uuid_item->valuestring);
          } else {
            snprintf(claim_cmd.service_uuid, sizeof(claim_cmd.service_uuid),
                     "%s", ONLOI_NUS_SVC_UUID);
          }
          snprintf(claim_cmd.char_uuid, sizeof(claim_cmd.char_uuid), "%s",
                   ONLOI_NUS_RX_UUID);
          snprintf(claim_cmd.tx_char_uuid, sizeof(claim_cmd.tx_char_uuid), "%s",
                   ONLOI_NUS_TX_UUID);

          if (req_id && cJSON_IsString(req_id)) {
            snprintf(claim_cmd.request_id, sizeof(claim_cmd.request_id), "%s",
                     req_id->valuestring);
          } else if (req_id && cJSON_IsNumber(req_id)) {
            snprintf(claim_cmd.request_id, sizeof(claim_cmd.request_id), "%d",
                     req_id->valueint);
          }
          claim_cmd.timeout_ms =
              15000; // 15s timeout for complete claim handshake

          if (ble_cmd_queue != NULL) {
            if (xQueueSend(ble_cmd_queue, &claim_cmd, pdMS_TO_TICKS(1000)) !=
                pdTRUE) {
              ESP_LOGE(TAG, "Failed to push claim request to ble_cmd_queue "
                            "(queue full).");
              mqtt_manager_publish_claim_ack(claim_cmd.request_id,
                                             mac_item->valuestring, NULL, NULL,
                                             NULL, 0, "error", "Queue full");
            }
          }
        } else {
          ESP_LOGE(TAG, "Missing 'mac' in device_claim payload");
        }
        cJSON_Delete(root);
      }
    } else if (topic_equals(event->topic, event->topic_len,
                            s_sub_unclaim_topic) ||
               (strlen(s_alt_sub_unclaim_topic) > 0 &&
                topic_equals(event->topic, event->topic_len,
                             s_alt_sub_unclaim_topic))) {
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
          cJSON_AddStringToObject(ack, "targetDeviceId",
                                  target_id->valuestring);
        }
        cJSON_AddStringToObject(ack, "result", "ok");

        char *ack_str = cJSON_PrintUnformatted(ack);
        if (ack_str != NULL) {
          ESP_LOGI(TAG, "[PUBLISH UNCLAIM ACK] %s", ack_str);
          esp_mqtt_client_publish(client, s_pub_unclaim_ack_topic, ack_str, 0,
                                  1, 0);
          if (strlen(s_alt_pub_unclaim_ack_topic) > 0 &&
              strcmp(s_pub_unclaim_ack_topic, s_alt_pub_unclaim_ack_topic) !=
                  0) {
            esp_mqtt_client_publish(client, s_alt_pub_unclaim_ack_topic,
                                    ack_str, 0, 1, 0);
          }
          free(ack_str);
        }
        cJSON_Delete(ack);
        cJSON_Delete(root);
      }
    } else if (topic_equals(event->topic, event->topic_len,
                            s_sub_keyrot_topic) ||
               (strlen(s_alt_sub_keyrot_topic) > 0 &&
                topic_equals(event->topic, event->topic_len,
                             s_alt_sub_keyrot_topic))) {
      cJSON *root = cJSON_Parse(payload);
      if (root != NULL) {
        cJSON *req_id = cJSON_GetObjectItem(root, "requestId");
        cJSON *target_id = cJSON_GetObjectItem(root, "targetDeviceId");
        cJSON *ver_item = cJSON_GetObjectItem(root, "currentKeyVersion");

        int next_ver =
            (ver_item && cJSON_IsNumber(ver_item)) ? ver_item->valueint + 1 : 2;

        uint8_t new_local_key[32];
        esp_fill_random(new_local_key, 32);
        char b64_key[64];
        size_t b64_len = 0;
        mbedtls_base64_encode((unsigned char *)b64_key, sizeof(b64_key),
                              &b64_len, new_local_key, 32);
        b64_key[b64_len] = '\0';

        cJSON *ack = cJSON_CreateObject();
        if (req_id && cJSON_IsString(req_id)) {
          cJSON_AddStringToObject(ack, "requestId", req_id->valuestring);
        } else if (req_id && cJSON_IsNumber(req_id)) {
          cJSON_AddNumberToObject(ack, "requestId", req_id->valueint);
        }
        if (target_id && cJSON_IsString(target_id)) {
          cJSON_AddStringToObject(ack, "targetDeviceId",
                                  target_id->valuestring);
        }
        cJSON_AddStringToObject(ack, "newLocalKey", b64_key);
        cJSON_AddNumberToObject(ack, "keyVersion", next_ver);
        cJSON_AddStringToObject(ack, "result", "ok");

        char *ack_str = cJSON_PrintUnformatted(ack);
        if (ack_str != NULL) {
          ESP_LOGI(TAG, "[PUBLISH KEYROT ACK] %s", ack_str);
          esp_mqtt_client_publish(client, s_pub_keyrot_ack_topic, ack_str, 0, 1,
                                  0);
          if (strlen(s_alt_pub_keyrot_ack_topic) > 0 &&
              strcmp(s_pub_keyrot_ack_topic, s_alt_pub_keyrot_ack_topic) != 0) {
            esp_mqtt_client_publish(client, s_alt_pub_keyrot_ack_topic, ack_str,
                                    0, 1, 0);
          }
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
static void mqtt_publisher_task(void *pvParameters) {
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
      sprintf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", report.mac[0],
              report.mac[1], report.mac[2], report.mac[3], report.mac[4],
              report.mac[5]);

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
        if (strlen(s_alt_pub_scan_topic) > 0 &&
            strcmp(s_pub_scan_topic, s_alt_pub_scan_topic) != 0) {
          esp_mqtt_client_publish(client, s_alt_pub_scan_topic, json_str, 0, 1,
                                  0);
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
  ESP_LOGI(TAG, "Initializing MQTT Client...");

  s_session_epoch = session_epoch;

  // Normalize MAC address: colon-separated format for primary, raw format for
  // secondary
  char col_dev_id[32] = {0};
  char raw_dev_id[32] = {0};
  if (strchr(device_id, ':') != NULL) {
    strncpy(col_dev_id, device_id, sizeof(col_dev_id) - 1);
    int j = 0;
    for (int i = 0; device_id[i] && j < (int)sizeof(raw_dev_id) - 1; i++) {
      if (device_id[i] != ':')
        raw_dev_id[j++] = device_id[i];
    }
  } else if (strlen(device_id) == 12) {
    snprintf(col_dev_id, sizeof(col_dev_id), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
             device_id[0], device_id[1], device_id[2], device_id[3],
             device_id[4], device_id[5], device_id[6], device_id[7],
             device_id[8], device_id[9], device_id[10], device_id[11]);
    strncpy(raw_dev_id, device_id, sizeof(raw_dev_id) - 1);
  } else {
    strncpy(col_dev_id, device_id, sizeof(col_dev_id) - 1);
    strncpy(raw_dev_id, device_id, sizeof(raw_dev_id) - 1);
  }

  strncpy(s_device_id, col_dev_id, sizeof(s_device_id) - 1);
  strncpy(s_raw_device_id, raw_dev_id, sizeof(s_raw_device_id) - 1);

  sprintf(s_pub_scan_topic, "/gateway/%s/scan_report", s_device_id);
  sprintf(s_alt_pub_scan_topic, "/gateway/%s/scan_report", s_raw_device_id);
  sprintf(s_pub_status_topic, "/gateway/%s/status", s_device_id);
  sprintf(s_alt_pub_status_topic, "/gateway/%s/status", s_raw_device_id);
  sprintf(s_pub_notify_topic, "/gateway/%s/notify", s_device_id);
  sprintf(s_alt_pub_notify_topic, "/gateway/%s/notify", s_raw_device_id);
  sprintf(s_pub_ota_status_topic, "/gateway/%s/ota/status", s_device_id);
  sprintf(s_sub_cmd_topic, "/gateway/%s/command", s_device_id);
  sprintf(s_alt_sub_cmd_topic, "/gateway/%s/command", s_raw_device_id);
  sprintf(s_sub_ota_topic, "/gateway/%s/ota", s_device_id);
  sprintf(s_alt_sub_ota_topic, "/gateway/%s/ota", s_raw_device_id);
  sprintf(s_sub_sub_ota_topic, "/gateway/%s/sub_ota", s_device_id);
  sprintf(s_alt_sub_sub_ota_topic, "/gateway/%s/sub_ota", s_raw_device_id);
  sprintf(s_pub_sub_ota_ack_topic, "/gateway/%s/sub_ota/ack", s_device_id);
  sprintf(s_alt_pub_sub_ota_ack_topic, "/gateway/%s/sub_ota/ack",
          s_raw_device_id);
  sprintf(s_sub_claim_topic, "/gateway/%s/device_claim", s_device_id);
  sprintf(s_alt_sub_claim_topic, "/gateway/%s/device_claim", s_raw_device_id);
  sprintf(s_pub_claim_ack_topic, "/gateway/%s/device_claim/ack", s_device_id);
  sprintf(s_alt_pub_claim_ack_topic, "/gateway/%s/device_claim/ack",
          s_raw_device_id);
  sprintf(s_sub_unclaim_topic, "/gateway/%s/device_unclaim", s_device_id);
  sprintf(s_alt_sub_unclaim_topic, "/gateway/%s/device_unclaim",
          s_raw_device_id);
  sprintf(s_pub_unclaim_ack_topic, "/gateway/%s/device_unclaim/ack",
          s_device_id);
  sprintf(s_alt_pub_unclaim_ack_topic, "/gateway/%s/device_unclaim/ack",
          s_raw_device_id);
  sprintf(s_sub_keyrot_topic, "/gateway/%s/key_rotate", s_device_id);
  sprintf(s_alt_sub_keyrot_topic, "/gateway/%s/key_rotate", s_raw_device_id);
  sprintf(s_pub_keyrot_ack_topic, "/gateway/%s/key_rotate/ack", s_device_id);
  sprintf(s_alt_pub_keyrot_ack_topic, "/gateway/%s/key_rotate/ack",
          s_raw_device_id);

  snprintf(s_proto_lifecycle_topic, sizeof(s_proto_lifecycle_topic),
           "onloi/v1/d/%s/lifecycle", s_device_id);
  snprintf(s_alt_proto_lifecycle_topic, sizeof(s_alt_proto_lifecycle_topic),
           "onloi/v1/d/%s/lifecycle", s_raw_device_id);
  snprintf(s_sub_proto_cmd_topic, sizeof(s_sub_proto_cmd_topic),
           "onloi/v1/d/%s/cmd", s_device_id);
  snprintf(s_alt_sub_proto_cmd_topic, sizeof(s_alt_sub_proto_cmd_topic),
           "onloi/v1/d/%s/cmd", s_raw_device_id);
  snprintf(s_sub_proto_reply_topic, sizeof(s_sub_proto_reply_topic),
           "onloi/v1/d/%s/reply", s_device_id);
  snprintf(s_sub_proto_config_topic, sizeof(s_sub_proto_config_topic),
           "onloi/v1/d/%s/config", s_device_id);

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
  // clientId, username and password are taken as-is from the HTTPS claim
  // response (Protocol Step 10)
  static char s_mqtt_client_id[64] = {0};
  static char s_mqtt_username[64] = {0};
  static char s_mqtt_password[128] = {0};
  if (client_id != NULL && strlen(client_id) > 0) {
    strncpy(s_mqtt_client_id, client_id, sizeof(s_mqtt_client_id) - 1);
  } else {
    ESP_LOGW(
        TAG,
        "Claim response has no clientId, falling back to onloi_d_<deviceId>");
    snprintf(s_mqtt_client_id, sizeof(s_mqtt_client_id), "onloi_d_%s",
             s_device_id);
  }
  if (username != NULL)
    strncpy(s_mqtt_username, username, sizeof(s_mqtt_username) - 1);
  if (password != NULL)
    strncpy(s_mqtt_password, password, sizeof(s_mqtt_password) - 1);
  esp_mqtt_transport_t transport = MQTT_TRANSPORT_OVER_TCP;
  if (strncmp(uri, "mqtts://", 8) == 0) {
    transport = MQTT_TRANSPORT_OVER_SSL;
  } else if (port == 8883) {
    // If URI does not specify mqtts and claim returned 8883, note that broker
    // currently runs plain TCP
    transport = MQTT_TRANSPORT_OVER_TCP;
  } else {
    transport = MQTT_TRANSPORT_OVER_TCP;
  }
  ESP_LOGI(
      TAG,
      "MQTT Config -> host=%s port=%u transport=%s clientId=%s username=%s",
      s_mqtt_host, port,
      transport == MQTT_TRANSPORT_OVER_SSL ? "SSL/TLS" : "TCP",
      s_mqtt_client_id, s_mqtt_username);

  esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.hostname = s_mqtt_host,
      .broker.address.port = port,
      .broker.address.transport = transport,
      .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
      .credentials.username = s_mqtt_username,
      .credentials.authentication.password = s_mqtt_password,
      .credentials.client_id = s_mqtt_client_id,
      .session = {.disable_clean_session = true,   // cleanSession: false
                  .keepalive = MQTT_KEEPALIVE_SEC, // keepAlive: 30 s
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

  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                 NULL);

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

  // Spawn publisher background task
  xTaskCreate(&mqtt_publisher_task, "mqtt_pub_task", 4096, NULL, 5, NULL);

  return ESP_OK;
}

esp_err_t mqtt_manager_stop(void) {
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

bool mqtt_manager_is_connected(void) { return s_mqtt_connected; }

void mqtt_manager_publish_notification(const uint8_t *mac, const uint8_t *data,
                                       size_t data_len) {
  mqtt_manager_publish_notification_ext(mac, NULL, NULL, data, data_len);
}

void mqtt_manager_publish_notification_ext(const uint8_t *mac,
                                           const char *request_id,
                                           const char *target_device_id,
                                           const uint8_t *data,
                                           size_t data_len) {
  if (!s_mqtt_connected || client == NULL)
    return;

  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
    return;

  char mac_str[18];
  sprintf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
          mac[3], mac[4], mac[5]);

  char hex_data[256] = {0};
  size_t limit = data_len > 120 ? 120 : data_len;
  for (size_t i = 0; i < limit; i++) {
    sprintf(hex_data + (i * 2), "%02X", data[i]);
  }

  if (request_id && strlen(request_id) > 0) {
    cJSON_AddStringToObject(root, "requestId", request_id);
  }
  if (target_device_id && strlen(target_device_id) > 0) {
    cJSON_AddStringToObject(root, "targetDeviceId", target_device_id);
  }
  cJSON_AddStringToObject(root, "mac", mac_str);
  cJSON_AddStringToObject(root, "data", hex_data);
  cJSON_AddStringToObject(root, "result", "ok");

  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str != NULL) {
    ESP_LOGI(TAG, "[PUBLISH NOTIFY] %s", json_str);
    esp_mqtt_client_publish(client, s_pub_notify_topic, json_str, 0, 1, 0);
    if (strlen(s_alt_pub_notify_topic) > 0 &&
        strcmp(s_pub_notify_topic, s_alt_pub_notify_topic) != 0) {
      esp_mqtt_client_publish(client, s_alt_pub_notify_topic, json_str, 0, 1,
                              0);
    }
    free(json_str);
  }
  cJSON_Delete(root);
}

void mqtt_manager_publish_ota_status(const char *status, const char *reason) {
  if (!s_mqtt_connected || client == NULL)
    return;

  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
    return;

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
  if (!s_mqtt_connected || client == NULL)
    return;

  cJSON *ack = cJSON_CreateObject();
  if (ack == NULL)
    return;

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
    cJSON_AddNumberToObject(ack, "keyVersion",
                            key_version > 0 ? key_version : 1);
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
      esp_mqtt_client_publish(client, s_alt_pub_claim_ack_topic, ack_str, 0, 1,
                              0);
    }
    free(ack_str);
  }
  cJSON_Delete(ack);
}
