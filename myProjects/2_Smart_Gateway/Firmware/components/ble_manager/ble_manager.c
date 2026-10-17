#include "ble_manager.h"
#include "ble_frame_parser.h"
#include "ble_server.h"
#include "crypto_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "gateway_version.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "mbedtls/base64.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include <string.h>

#define SCAN_RATE_LIMIT_MS 3000
#define SCAN_CACHE_ENTRIES 32

typedef struct {
  uint8_t mac[BLE_MAC_LEN];
  int64_t last_report_time_ms;
  bool is_used;
} scan_rate_limit_entry_t;

static scan_rate_limit_entry_t s_scan_cache[SCAN_CACHE_ENTRIES];

static bool should_throttle_scan_report(const uint8_t *mac, int64_t now_ms) {
  int oldest_idx = 0;
  int64_t oldest_time = INT64_MAX;

  for (int i = 0; i < SCAN_CACHE_ENTRIES; i++) {
    if (s_scan_cache[i].is_used &&
        memcmp(s_scan_cache[i].mac, mac, BLE_MAC_LEN) == 0) {
      if ((now_ms - s_scan_cache[i].last_report_time_ms) < SCAN_RATE_LIMIT_MS) {
        return true; // Throttle / skip duplicate within 3s
      }
      s_scan_cache[i].last_report_time_ms = now_ms;
      return false; // Time elapsed, allow report
    }
    if (!s_scan_cache[i].is_used) {
      oldest_idx = i;
      oldest_time = 0;
    } else if (s_scan_cache[i].last_report_time_ms < oldest_time) {
      oldest_time = s_scan_cache[i].last_report_time_ms;
      oldest_idx = i;
    }
  }

  // New device: save in cache and allow report
  memcpy(s_scan_cache[oldest_idx].mac, mac, BLE_MAC_LEN);
  s_scan_cache[oldest_idx].last_report_time_ms = now_ms;
  s_scan_cache[oldest_idx].is_used = true;
  return false;
}

// External MQTT callbacks
extern void mqtt_manager_publish_notification(const uint8_t *mac,
                                              const uint8_t *data,
                                              size_t data_len);
extern void mqtt_manager_publish_notification_ext(const uint8_t *mac,
                                                  const char *request_id,
                                                  const char *target_device_id,
                                                  const uint8_t *data,
                                                  size_t data_len);
extern void
mqtt_manager_publish_claim_ack(const char *request_id, const char *mac,
                               const char *device_id, const char *device_code,
                               const char *local_key_b64, int key_version,
                               const char *result, const char *error_msg);

static const char *TAG = "BLE_MGR";

// Increased stack size for BLE Worker Task to handle X25519 ECDH, HKDF, AES-GCM
// and frame parsing without stack overflow
#define BLE_CMD_TASK_STACK_SIZE 8192

QueueHandle_t ble_scan_queue = NULL;
QueueHandle_t ble_cmd_queue = NULL;

static uint8_t s_own_addr_type;
static bool s_scanning = false;
static bool s_connected = false;
static uint16_t s_conn_handle = 0;
static bool s_ble_synced = false;

static int ble_gap_event(struct ble_gap_event *event, void *arg);

// -- GATT CLIENT INTERNAL STATE --
static SemaphoreHandle_t s_channel_ready_sem = NULL;
static SemaphoreHandle_t s_write_sem = NULL;
static SemaphoreHandle_t s_notify_sem = NULL;

static ble_lock_cmd_t s_current_cmd;
static uint16_t s_svc_start_handle = 0;
static uint16_t s_svc_end_handle = 0;
static uint16_t s_rx_val_handle = 0;
static uint16_t s_tx_val_handle = 0;
static uint16_t s_cccd_handle = 0;
static bool s_svc_found = false;
static bool s_rx_found = false;
static bool s_tx_found = false;
static bool s_channel_ready = false;
static bool s_write_done = false;

// Notification buffer
static uint8_t s_rx_notify_buf[256];
static uint16_t s_rx_notify_len = 0;

// Forward declarations
static void ble_start_service_discovery(uint16_t conn_handle);
static void ble_app_scan(void);
static void ble_app_scan_cancel(void);
static void ble_execute_passthrough_cmd(const ble_lock_cmd_t *cmd);
static void ble_execute_device_claim(const ble_lock_cmd_t *cmd);

static int ble_on_cmd_write(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg) {
  ESP_LOGI(TAG, "Command write complete; status=%d", error->status);
  if (error->status == 0) {
    s_write_done = true;
  } else {
    ESP_LOGE(TAG, "Command write failed; status=%d", error->status);
  }
  if (s_write_sem != NULL) {
    xSemaphoreGive(s_write_sem);
  }
  return 0;
}

static int ble_on_cccd_write(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
  if (error->status == 0) {
    ESP_LOGI(TAG, "CCCD notify subscription complete.");
    s_channel_ready = true;
  } else {
    ESP_LOGE(TAG, "CCCD notify subscription failed; status=%d", error->status);
    s_channel_ready = (s_rx_found && s_rx_val_handle != 0);
  }
  if (s_channel_ready_sem != NULL) {
    xSemaphoreGive(s_channel_ready_sem);
  }
  return 0;
}

static int ble_on_disc_dscs(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc, void *arg) {
  if (error->status == 0) {
    uint16_t uuid16 = ble_uuid_u16(&dsc->uuid.u);
    ESP_LOGI(TAG, "Discovered descriptor handle %d, UUID16: 0x%04X",
             dsc->handle, uuid16);
    if (uuid16 == BLE_GATT_DSC_CLT_CFG_UUID16) {
      s_cccd_handle = dsc->handle;
      ESP_LOGI(TAG, "Found CCCD handle: %d", s_cccd_handle);
    }
  } else if (error->status == BLE_HS_EDONE) {
    if (s_cccd_handle != 0) {
      ESP_LOGI(TAG, "Subscribing to notifications on CCCD handle %d...",
               s_cccd_handle);
      static const uint8_t enable_notify[2] = {0x01, 0x00};
      int rc =
          ble_gattc_write_flat(conn_handle, s_cccd_handle, enable_notify,
                               sizeof(enable_notify), ble_on_cccd_write, NULL);
      if (rc != 0) {
        ESP_LOGE(TAG,
                 "Failed to write CCCD; rc=%d. Falling back to write-only.",
                 rc);
        s_channel_ready = (s_rx_found && s_rx_val_handle != 0);
        if (s_channel_ready_sem != NULL)
          xSemaphoreGive(s_channel_ready_sem);
      }
    } else {
      ESP_LOGW(
          TAG,
          "No CCCD found for TX characteristic. Channel ready for write-only.");
      s_channel_ready = (s_rx_found && s_rx_val_handle != 0);
      if (s_channel_ready_sem != NULL)
        xSemaphoreGive(s_channel_ready_sem);
    }
  } else {
    ESP_LOGE(TAG, "Descriptor discovery failed; status=%d.", error->status);
    s_channel_ready = (s_rx_found && s_rx_val_handle != 0);
    if (s_channel_ready_sem != NULL)
      xSemaphoreGive(s_channel_ready_sem);
  }
  return 0;
}

static int ble_on_disc_all_chrs(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg) {
  if (error->status == 0) {
    char uuid_str[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&chr->uuid.u, uuid_str);
    ESP_LOGI(TAG, "Found char: %s, def_handle=%d, val_handle=%d, props=0x%02X",
             uuid_str, chr->def_handle, chr->val_handle, chr->properties);

    ble_uuid_any_t rx_target, tx_target;
    bool has_rx_target =
        (ble_uuid_from_str(&rx_target, s_current_cmd.char_uuid) == 0);
    bool has_tx_target =
        (ble_uuid_from_str(&tx_target, s_current_cmd.tx_char_uuid) == 0);

    // Match RX characteristic (for writing commands/handshake)
    if (has_rx_target && ble_uuid_cmp(&chr->uuid.u, &rx_target.u) == 0) {
      s_rx_val_handle = chr->val_handle;
      s_rx_found = true;
      ESP_LOGI(TAG, "Matched designated RX characteristic (handle %d)",
               s_rx_val_handle);
    } else if (!s_rx_found &&
               (chr->properties &
                (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP))) {
      s_rx_val_handle = chr->val_handle;
      s_rx_found = true;
      ESP_LOGI(TAG, "Fallback matched write characteristic (handle %d)",
               s_rx_val_handle);
    }

    // Match TX characteristic (for notifications/replies)
    if (has_tx_target && ble_uuid_cmp(&chr->uuid.u, &tx_target.u) == 0) {
      s_tx_val_handle = chr->val_handle;
      s_tx_found = true;
      ESP_LOGI(TAG, "Matched designated TX characteristic (handle %d)",
               s_tx_val_handle);
    } else if (!s_tx_found &&
               (chr->properties &
                (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
      s_tx_val_handle = chr->val_handle;
      s_tx_found = true;
      ESP_LOGI(TAG, "Fallback matched notify characteristic (handle %d)",
               s_tx_val_handle);
    }
  } else if (error->status == BLE_HS_EDONE) {
    ESP_LOGI(TAG,
             "Characteristics discovery finished. RX handle: %d, TX handle: %d",
             s_rx_val_handle, s_tx_val_handle);
    if (s_tx_found && s_tx_val_handle != 0) {
      ESP_LOGI(TAG, "Discovering descriptors for TX characteristic...");
      s_cccd_handle = 0;
      int rc =
          ble_gattc_disc_all_dscs(conn_handle, s_tx_val_handle,
                                  s_svc_end_handle, ble_on_disc_dscs, NULL);
      if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start descriptor discovery; rc=%d.", rc);
        s_channel_ready = (s_rx_found && s_rx_val_handle != 0);
        if (s_channel_ready_sem != NULL)
          xSemaphoreGive(s_channel_ready_sem);
      }
    } else if (s_rx_found && s_rx_val_handle != 0) {
      ESP_LOGI(TAG, "No TX characteristic discovered; ready for write-only.");
      s_channel_ready = true;
      if (s_channel_ready_sem != NULL)
        xSemaphoreGive(s_channel_ready_sem);
    } else {
      ESP_LOGE(TAG, "No suitable RX/TX characteristics found in service!");
      s_channel_ready = false;
      if (s_channel_ready_sem != NULL)
        xSemaphoreGive(s_channel_ready_sem);
    }
  } else {
    ESP_LOGE(TAG, "Characteristics discovery failed; status=%d", error->status);
    s_channel_ready = false;
    if (s_channel_ready_sem != NULL)
      xSemaphoreGive(s_channel_ready_sem);
  }
  return 0;
}

static int ble_on_disc_svc(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg) {
  if (error->status == 0) {
    ESP_LOGI(TAG, "Found target service. Start handle: %d, End handle: %d",
             service->start_handle, service->end_handle);
    s_svc_found = true;
    s_svc_start_handle = service->start_handle;
    s_svc_end_handle = service->end_handle;
    s_rx_val_handle = 0;
    s_tx_val_handle = 0;
    s_cccd_handle = 0;
    s_rx_found = false;
    s_tx_found = false;

    int rc = ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                     service->end_handle, ble_on_disc_all_chrs,
                                     NULL);
    if (rc != 0) {
      ESP_LOGE(TAG, "Failed to initiate char discovery; rc=%d", rc);
      s_channel_ready = false;
      if (s_channel_ready_sem != NULL)
        xSemaphoreGive(s_channel_ready_sem);
    }
  } else if (error->status == BLE_HS_EDONE) {
    if (!s_svc_found) {
      ESP_LOGE(TAG, "Target service not found on peripheral.");
      s_channel_ready = false;
      if (s_channel_ready_sem != NULL)
        xSemaphoreGive(s_channel_ready_sem);
    }
  } else {
    ESP_LOGE(TAG, "Service discovery failed; status=%d", error->status);
    s_channel_ready = false;
    if (s_channel_ready_sem != NULL)
      xSemaphoreGive(s_channel_ready_sem);
  }
  return 0;
}

static void ble_start_service_discovery(uint16_t conn_handle) {
  s_svc_found = false;
  ble_uuid_any_t svc_uuid;
  int rc = ble_uuid_from_str(&svc_uuid, s_current_cmd.service_uuid);
  if (rc == 0) {
    rc = ble_gattc_disc_svc_by_uuid(conn_handle, &svc_uuid.u, ble_on_disc_svc,
                                    NULL);
    if (rc != 0) {
      ESP_LOGE(TAG, "Failed to initiate service discovery; rc=%d", rc);
      s_channel_ready = false;
      if (s_channel_ready_sem != NULL)
        xSemaphoreGive(s_channel_ready_sem);
    }
  } else {
    ESP_LOGE(TAG, "Invalid Service UUID string: %s",
             s_current_cmd.service_uuid);
    s_channel_ready = false;
    if (s_channel_ready_sem != NULL)
      xSemaphoreGive(s_channel_ready_sem);
  }
}

static int ble_on_mtu_exchange(uint16_t conn_handle,
                               const struct ble_gatt_error *error, uint16_t mtu,
                               void *arg) {
  if (error->status == 0) {
    ESP_LOGI(TAG, "BLE MTU exchange successful: MTU=%u bytes", mtu);
  } else {
    ESP_LOGW(TAG,
             "BLE MTU exchange failed (status=%d), continuing with default MTU",
             error->status);
  }
  ble_start_service_discovery(conn_handle);
  return 0;
}

// Start scanning
static void ble_app_scan(void) {
  struct ble_gap_disc_params disc_params;
  int rc;

  if (s_scanning || s_connected) {
    return;
  }

  ESP_LOGI(TAG, "Starting BLE continuous scanning...");

  memset(&disc_params, 0, sizeof(disc_params));
  disc_params.filter_duplicates = 0; // Continuous scan reports
  disc_params.passive = 0;           // Active scanning: sends SCAN_REQ to retrieve scan response
  disc_params.itvl = 0;
  disc_params.window = 0;
  disc_params.filter_policy = 0;
  disc_params.limited = 0;

  rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &disc_params,
                    ble_gap_event, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "Error initiating scan; rc=%d", rc);
    s_scanning = false;
  } else {
    s_scanning = true;
  }
}

// Stop scanning
static void ble_app_scan_cancel(void) {
  if (!s_scanning) {
    return;
  }
  ESP_LOGI(TAG, "Stopping BLE scanning...");
  int rc = ble_gap_disc_cancel();
  if (rc == 0 || rc == BLE_HS_EALREADY) {
    s_scanning = false;
  } else {
    ESP_LOGE(TAG, "Failed to cancel scanning; rc=%d", rc);
  }
}

// BLE Gap event handler
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
  case BLE_GAP_EVENT_DISC: {
    struct ble_hs_adv_fields fields;
    int rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                     event->disc.length_data);
    if (rc != 0) {
      return 0;
    }

    ble_scan_report_t report;
    memset(&report, 0, sizeof(report));
    memcpy(report.mac, event->disc.addr.val, BLE_MAC_LEN);
    report.addr_type = event->disc.addr.type;
    report.rssi = event->disc.rssi;
    report.data_len = event->disc.length_data > BLE_MAX_DATA_LEN
                          ? BLE_MAX_DATA_LEN
                          : event->disc.length_data;
    memcpy(report.data, event->disc.data, report.data_len);
    report.is_onloi_beacon = false;

    // 1. Parse Manufacturer Data:
    // Case A: 0x4F 0x4B at offset 0 (standard Onloi beacon: [0x4F, 0x4B, code[0..5], claimed])
    if (fields.mfg_data != NULL && fields.mfg_data_len >= 9) {
      if (fields.mfg_data[0] == 0x4F && fields.mfg_data[1] == 0x4B) {
        report.is_onloi_beacon = true;
        memcpy(report.device_code, &fields.mfg_data[2], 6);
        report.device_code[6] = '\0';
        report.claimed = (fields.mfg_data[8] == 0x01);
      } else if (fields.mfg_data_len >= 11 && fields.mfg_data[2] == 0x4F &&
                 fields.mfg_data[3] == 0x4B) {
        // Case B: Company ID (2 bytes) followed by 0x4F 0x4B
        report.is_onloi_beacon = true;
        memcpy(report.device_code, &fields.mfg_data[4], 6);
        report.device_code[6] = '\0';
        report.claimed = (fields.mfg_data[10] == 0x01);
      }
    }

    // 2. Parse Service Data UUID16: 0xFFF0 (Onloi Smart Lock deviceCode in Scan Response)
    if (!report.is_onloi_beacon && fields.svc_data_uuid16 != NULL &&
        fields.svc_data_uuid16_len >= 8) {
      uint16_t uuid16 =
          fields.svc_data_uuid16[0] | (fields.svc_data_uuid16[1] << 8);
      if (uuid16 == 0xFFF0) {
        report.is_onloi_beacon = true;
        memcpy(report.device_code, &fields.svc_data_uuid16[2], 6);
        report.device_code[6] = '\0';
      }
    }

    // 3. Parse Complete or Shortened Local Name starting with OK, OD, OG, OB or ONLOI
    if (!report.is_onloi_beacon && fields.name != NULL &&
        fields.name_len >= 2) {
      if (fields.name_len >= 6 && fields.name[0] == 'O' &&
          (fields.name[1] == 'K' || fields.name[1] == 'D' ||
           fields.name[1] == 'G' || fields.name[1] == 'B')) {
        report.is_onloi_beacon = true;
        memcpy(report.device_code, fields.name, 6);
        report.device_code[6] = '\0';
      } else if (fields.name_len >= 5 && memcmp(fields.name, "ONLOI", 5) == 0) {
        report.is_onloi_beacon = true;
        size_t cplen = fields.name_len > 6 ? 6 : fields.name_len;
        memcpy(report.device_code, fields.name, cplen);
        report.device_code[cplen] = '\0';
      }
    }

    // 4. Raw packet fallback search for "OK" signature (0x4F, 0x4B)
    if (!report.is_onloi_beacon && event->disc.length_data >= 9) {
      for (int i = 0; i <= event->disc.length_data - 9; i++) {
        if (event->disc.data[i] == 0x4F && event->disc.data[i + 1] == 0x4B) {
          bool ascii_ok = true;
          for (int c = 0; c < 6; c++) {
            char ch = (char)event->disc.data[i + 2 + c];
            if (ch < 32 || ch > 126) {
              ascii_ok = false;
              break;
            }
          }
          if (ascii_ok) {
            report.is_onloi_beacon = true;
            memcpy(report.device_code, &event->disc.data[i + 2], 6);
            report.device_code[6] = '\0';
            report.claimed = (event->disc.data[i + 8] == 0x01);
            break;
          }
        }
      }
    }

    // Only process and enqueue Onloi beacons!
    // Discard random non-Onloi BLE devices so the queue and MQTT never clog
    if (!report.is_onloi_beacon) {
      break;
    }

    // Apply rate-limiting so we don't spam MQTT every 100ms
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (should_throttle_scan_report(report.mac, now_ms)) {
      break;
    }

    ESP_LOGI(TAG,
             "[SCAN] Discovered Onloi Beacon: MAC=" MACSTR
             ", RSSI=%d, Code=%s, Claimed=%d",
             MAC2STR(report.mac), report.rssi, report.device_code,
             report.claimed);

    // Send to MQTT Queue without blocking
    if (ble_scan_queue != NULL) {
      if (xQueueSend(ble_scan_queue, &report, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[SCAN] Scan queue full, dropping beacon report");
      }
    }
    break;
  }

  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0) {
      ESP_LOGI(TAG, "Connected successfully to peripheral! Conn Handle: %d",
               event->connect.conn_handle);
      s_connected = true;
      s_conn_handle = event->connect.conn_handle;

      // Negotiate MTU before starting service discovery to avoid 20-byte
      // payload truncation
      ESP_LOGI(TAG, "Initiating BLE MTU exchange...");
      int rc = ble_gattc_exchange_mtu(s_conn_handle, ble_on_mtu_exchange, NULL);
      if (rc != 0) {
        ESP_LOGW(TAG,
                 "MTU exchange initiation failed (rc=%d). Proceeding to "
                 "service discovery...",
                 rc);
        ble_start_service_discovery(s_conn_handle);
      }
    } else {
      ESP_LOGE(TAG, "Connection failed; status=%d", event->connect.status);
      s_connected = false;
      s_channel_ready = false;
      if (s_channel_ready_sem != NULL)
        xSemaphoreGive(s_channel_ready_sem);
      ble_app_scan();
    }
    break;

  case BLE_GAP_EVENT_NOTIFY_RX: {
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    ESP_LOGI(TAG, "Notification received from handle %d, length %u",
             event->notify_rx.attr_handle, len);
    if (len > sizeof(s_rx_notify_buf))
      len = sizeof(s_rx_notify_buf);
    os_mbuf_copydata(event->notify_rx.om, 0, len, s_rx_notify_buf);
    s_rx_notify_len = len;

    // For passthrough commands, immediately relay to MQTT
    if (s_current_cmd.op_type == BLE_OP_PASSTHROUGH_CMD) {
      mqtt_manager_publish_notification_ext(
          s_current_cmd.mac, s_current_cmd.request_id,
          s_current_cmd.target_device_id, s_rx_notify_buf, s_rx_notify_len);
    }

    // Signal listener task that notification has arrived
    if (s_notify_sem != NULL) {
      xSemaphoreGive(s_notify_sem);
    }
    break;
  }

  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI(TAG, "Disconnected from peripheral; reason=%d",
             event->disconnect.reason);
    s_connected = false;
    s_conn_handle = 0;
    s_channel_ready = false;
    if (s_channel_ready_sem != NULL)
      xSemaphoreGive(s_channel_ready_sem);
    if (s_notify_sem != NULL)
      xSemaphoreGive(s_notify_sem);
    ble_app_scan();
    break;

  case BLE_GAP_EVENT_DISC_COMPLETE:
    ESP_LOGI(TAG, "BLE scan discovery completed; reason=%d", event->disc_complete.reason);
    s_scanning = false;
    break;

  default:
    break;
  }
  return 0;
}

static void ble_on_sync(void) {
  int rc = ble_hs_util_ensure_addr(0);
  assert(rc == 0);
  rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGE(TAG, "Error determining address type; rc=%d", rc);
    return;
  }

  s_ble_synced = true;
  ESP_LOGI(TAG, "BLE Host synced. Starting default scan.");
  ble_app_scan();

  // Also start peripheral advertising for onboarding
  ble_server_start_adv();
}

static void ble_on_reset(int reason) {
  s_ble_synced = false;
  ESP_LOGW(TAG, "BLE Host reset; reason=%d", reason);
}

// Executes Blind Relay command: writes pre-encrypted bytes to RX char, waits
// for TX notification
static void ble_execute_passthrough_cmd(const ble_lock_cmd_t *cmd) {
  if (s_rx_val_handle == 0) {
    ESP_LOGE(TAG, "Cannot write command: RX characteristic handle not found");
    return;
  }

  ESP_LOGI(TAG, "Writing passthrough command (%u bytes) to handle %d...",
           cmd->cmd_len, s_rx_val_handle);
  s_write_done = false;
  while (xSemaphoreTake(s_write_sem, 0) == pdTRUE) {
  }
  while (xSemaphoreTake(s_notify_sem, 0) == pdTRUE) {
  }

  int rc = ble_gattc_write_flat(s_conn_handle, s_rx_val_handle, cmd->cmd_data,
                                cmd->cmd_len, ble_on_cmd_write, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to initiate command write; rc=%d", rc);
    return;
  }

  // Wait for GATT write ACK
  if (xSemaphoreTake(s_write_sem, pdMS_TO_TICKS(4000)) != pdTRUE ||
      !s_write_done) {
    ESP_LOGE(TAG, "GATT Write Operation failed or timed out.");
    return;
  }
  ESP_LOGI(TAG, "GATT Write confirmed by peripheral.");

  // If TX characteristic was discovered, wait for notification
  if (s_tx_found && s_tx_val_handle != 0) {
    uint32_t wait_timeout = (cmd->timeout_ms > 0) ? cmd->timeout_ms : 8000;
    ESP_LOGI(TAG, "Waiting up to %u ms for peripheral notification...",
             wait_timeout);
    if (xSemaphoreTake(s_notify_sem, pdMS_TO_TICKS(wait_timeout)) == pdTRUE) {
      ESP_LOGI(TAG, "Peripheral notification received successfully.");
      vTaskDelay(pdMS_TO_TICKS(300)); // Grace period for subsequent frames
    } else {
      ESP_LOGW(TAG, "Notification wait timeout (%u ms) reached.", wait_timeout);
    }
  }
}

// Executes Sub-Device Claim according to Onloi BLE Gateway Protocol v1.2
// Section 3:
// 1. KEX_INIT (0x71) -> Lock returns deviceId & deviceCode
// 2. Derives claimSessionKey using HKDF-SHA256
// 3. Generates new localKey & delivers encrypted via KEY_DELIVER (0x72)
// 4. Publishes real /device_claim/ack to MQTT
static void ble_execute_device_claim(const ble_lock_cmd_t *cmd) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           cmd->mac[0], cmd->mac[1], cmd->mac[2], cmd->mac[3], cmd->mac[4],
           cmd->mac[5]);

  ESP_LOGI(TAG, "==================================================");
  ESP_LOGI(TAG, "STARTING SUB-DEVICE CLAIM FOR MAC: %s (ReqID: %s)", mac_str,
           cmd->request_id);
  ESP_LOGI(TAG, "==================================================");

  if (s_rx_val_handle == 0 || s_tx_val_handle == 0) {
    ESP_LOGE(TAG, "Claim failed: RX/TX characteristics missing (rx=%d, tx=%d)",
             s_rx_val_handle, s_tx_val_handle);
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "GATT Chars Missing");
    return;
  }

  // ==========================================
  // STEP 1: KEX_INIT (0x71) REQUEST
  // ==========================================
  uint8_t gw_priv[CRYPTO_X25519_KEY_SIZE];
  uint8_t gw_pub[CRYPTO_X25519_KEY_SIZE];
  if (!crypto_generate_x25519_keypair(gw_priv, gw_pub)) {
    ESP_LOGE(TAG, "Failed to generate Gateway X25519 keypair");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "Keygen Failed");
    return;
  }

  // Payload: [role: 0x01 (GW)][ephemeralPubKey: 32 bytes] = 33 bytes
  uint8_t kex_req_payload[33];
  kex_req_payload[0] = 0x01; // Role: Gateway
  memcpy(&kex_req_payload[1], gw_pub, CRYPTO_X25519_KEY_SIZE);

  uint8_t tx_buf[128];
  uint16_t tx_len = ble_frame_build_req(0x71, 1, kex_req_payload,
                                        sizeof(kex_req_payload), tx_buf);

  s_write_done = false;
  while (xSemaphoreTake(s_write_sem, 0) == pdTRUE) {
  }
  while (xSemaphoreTake(s_notify_sem, 0) == pdTRUE) {
  }

  ESP_LOGI(TAG, "Writing KEX_INIT (0x71) REQUEST (%u bytes) to handle %d...",
           tx_len, s_rx_val_handle);
  int rc = ble_gattc_write_flat(s_conn_handle, s_rx_val_handle, tx_buf, tx_len,
                                ble_on_cmd_write, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to write KEX_INIT frame; rc=%d", rc);
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "KEX_INIT Write Failed");
    return;
  }

  if (xSemaphoreTake(s_write_sem, pdMS_TO_TICKS(4000)) != pdTRUE ||
      !s_write_done) {
    ESP_LOGE(TAG, "KEX_INIT write ACK timed out or failed");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "KEX_INIT Write Ack Timeout");
    return;
  }

  // Wait for KEX_INIT RESPONSE via notification
  ESP_LOGI(TAG, "Waiting for KEX_INIT RESPONSE from lock...");
  if (xSemaphoreTake(s_notify_sem, pdMS_TO_TICKS(6000)) != pdTRUE) {
    ESP_LOGE(TAG, "Timeout waiting for KEX_INIT RESPONSE from lock");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "KEX_INIT Timeout");
    return;
  }

  ble_frame_t kex_resp;
  if (!ble_frame_parse(s_rx_notify_buf, s_rx_notify_len, &kex_resp)) {
    ESP_LOGE(TAG,
             "Failed to parse KEX_INIT RESPONSE frame (CRC or format error)");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "Invalid KEX_INIT Frame");
    return;
  }

  if (kex_resp.cmd != 0x71 || kex_resp.type != 0x02 ||
      kex_resp.status != 0x00 || kex_resp.len < 56) {
    ESP_LOGE(TAG,
             "KEX_INIT RESPONSE invalid or rejected: cmd=0x%02X type=0x%02X "
             "status=0x%02X len=%u",
             kex_resp.cmd, kex_resp.type, kex_resp.status, kex_resp.len);
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "KEX_INIT Rejected");
    return;
  }

  // Lock KEX_INIT response payload:
  // [0..31]  = lock ephemeral public key (32 bytes)
  // [32..47] = lock deviceId (16 bytes ASCII)
  // [48..55] = lock deviceCode (8 bytes ASCII)
  uint8_t lock_pub[CRYPTO_X25519_KEY_SIZE];
  memcpy(lock_pub, kex_resp.payload, CRYPTO_X25519_KEY_SIZE);

  char lock_device_id[17] = {0};
  memcpy(lock_device_id, &kex_resp.payload[32], 16);
  for (int i = 15; i >= 0; i--) {
    if (lock_device_id[i] == '\0' || lock_device_id[i] == ' ')
      lock_device_id[i] = '\0';
    else
      break;
  }

  char lock_device_code[9] = {0};
  memcpy(lock_device_code, &kex_resp.payload[48], 8);
  for (int i = 7; i >= 0; i--) {
    if (lock_device_code[i] == '\0' || lock_device_code[i] == ' ')
      lock_device_code[i] = '\0';
    else
      break;
  }

  ESP_LOGI(TAG,
           "KEX_INIT Handshake Success! Lock deviceId='%s', deviceCode='%s'",
           lock_device_id, lock_device_code);

  // ==========================================
  // STEP 2: Derive claimSessionKey
  // ==========================================
  uint8_t shared_secret[CRYPTO_X25519_KEY_SIZE];
  if (!crypto_ecdh_shared_secret(gw_priv, lock_pub, shared_secret)) {
    ESP_LOGE(TAG, "ECDH shared secret computation failed");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "ECDH Failed");
    return;
  }

  uint8_t device_id_16[16] = {0};
  memcpy(device_id_16, &kex_resp.payload[32], 16);

  const uint8_t *info = (const uint8_t *)"onloi-gw-session-v1";
  uint8_t claim_session_key[CRYPTO_AES_KEY_SIZE];
  if (!crypto_hkdf_sha256(shared_secret, sizeof(shared_secret), device_id_16,
                          sizeof(device_id_16), info, strlen((char *)info),
                          claim_session_key, sizeof(claim_session_key))) {
    ESP_LOGE(TAG, "HKDF session key derivation failed");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "HKDF Failed");
    return;
  }

  // ==========================================
  // STEP 3: KEY_DELIVER (0x72) REQUEST
  // ==========================================
  uint8_t new_local_key[CRYPTO_LOCAL_KEY_SIZE];
  if (!crypto_generate_random(new_local_key, sizeof(new_local_key))) {
    ESP_LOGE(TAG, "RNG failed to generate new localKey");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "RNG Failed");
    return;
  }

  uint8_t iv[CRYPTO_AES_IV_SIZE];
  crypto_generate_random(iv, sizeof(iv));

  uint8_t ciphertext[CRYPTO_LOCAL_KEY_SIZE];
  uint8_t tag[CRYPTO_AES_TAG_SIZE];
  uint8_t purpose = 0x01; // 0x01 = CLAIM

  // Encrypt localKey (Section 3 Step 5: AES-GCM without AAD)
  if (!crypto_aes_gcm_encrypt(claim_session_key, iv, new_local_key,
                              sizeof(new_local_key), NULL, 0, ciphertext,
                              tag)) {
    ESP_LOGE(TAG, "AES-GCM encryption of localKey failed");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "GCM Encrypt Failed");
    return;
  }

  // Build KEY_DELIVER Payload:
  // [0]     = purpose (0x01)
  // [1..12] = IV (12 bytes)
  // [13..44]= ciphertext (32 bytes)
  // [45..60]= authTag (16 bytes)
  // Total len = 61 bytes
  uint8_t deliver_payload[61];
  deliver_payload[0] = purpose;
  memcpy(&deliver_payload[1], iv, CRYPTO_AES_IV_SIZE);
  memcpy(&deliver_payload[13], ciphertext, CRYPTO_LOCAL_KEY_SIZE);
  memcpy(&deliver_payload[45], tag, CRYPTO_AES_TAG_SIZE);

  tx_len = ble_frame_build_req(0x72, 2, deliver_payload,
                               sizeof(deliver_payload), tx_buf);

  s_write_done = false;
  while (xSemaphoreTake(s_write_sem, 0) == pdTRUE) {
  }
  while (xSemaphoreTake(s_notify_sem, 0) == pdTRUE) {
  }

  ESP_LOGI(TAG, "Writing KEY_DELIVER (0x72) REQUEST (%u bytes) to handle %d...",
           tx_len, s_rx_val_handle);
  rc = ble_gattc_write_flat(s_conn_handle, s_rx_val_handle, tx_buf, tx_len,
                            ble_on_cmd_write, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to write KEY_DELIVER frame; rc=%d", rc);
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "KEY_DELIVER Write Failed");
    return;
  }

  if (xSemaphoreTake(s_write_sem, pdMS_TO_TICKS(4000)) != pdTRUE ||
      !s_write_done) {
    ESP_LOGE(TAG, "KEY_DELIVER write ACK timed out or failed");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "KEY_DELIVER Write Ack Timeout");
    return;
  }

  // Wait for KEY_DELIVER RESPONSE
  ESP_LOGI(TAG, "Waiting for KEY_DELIVER RESPONSE from lock...");
  if (xSemaphoreTake(s_notify_sem, pdMS_TO_TICKS(6000)) != pdTRUE) {
    ESP_LOGE(TAG, "Timeout waiting for KEY_DELIVER RESPONSE from lock");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "KEY_DELIVER Timeout");
    return;
  }

  ble_frame_t deliver_resp;
  if (!ble_frame_parse(s_rx_notify_buf, s_rx_notify_len, &deliver_resp)) {
    ESP_LOGE(TAG, "Failed to parse KEY_DELIVER RESPONSE frame");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "Invalid KEY_DELIVER Frame");
    return;
  }

  if (deliver_resp.cmd != 0x72 || deliver_resp.type != 0x02 ||
      deliver_resp.status != 0x00) {
    ESP_LOGE(TAG, "KEY_DELIVER rejected by lock: cmd=0x%02X status=0x%02X",
             deliver_resp.cmd, deliver_resp.status);
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL,
                                   0, "error", "KEY_DELIVER Rejected");
    return;
  }

  // ==========================================
  // STEP 4: Success! Publish /device_claim/ack
  // ==========================================
  char local_key_b64[64] = {0};
  size_t b64_len = 0;
  mbedtls_base64_encode((unsigned char *)local_key_b64, sizeof(local_key_b64),
                        &b64_len, new_local_key, sizeof(new_local_key));
  local_key_b64[b64_len] = '\0';

  ESP_LOGI(TAG, "=== SUB-DEVICE CLAIM SUCCESSFUL! ===");
  ESP_LOGI(TAG, "MAC: %s | DeviceId: %s | DeviceCode: %s", mac_str,
           lock_device_id, lock_device_code);
  ESP_LOGI(TAG, "LocalKey (b64): %s", local_key_b64);

  mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, lock_device_id,
                                 lock_device_code, local_key_b64, 1, "ok",
                                 NULL);
}

// Background task handling BLE connection, MTU exchange, GATT discovery,
// command relay and claiming
static void ble_cmd_listener_task(void *pvParameters) {
  ble_lock_cmd_t cmd;
  ESP_LOGI(TAG,
           "BLE Command Listener Task started (allocated stack: %u bytes).",
           (unsigned)BLE_CMD_TASK_STACK_SIZE);

  while (1) {
    if (xQueueReceive(ble_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
      ESP_LOGI(TAG, "Processing operation %d for MAC " MACSTR ", ReqID: %s",
               cmd.op_type, MAC2STR(cmd.mac), cmd.request_id);

      // Stop scanning to connect
      ble_app_scan_cancel();

      // Setup global state for this operation
      memcpy(&s_current_cmd, &cmd, sizeof(ble_lock_cmd_t));
      s_channel_ready = false;
      s_write_done = false;
      s_rx_notify_len = 0;

      // Drain semaphores
      while (xSemaphoreTake(s_channel_ready_sem, 0) == pdTRUE) {
      }
      while (xSemaphoreTake(s_write_sem, 0) == pdTRUE) {
      }
      while (xSemaphoreTake(s_notify_sem, 0) == pdTRUE) {
      }

      ble_addr_t peer_addr;
      memcpy(peer_addr.val, cmd.mac, BLE_MAC_LEN);
      peer_addr.type = cmd.addr_type; // 0=Public, 1=Random

      ESP_LOGI(TAG, "Connecting to target device (addr_type=%u)...",
               peer_addr.type);
      int rc = ble_gap_connect(s_own_addr_type, &peer_addr, 30000, NULL,
                               ble_gap_event, NULL);
      if (rc != 0) {
        ESP_LOGE(TAG, "Error initiating connection; rc=%d", rc);
        if (cmd.op_type == BLE_OP_DEVICE_CLAIM) {
          char mac_str[18];
          snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                   cmd.mac[0], cmd.mac[1], cmd.mac[2], cmd.mac[3], cmd.mac[4],
                   cmd.mac[5]);
          mqtt_manager_publish_claim_ack(cmd.request_id, mac_str, NULL, NULL,
                                         NULL, 0, "error",
                                         "BLE Connect Failed");
        }
        ble_app_scan();
        continue;
      }

      // Wait for connection, MTU exchange, and service discovery (12s timeout)
      if (xSemaphoreTake(s_channel_ready_sem, pdMS_TO_TICKS(12000)) != pdTRUE ||
          !s_channel_ready) {
        ESP_LOGE(TAG, "GATT channel setup failed or timed out.");
        if (cmd.op_type == BLE_OP_DEVICE_CLAIM) {
          char mac_str[18];
          snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                   cmd.mac[0], cmd.mac[1], cmd.mac[2], cmd.mac[3], cmd.mac[4],
                   cmd.mac[5]);
          mqtt_manager_publish_claim_ack(cmd.request_id, mac_str, NULL, NULL,
                                         NULL, 0, "error",
                                         "GATT Discovery Failed");
        }
      } else {
        // Channel is ready! Dispatch by op_type
        if (cmd.op_type == BLE_OP_PASSTHROUGH_CMD) {
          ble_execute_passthrough_cmd(&cmd);
        } else if (cmd.op_type == BLE_OP_DEVICE_CLAIM) {
          ble_execute_device_claim(&cmd);
        }
      }

      // Terminate connection cleanly if still connected
      if (s_connected) {
        ESP_LOGI(TAG, "Disconnecting from peripheral...");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(300));
      } else {
        ble_gap_conn_cancel();
        ble_app_scan();
      }

      // Monitor FreeRTOS stack high water mark to ensure no stack overflow
      UBaseType_t high_water = uxTaskGetStackHighWaterMark(NULL);
      ESP_LOGI(TAG, "BLE Task Stack Remaining (Watermark): %u bytes",
               (unsigned)(high_water * sizeof(StackType_t)));
    }
  }
}

static void ble_host_task(void *param) {
  ESP_LOGI(TAG, "BLE Host Task started.");
  nimble_port_run();
  nimble_port_freertos_deinit();
}

esp_err_t ble_manager_init(void) {
  ESP_LOGI(TAG, "Initializing NimBLE Stack...");

  ble_scan_queue = xQueueCreate(20, sizeof(ble_scan_report_t));
  ble_cmd_queue = xQueueCreate(5, sizeof(ble_lock_cmd_t));
  s_channel_ready_sem = xSemaphoreCreateBinary();
  s_write_sem = xSemaphoreCreateBinary();
  s_notify_sem = xSemaphoreCreateBinary();

  if (ble_scan_queue == NULL || ble_cmd_queue == NULL ||
      s_channel_ready_sem == NULL || s_write_sem == NULL ||
      s_notify_sem == NULL) {
    ESP_LOGE(TAG, "Failed to create BLE queues or semaphores.");
    return ESP_ERR_NO_MEM;
  }

  esp_err_t ret = nimble_port_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize nimble port: %s", esp_err_to_name(ret));
    return ret;
  }

  // Configure NimBLE callbacks
  ble_hs_cfg.reset_cb = ble_on_reset;
  ble_hs_cfg.sync_cb = ble_on_sync;

  int rc = ble_svc_gap_device_name_set(GATEWAY_DEVICE_CODE);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to set device name; rc=%d", rc);
    return ESP_FAIL;
  }

  esp_err_t server_err = ble_server_init();
  if (server_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize BLE GATT server");
    return server_err;
  }

  return ESP_OK;
}

esp_err_t ble_manager_start(void) {
  ESP_LOGI(TAG, "Starting BLE Host thread and command listener...");
  nimble_port_freertos_init(ble_host_task);

  // Spawn task to listen for outgoing BLE command writes (8192 bytes stack for
  // crypto + cJSON)
  xTaskCreate(&ble_cmd_listener_task, "ble_cmd_task", BLE_CMD_TASK_STACK_SIZE,
              NULL, 5, NULL);

  return ESP_OK;
}

esp_err_t ble_manager_stop(void) {
  ESP_LOGI(TAG, "Stopping BLE Manager...");
  s_ble_synced = false;
  ble_app_scan_cancel();
  return nimble_port_stop();
}

bool ble_manager_is_scanning(void) {
  return s_scanning;
}

void ble_manager_trigger_scan(void) {
  if (!s_ble_synced) {
    ESP_LOGW(TAG, "BLE Host is not synced yet. Cannot trigger scan.");
    return;
  }

  ESP_LOGI(TAG, "Manual BLE scan triggered by button.");
  // Reset scan throttle cache so all beacons/locks are immediately re-discovered
  memset(s_scan_cache, 0, sizeof(s_scan_cache));

  if (s_connected) {
    ESP_LOGW(TAG, "Currently connected to peripheral, scan will resume upon disconnect.");
    return;
  }

  if (s_scanning) {
    ble_app_scan_cancel();
  }
  ble_app_scan();
}
