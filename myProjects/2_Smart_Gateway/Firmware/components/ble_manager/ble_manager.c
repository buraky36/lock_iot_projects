#include "ble_manager.h"
#include "ble_frame_parser.h"
#include "ble_server.h"
#include "config_manager.h"
#include "crypto_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "mbedtls/base64.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "esp_partition.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include <string.h>

#define BLE_CMD_TASK_STACK_SIZE 8192
#define BLE_MAX_PEERS           4

// Scan rate limiter
#define SCAN_CACHE_ENTRIES 16
#define SCAN_THROTTLE_MS   5000 // Report each device at most once every 5 seconds

typedef struct {
  uint8_t mac[BLE_MAC_LEN];
  int64_t last_reported_ms;
} scan_rate_limit_entry_t;

static scan_rate_limit_entry_t s_scan_cache[SCAN_CACHE_ENTRIES];

static bool should_throttle_scan_report(const uint8_t *mac, int64_t now_ms) {
  int oldest_idx = 0;
  int64_t oldest_time = now_ms;

  for (int i = 0; i < SCAN_CACHE_ENTRIES; i++) {
    if (memcmp(s_scan_cache[i].mac, mac, BLE_MAC_LEN) == 0) {
      if ((now_ms - s_scan_cache[i].last_reported_ms) < SCAN_THROTTLE_MS) {
        return true;
      }
      s_scan_cache[i].last_reported_ms = now_ms;
      return false;
    }
    if (s_scan_cache[i].last_reported_ms < oldest_time) {
      oldest_time = s_scan_cache[i].last_reported_ms;
      oldest_idx = i;
    }
  }

  memcpy(s_scan_cache[oldest_idx].mac, mac, BLE_MAC_LEN);
  s_scan_cache[oldest_idx].last_reported_ms = now_ms;
  return false;
}

static const char *TAG = "BLE_MGR";

QueueHandle_t ble_scan_queue = NULL;
QueueHandle_t ble_cmd_queue = NULL;

static uint8_t s_own_addr_type;
static bool s_scanning = false;
static bool s_ble_synced = false;

typedef enum {
  PEER_STATE_DISCONNECTED = 0,
  PEER_STATE_CONNECTING,
  PEER_STATE_CONNECTED_DISCOVERING,
  PEER_STATE_UNAUTHENTICATED, // Connected + GATT ready, but SESSION_INIT not done
  PEER_STATE_AUTHENTICATED,   // SESSION_INIT OK, sessionKey derived
} peer_conn_state_t;

typedef struct {
  uint8_t mac[BLE_MAC_LEN];
  uint8_t addr_type;
  char device_id[SUBKEY_DEVICE_ID_LEN + 1];

  uint16_t conn_handle;
  peer_conn_state_t state;

  uint16_t svc_start_handle;
  uint16_t svc_end_handle;
  uint16_t rx_val_handle;
  uint16_t tx_val_handle;
  uint16_t cccd_handle;
  bool svc_found;
  bool rx_found;
  bool tx_found;

  // Cryptographic session data
  uint8_t gw_eph_priv[CRYPTO_X25519_KEY_SIZE];
  uint8_t gw_eph_pub[CRYPTO_X25519_KEY_SIZE];
  uint8_t dev_eph_pub[CRYPTO_X25519_KEY_SIZE];
  uint8_t conn_nonce[8];
  uint8_t session_key[CRYPTO_AES_KEY_SIZE];

  // Sequence numbers
  uint16_t local_seq;
  uint16_t last_remote_seq;
  bool has_remote_seq;
  uint16_t att_mtu;

  // Synchronization semaphores
  SemaphoreHandle_t op_sem;
  SemaphoreHandle_t write_sem;
  SemaphoreHandle_t notify_sem;
  bool write_done;

  uint8_t rx_notify_buf[520];
  uint16_t rx_notify_len;

  ble_lock_cmd_t active_cmd;
  bool has_active_cmd;
  char last_request_id[64];

  int64_t last_disconnect_ms;
  uint32_t reconnect_backoff_sec; // 5s -> 10s -> 20s -> 40s -> 60s
  int64_t next_reconnect_ms;
} peer_ctx_t;

static peer_ctx_t s_peers[BLE_MAX_PEERS];

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_start_service_discovery(peer_ctx_t *peer);
static void ble_app_scan(void);
static void ble_app_scan_cancel(void);
static void build_session_gcm_nonce(const uint8_t conn_nonce[8], uint16_t seq, uint8_t out_iv[12]);
static esp_err_t ble_execute_session_init(peer_ctx_t *peer, const subkey_entry_t *subkey);
static void ble_execute_passthrough_cmd(peer_ctx_t *peer, const ble_lock_cmd_t *cmd);
static void ble_execute_device_claim(peer_ctx_t *peer, const ble_lock_cmd_t *cmd);
static void ble_execute_device_unclaim(peer_ctx_t *peer, const ble_lock_cmd_t *cmd);
static void ble_execute_key_rotate(peer_ctx_t *peer, const ble_lock_cmd_t *cmd);
static void ble_execute_sub_ota(peer_ctx_t *peer, const ble_lock_cmd_t *cmd);
static void ble_execute_get_fw_info(peer_ctx_t *peer, const ble_lock_cmd_t *cmd);

static peer_ctx_t *peer_find_by_handle(uint16_t conn_handle) {
  if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return NULL;
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    if (s_peers[i].conn_handle == conn_handle) {
      return &s_peers[i];
    }
  }
  return NULL;
}

static peer_ctx_t *peer_find_by_mac(const uint8_t *mac) {
  if (!mac) return NULL;
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    if (s_peers[i].state != PEER_STATE_DISCONNECTED &&
        memcmp(s_peers[i].mac, mac, BLE_MAC_LEN) == 0) {
      return &s_peers[i];
    }
  }
  return NULL;
}

static peer_ctx_t *peer_find_by_device_id(const char *device_id) {
  if (!device_id || strlen(device_id) == 0) return NULL;
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    if (s_peers[i].state != PEER_STATE_DISCONNECTED &&
        strcmp(s_peers[i].device_id, device_id) == 0) {
      return &s_peers[i];
    }
  }
  return NULL;
}

static peer_ctx_t *peer_alloc_slot(const uint8_t *mac, uint8_t addr_type) {
  // 1. Check if matching slot exists
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    if (memcmp(s_peers[i].mac, mac, BLE_MAC_LEN) == 0) {
      s_peers[i].addr_type = addr_type;
      return &s_peers[i];
    }
  }
  // 2. Find disconnected slot
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    if (s_peers[i].state == PEER_STATE_DISCONNECTED) {
      memcpy(s_peers[i].mac, mac, BLE_MAC_LEN);
      s_peers[i].addr_type = addr_type;
      s_peers[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
      s_peers[i].reconnect_backoff_sec = 5;
      s_peers[i].next_reconnect_ms = 0;
      return &s_peers[i];
    }
  }
  return NULL;
}

static void peer_reset_state(peer_ctx_t *peer) {
  if (!peer) return;
  peer->state = PEER_STATE_DISCONNECTED;
  peer->conn_handle = BLE_HS_CONN_HANDLE_NONE;
  peer->has_remote_seq = false;
  peer->has_active_cmd = false;
  peer->last_request_id[0] = '\0';
  peer->att_mtu = 23;
  peer->svc_start_handle = 0;
  peer->svc_end_handle = 0;
  peer->rx_val_handle = 0;
  peer->tx_val_handle = 0;
  peer->cccd_handle = 0;
  peer->svc_found = false;
  peer->rx_found = false;
  peer->tx_found = false;
  peer->write_done = false;
  peer->has_active_cmd = false;
  memset(peer->gw_eph_priv, 0, sizeof(peer->gw_eph_priv));
  memset(peer->gw_eph_pub, 0, sizeof(peer->gw_eph_pub));
  memset(peer->dev_eph_pub, 0, sizeof(peer->dev_eph_pub));
  memset(peer->conn_nonce, 0, sizeof(peer->conn_nonce));
  memset(peer->session_key, 0, sizeof(peer->session_key));
  peer->last_remote_seq = 0;
}

static int ble_on_cmd_write(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg) {
  peer_ctx_t *peer = peer_find_by_handle(conn_handle);
  if (!peer) return 0;

  if (error->status == 0) {
    ESP_LOGD(TAG, "GATT Write Success on conn_handle %d", conn_handle);
    peer->write_done = true;
  } else {
    ESP_LOGE(TAG, "GATT Write Failed on conn_handle %d: status=%d",
             conn_handle, error->status);
    peer->write_done = false;
  }
  if (peer->write_sem != NULL) {
    xSemaphoreGive(peer->write_sem);
  }
  return 0;
}

static int ble_on_cccd_write(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
  peer_ctx_t *peer = peer_find_by_handle(conn_handle);
  if (!peer) return 0;

  if (error->status == 0) {
    ESP_LOGI(TAG, "CCCD Subscription enabled on conn_handle %d! Ready for auth.", conn_handle);
    peer->state = PEER_STATE_UNAUTHENTICATED;
  } else {
    ESP_LOGE(TAG, "Failed to enable CCCD on conn_handle %d: status=%d",
             conn_handle, error->status);
    peer->state = PEER_STATE_DISCONNECTED;
  }
  if (peer->op_sem != NULL) {
    xSemaphoreGive(peer->op_sem);
  }
  return 0;
}

static int ble_on_disc_dscs(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc, void *arg) {
  peer_ctx_t *peer = peer_find_by_handle(conn_handle);
  if (!peer) return 0;

  if (error->status == 0 && dsc != NULL) {
    uint16_t uuid16 = ble_uuid_u16(&dsc->uuid.u);
    if (uuid16 == 0x2902) {
      peer->cccd_handle = dsc->handle;
      ESP_LOGI(TAG, "Found CCCD descriptor for conn_handle %d at handle %d",
               conn_handle, peer->cccd_handle);
    }
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    if (peer->cccd_handle != 0) {
      ESP_LOGI(TAG, "Subscribing to notifications on CCCD handle %d...", peer->cccd_handle);
      uint8_t value[2] = {0x01, 0x00}; // Enable Notify
      int rc = ble_gattc_write_flat(conn_handle, peer->cccd_handle, value,
                                    sizeof(value), ble_on_cccd_write, NULL);
      if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate CCCD write; rc=%d", rc);
        if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
      }
    } else {
      ESP_LOGE(TAG, "No CCCD descriptor found for TX characteristic on conn_handle %d", conn_handle);
      if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
    }
  } else if (error->status != 0) {
    ESP_LOGE(TAG, "Descriptor discovery error on conn_handle %d: %d", conn_handle, error->status);
    if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
  }
  return 0;
}

static int ble_on_disc_all_chrs(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg) {
  peer_ctx_t *peer = peer_find_by_handle(conn_handle);
  if (!peer) return 0;

  if (error->status == 0 && chr != NULL) {
    char uuid_str[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&chr->uuid.u, uuid_str);

    if (strcasecmp(uuid_str, ONLOI_NUS_RX_UUID) == 0) {
      peer->rx_val_handle = chr->val_handle;
      peer->rx_found = true;
      ESP_LOGI(TAG, "Discovered RX (Write) Char on conn_handle %d at handle %d",
               conn_handle, peer->rx_val_handle);
    } else if (strcasecmp(uuid_str, ONLOI_NUS_TX_UUID) == 0) {
      peer->tx_val_handle = chr->val_handle;
      peer->tx_found = true;
      ESP_LOGI(TAG, "Discovered TX (Notify) Char on conn_handle %d at handle %d",
               conn_handle, peer->tx_val_handle);
    }
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    if (peer->rx_found && peer->tx_found) {
      ESP_LOGI(TAG, "Discovered required RX/TX characteristics on conn_handle %d. Finding CCCD...", conn_handle);
      int rc = ble_gattc_disc_all_dscs(conn_handle, peer->tx_val_handle,
                                       peer->svc_end_handle, ble_on_disc_dscs, NULL);
      if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate descriptor discovery; rc=%d", rc);
        if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
      }
    } else {
      ESP_LOGE(TAG, "Required characteristics missing (rx=%d, tx=%d) on conn_handle %d",
               peer->rx_found, peer->tx_found, conn_handle);
      if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
    }
  } else if (error->status != 0) {
    ESP_LOGE(TAG, "Characteristic discovery error on conn_handle %d: %d", conn_handle, error->status);
    if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
  }
  return 0;
}

static int ble_on_disc_svc(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg) {
  peer_ctx_t *peer = peer_find_by_handle(conn_handle);
  if (!peer) return 0;

  if (error->status == 0 && service != NULL) {
    peer->svc_start_handle = service->start_handle;
    peer->svc_end_handle = service->end_handle;
    peer->svc_found = true;
    ESP_LOGI(TAG, "Service found on conn_handle %d: handle %d to %d",
             conn_handle, peer->svc_start_handle, peer->svc_end_handle);
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    if (peer->svc_found) {
      ESP_LOGI(TAG, "Discovering characteristics for conn_handle %d...", conn_handle);
      int rc = ble_gattc_disc_all_chrs(conn_handle, peer->svc_start_handle,
                                       peer->svc_end_handle, ble_on_disc_all_chrs, NULL);
      if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate char discovery; rc=%d", rc);
        if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
      }
    } else {
      ESP_LOGE(TAG, "Target Nordic UART Service not found on conn_handle %d", conn_handle);
      if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
    }
  } else if (error->status != 0) {
    ESP_LOGE(TAG, "Service discovery error on conn_handle %d: %d", conn_handle, error->status);
    if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
  }
  return 0;
}

static void ble_start_service_discovery(peer_ctx_t *peer) {
  if (!peer || peer->conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

  ble_uuid128_t svc_uuid;
  int rc = ble_uuid_from_str((ble_uuid_any_t *)&svc_uuid, ONLOI_NUS_SVC_UUID);
  if (rc != 0) {
    ESP_LOGE(TAG, "Invalid Service UUID string: %s", ONLOI_NUS_SVC_UUID);
    if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
    return;
  }

  peer->svc_found = false;
  peer->rx_found = false;
  peer->tx_found = false;
  peer->rx_val_handle = 0;
  peer->tx_val_handle = 0;
  peer->cccd_handle = 0;

  ESP_LOGI(TAG, "Starting GATT Service Discovery on conn_handle %d for UUID: %s",
           peer->conn_handle, ONLOI_NUS_SVC_UUID);
  rc = ble_gattc_disc_svc_by_uuid(peer->conn_handle, &svc_uuid.u, ble_on_disc_svc, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to initiate Service discovery; rc=%d", rc);
    if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
  }
}

static int ble_on_mtu_exchange(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               uint16_t mtu, void *arg) {
  peer_ctx_t *peer = peer_find_by_handle(conn_handle);
  if (!peer) return 0;

  if (error->status == 0) {
    peer->att_mtu = mtu;
    ESP_LOGI(TAG, "MTU exchange completed for conn_handle %d! Negotiated MTU: %u bytes",
             conn_handle, mtu);
  } else {
    peer->att_mtu = 23;
    ESP_LOGW(TAG, "MTU exchange failed (status=%d). Using default MTU.", error->status);
  }
  ble_start_service_discovery(peer);
  return 0;
}

static void ble_app_scan(void) {
  if (s_scanning) return;

  struct ble_gap_disc_params disc_params;
  memset(&disc_params, 0, sizeof(disc_params));
  disc_params.filter_duplicates = 0;
  disc_params.passive = 0; // Active scanning
  disc_params.itvl = BLE_GAP_SCAN_FAST_INTERVAL_MIN;
  disc_params.window = BLE_GAP_SCAN_FAST_WINDOW;
  disc_params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
  disc_params.limited = 0;

  int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &disc_params,
                        ble_gap_event, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "Error initiating BLE scan; rc=%d", rc);
  } else {
    s_scanning = true;
    ESP_LOGI(TAG, "BLE Scan started (Active Mode)...");
  }
}

static void ble_app_scan_cancel(void) {
  if (s_scanning) {
    int rc = ble_gap_disc_cancel();
    if (rc == 0) {
      s_scanning = false;
      ESP_LOGI(TAG, "BLE Scan cancelled successfully.");
    } else {
      ESP_LOGW(TAG, "Failed to cancel scan; rc=%d", rc);
    }
  }
}

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

    // 1. Parse Manufacturer Data
    if (fields.mfg_data != NULL && fields.mfg_data_len >= 9) {
      if (fields.mfg_data[0] == 0x4F && fields.mfg_data[1] == 0x4B) {
        report.is_onloi_beacon = true;
        memcpy(report.device_code, &fields.mfg_data[2], 6);
        report.device_code[6] = '\0';
        report.claimed = (fields.mfg_data[8] == 0x01);
      } else if (fields.mfg_data_len >= 11 && fields.mfg_data[2] == 0x4F &&
                 fields.mfg_data[3] == 0x4B) {
        report.is_onloi_beacon = true;
        memcpy(report.device_code, &fields.mfg_data[4], 6);
        report.device_code[6] = '\0';
        report.claimed = (fields.mfg_data[10] == 0x01);
      }
    }

    // 2. Parse Service Data UUID16: 0xFFF0
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

    // 3. Parse Local Name starting with OK, OD, OG, OB or ONLOI
    if (!report.is_onloi_beacon && fields.name != NULL && fields.name_len >= 2) {
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

    // 4. Raw packet fallback search for "OK" (0x4F, 0x4B)
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

    if (!report.is_onloi_beacon) {
      break;
    }

    // Check if this Onloi beacon is a claimed subdevice in NVS (Section 7 Persistent Telemetry)
    if (subkey_is_claimed(report.mac)) {
      peer_ctx_t *peer = peer_find_by_mac(report.mac);
      int64_t now_ms = esp_timer_get_time() / 1000;
      if (peer == NULL || peer->state == PEER_STATE_DISCONNECTED) {
        if (peer == NULL || now_ms >= peer->next_reconnect_ms) {
          ESP_LOGI(TAG, "[AUTO-RECONNECT] Claimed sub-device " MACSTR " detected in scan, scheduling telemetry connection",
                   MAC2STR(report.mac));
          ble_lock_cmd_t auto_cmd;
          memset(&auto_cmd, 0, sizeof(auto_cmd));
          auto_cmd.op_type = BLE_OP_AUTO_CONNECT;
          memcpy(auto_cmd.mac, report.mac, BLE_MAC_LEN);
          auto_cmd.addr_type = report.addr_type;
          if (ble_cmd_queue != NULL) {
            xQueueSend(ble_cmd_queue, &auto_cmd, 0);
          }
        }
      }
    }

    // Throttle scan reporting to MQTT
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (should_throttle_scan_report(report.mac, now_ms)) {
      break;
    }

    ESP_LOGI(TAG, "[SCAN] Discovered Onloi Beacon: MAC=" MACSTR
             ", RSSI=%d, Code=%s, Claimed=%d",
             MAC2STR(report.mac), report.rssi, report.device_code, report.claimed);

    if (ble_scan_queue != NULL) {
      if (xQueueSend(ble_scan_queue, &report, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[SCAN] Scan queue full, dropping beacon report");
      }
    }
    break;
  }

  case BLE_GAP_EVENT_CONNECT: {
    if (event->connect.status == 0) {
      // Find peer in connecting state
      peer_ctx_t *peer = NULL;
      for (int i = 0; i < BLE_MAX_PEERS; i++) {
        if (s_peers[i].state == PEER_STATE_CONNECTING) {
          peer = &s_peers[i];
          break;
        }
      }

      if (peer != NULL) {
        peer->conn_handle = event->connect.conn_handle;
        peer->state = PEER_STATE_CONNECTED_DISCOVERING;
        ESP_LOGI(TAG, "Connected successfully to peripheral " MACSTR "! Conn Handle: %d",
                 MAC2STR(peer->mac), peer->conn_handle);

        ESP_LOGI(TAG, "Initiating BLE MTU exchange for conn_handle %d...", peer->conn_handle);
        int rc = ble_gattc_exchange_mtu(peer->conn_handle, ble_on_mtu_exchange, NULL);
        if (rc != 0) {
          ESP_LOGW(TAG, "MTU exchange initiation failed (rc=%d). Proceeding to service discovery...", rc);
          ble_start_service_discovery(peer);
        }
      } else {
        ESP_LOGW(TAG, "Connected to conn_handle %d, but no peer was in CONNECTING state", event->connect.conn_handle);
      }
    } else {
      ESP_LOGE(TAG, "Connection attempt failed; status=%d", event->connect.status);
      for (int i = 0; i < BLE_MAX_PEERS; i++) {
        if (s_peers[i].state == PEER_STATE_CONNECTING) {
          peer_reset_state(&s_peers[i]);
          if (s_peers[i].op_sem != NULL) {
            xSemaphoreGive(s_peers[i].op_sem);
          }
          break;
        }
      }
      ble_app_scan();
    }
    break;
  }

  case BLE_GAP_EVENT_NOTIFY_RX: {
    uint16_t conn_handle = event->notify_rx.conn_handle;
    peer_ctx_t *peer = peer_find_by_handle(conn_handle);
    if (!peer) {
      ESP_LOGW(TAG, "Notification received for unknown conn_handle %d", conn_handle);
      break;
    }

    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len > sizeof(peer->rx_notify_buf)) len = sizeof(peer->rx_notify_buf);
    os_mbuf_copydata(event->notify_rx.om, 0, len, peer->rx_notify_buf);
    peer->rx_notify_len = len;

    ESP_LOGI(TAG, "Notification received on conn_handle %d, length %u", conn_handle, len);

    // 1. Verify CRC16-CCITT (Section 7.2 requirement: verify CRC16, discard corrupted frame)
    if (len >= 19) {
      uint16_t expected_crc = ((uint16_t)peer->rx_notify_buf[len - 2] << 8) | peer->rx_notify_buf[len - 1];
      uint16_t actual_crc = calc_crc16_ccitt(peer->rx_notify_buf, len - 2);
      if (expected_crc != actual_crc) {
        ESP_LOGW(TAG, "Corrupted BLE frame received on conn_handle %d (CRC 0x%04X != 0x%04X), dropped",
                 conn_handle, expected_crc, actual_crc);
        break;
      }

      // 2. Monotonic SEQ and replay protection (Section 10 Step 3 / WP7)
      uint16_t rx_seq = ((uint16_t)peer->rx_notify_buf[4] << 8) | peer->rx_notify_buf[5];
      if (peer->state == PEER_STATE_AUTHENTICATED) {
        if (peer->has_remote_seq && (int16_t)(rx_seq - peer->last_remote_seq) <= 0) {
          ESP_LOGW(TAG, "REPLAY_DETECTED (0x%02X) on MAC " MACSTR ": received SEQ %u <= lastSeq %u, dropping frame",
                   BLE_STATUS_REPLAY_DETECTED, MAC2STR(peer->mac), rx_seq, peer->last_remote_seq);
          break;
        }
        peer->last_remote_seq = rx_seq;
        peer->has_remote_seq = true;
      }
    }

    // 3. Notify Relay to MQTT for ALL notifications (Section 7.2 + Uplink Codec)
    if (peer->state == PEER_STATE_AUTHENTICATED) {
      if (len >= 19 && peer->rx_notify_buf[2] == BLE_MSG_TYPE_EVENT &&
          (peer->rx_notify_buf[3] == BLE_CMD_FIRMWARE_STATUS ||
           peer->rx_notify_buf[3] == BLE_CMD_FIRMWARE_INFO)) {
        mqtt_manager_publish_proto_event(peer->rx_notify_buf, peer->rx_notify_len);
      }

      mqtt_notify_relay_t relay;
      memset(&relay, 0, sizeof(relay));
      memcpy(relay.mac, peer->mac, 6);
      strncpy(relay.target_device_id, peer->device_id, sizeof(relay.target_device_id) - 1);
      if (peer->has_active_cmd && strlen(peer->active_cmd.request_id) > 0) {
        strncpy(relay.request_id, peer->active_cmd.request_id, sizeof(relay.request_id) - 1);
      } else if (strlen(peer->last_request_id) > 0) {
        strncpy(relay.request_id, peer->last_request_id, sizeof(relay.request_id) - 1);
      }
      relay.gw_eph_pub = peer->gw_eph_pub;
      relay.dev_eph_pub = peer->dev_eph_pub;
      relay.conn_nonce = peer->conn_nonce;
      relay.frame_data = peer->rx_notify_buf;
      relay.frame_len = peer->rx_notify_len;
      relay.result = "ok";

      uint8_t *dec_frame_buf = NULL;
      ble_frame_t rx_frame;
      if (ble_frame_parse(peer->rx_notify_buf, peer->rx_notify_len, &rx_frame)) {
        relay.cmd_code = rx_frame.cmd;
        relay.has_cmd = true;
        if ((rx_frame.type == BLE_MSG_TYPE_RESP || rx_frame.type == BLE_MSG_TYPE_EVENT) &&
            rx_frame.cmd != BLE_CMD_SESSION_INIT && rx_frame.cmd != BLE_CMD_KEX_INIT &&
            rx_frame.len >= CRYPTO_AES_TAG_SIZE) {
          uint16_t pt_len = rx_frame.len - CRYPTO_AES_TAG_SIZE;
          dec_frame_buf = malloc(19U + pt_len);
          if (dec_frame_buf != NULL) {
            uint8_t iv[CRYPTO_AES_IV_SIZE];
            build_session_gcm_nonce(peer->conn_nonce, rx_frame.seq, iv);
            const uint8_t *ct = rx_frame.payload;
            const uint8_t *tag = rx_frame.payload + pt_len;
            uint8_t *pt_out = dec_frame_buf + 17;
            if (crypto_aes_gcm_decrypt(peer->session_key, iv, ct, pt_len, NULL, 0, tag, pt_out)) {
              uint16_t dec_len = ble_frame_build_with_type(
                  rx_frame.type, rx_frame.cmd, rx_frame.seq, rx_frame.status,
                  rx_frame.timestamp, pt_out, pt_len, dec_frame_buf);
              relay.dec_frame_data = dec_frame_buf;
              relay.dec_frame_len = dec_len;
            }
          }
        }
      }

      mqtt_manager_publish_notify_relay(&relay);
      if (dec_frame_buf != NULL) {
        free(dec_frame_buf);
      }
    }

    // Signal listener task waiting for response
    if (peer->notify_sem != NULL) {
      xSemaphoreGive(peer->notify_sem);
    }
    break;
  }

  case BLE_GAP_EVENT_DISCONNECT: {
    peer_ctx_t *peer = peer_find_by_handle(event->disconnect.conn.conn_handle);
    if (peer != NULL) {
      ESP_LOGI(TAG, "Disconnected from peripheral " MACSTR " (conn_handle=%d, reason=%d)",
               MAC2STR(peer->mac), peer->conn_handle, event->disconnect.reason);
      peer_reset_state(peer);

      // Backoff calculation: 5s -> 10s -> 20s -> 40s -> 60s
      int64_t now_ms = esp_timer_get_time() / 1000;
      peer->last_disconnect_ms = now_ms;
      if (peer->reconnect_backoff_sec == 0) {
        peer->reconnect_backoff_sec = 5;
      } else {
        peer->reconnect_backoff_sec = (peer->reconnect_backoff_sec * 2 > 60) ? 60 : peer->reconnect_backoff_sec * 2;
      }
      peer->next_reconnect_ms = now_ms + (peer->reconnect_backoff_sec * 1000);
      ESP_LOGI(TAG, "Next reconnect attempt for " MACSTR " in %lu seconds",
               MAC2STR(peer->mac), (unsigned long)peer->reconnect_backoff_sec);

      if (peer->op_sem != NULL) xSemaphoreGive(peer->op_sem);
      if (peer->write_sem != NULL) xSemaphoreGive(peer->write_sem);
      if (peer->notify_sem != NULL) xSemaphoreGive(peer->notify_sem);
    }
    ble_app_scan();
    break;
  }

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
  if (rc != 0) {
    ESP_LOGE(TAG, "Error ensuring own BLE address; rc=%d", rc);
    return;
  }

  rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGE(TAG, "Error determining BLE address type; rc=%d", rc);
    return;
  }

  uint8_t addr_val[6] = {0};
  rc = ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
  ESP_LOGI(TAG, "NimBLE Host Synced! Device BLE Address: %02X:%02X:%02X:%02X:%02X:%02X (type=%u)",
           addr_val[5], addr_val[4], addr_val[3], addr_val[2], addr_val[1], addr_val[0],
           s_own_addr_type);

  s_ble_synced = true;
  ble_server_start_adv();
  ble_app_scan();
}

static void ble_on_reset(int reason) {
  ESP_LOGE(TAG, "Resetting NimBLE host; reason=%d", reason);
  s_scanning = false;
}

static esp_err_t ble_connect_to_peer(peer_ctx_t *peer) {
  if (!peer) return ESP_ERR_INVALID_ARG;
  if (peer->state == PEER_STATE_AUTHENTICATED || peer->state == PEER_STATE_UNAUTHENTICATED) {
    return ESP_OK;
  }

  ble_app_scan_cancel();
  vTaskDelay(pdMS_TO_TICKS(50));

  peer->state = PEER_STATE_CONNECTING;
  peer->svc_found = false;
  peer->rx_found = false;
  peer->tx_found = false;
  peer->rx_val_handle = 0;
  peer->tx_val_handle = 0;
  peer->cccd_handle = 0;

  while (xSemaphoreTake(peer->op_sem, 0) == pdTRUE) {}
  while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
  while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}

  ble_addr_t peer_addr;
  memcpy(peer_addr.val, peer->mac, BLE_MAC_LEN);
  peer_addr.type = peer->addr_type;

  ESP_LOGI(TAG, "Connecting to target device " MACSTR " (addr_type=%u)...",
           MAC2STR(peer->mac), peer_addr.type);

  int rc = ble_gap_connect(s_own_addr_type, &peer_addr, 30000, NULL, ble_gap_event, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "Error initiating connection to " MACSTR "; rc=%d", MAC2STR(peer->mac), rc);
    peer_reset_state(peer);
    ble_app_scan();
    return ESP_FAIL;
  }

  // Wait for connection, MTU exchange, GATT discovery, and CCCD subscription (12s timeout)
  if (xSemaphoreTake(peer->op_sem, pdMS_TO_TICKS(12000)) != pdTRUE ||
      (peer->state != PEER_STATE_UNAUTHENTICATED && peer->state != PEER_STATE_AUTHENTICATED)) {
    ESP_LOGE(TAG, "GATT channel setup failed or timed out for " MACSTR, MAC2STR(peer->mac));
    if (peer->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
      ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
      ble_gap_conn_cancel();
    }
    peer_reset_state(peer);
    ble_app_scan();
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGI(TAG, "GATT channel setup SUCCESS for " MACSTR " (rx=%d, tx=%d, cccd=%d)",
           MAC2STR(peer->mac), peer->rx_val_handle, peer->tx_val_handle, peer->cccd_handle);

  ble_app_scan();
  return ESP_OK;
}

// Section 5.2: SESSION_INIT (0x70)
static esp_err_t ble_execute_session_init(peer_ctx_t *peer, const subkey_entry_t *subkey) {
  if (!peer || !subkey) return ESP_ERR_INVALID_ARG;

  if (peer->state == PEER_STATE_AUTHENTICATED) {
    ESP_LOGI(TAG, "Session already AUTHENTICATED on connection handle %d, reusing existing session",
             peer->conn_handle);
    return ESP_OK;
  }

  if (peer->rx_val_handle == 0 || peer->tx_val_handle == 0) {
    ESP_LOGE(TAG, "SESSION_INIT failed: RX/TX char handles not ready on handle %d", peer->conn_handle);
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "==================================================");
  ESP_LOGI(TAG, "STARTING SESSION_INIT (0x70) FOR MAC: " MACSTR " (deviceId: '%s')",
           MAC2STR(peer->mac), subkey->device_id);
  ESP_LOGI(TAG, "==================================================");

  // a) Generate X25519 ephemeral keypair + 8-byte random connNonce
  if (!crypto_generate_x25519_keypair(peer->gw_eph_priv, peer->gw_eph_pub)) {
    ESP_LOGE(TAG, "Failed to generate Gateway ephemeral X25519 keypair");
    return ESP_FAIL;
  }
  crypto_generate_random(peer->conn_nonce, 8);

  // b) proof = HMAC-SHA256(localKey, ephPub || connNonce || deviceId)[0:16]
  uint8_t hmac_input[32 + 8 + 16];
  memcpy(hmac_input, peer->gw_eph_pub, 32);
  memcpy(hmac_input + 32, peer->conn_nonce, 8);
  uint8_t dev_id_16[16] = {0};
  memcpy(dev_id_16, subkey->device_id, strlen(subkey->device_id) > 16 ? 16 : strlen(subkey->device_id));
  memcpy(hmac_input + 40, dev_id_16, 16);

  uint8_t full_proof[32];
  if (!crypto_hmac_sha256(subkey->local_key, 32, hmac_input, sizeof(hmac_input), full_proof)) {
    ESP_LOGE(TAG, "HMAC-SHA256 failed for SESSION_INIT proof");
    return ESP_FAIL;
  }

  // c) REQUEST payload: ephPub(32) || connNonce(8) || proof(16) = 56 bytes
  uint8_t req_payload[56];
  memcpy(req_payload, peer->gw_eph_pub, 32);
  memcpy(req_payload + 32, peer->conn_nonce, 8);
  memcpy(req_payload + 40, full_proof, 16);

  uint8_t tx_buf[128];
  uint16_t seq = ++peer->local_seq;
  uint16_t tx_len = ble_frame_build_req(BLE_CMD_SESSION_INIT, seq, req_payload,
                                        sizeof(req_payload), tx_buf);

  // BUSY (0x06) exponential backoff retry loop (1s, 2s, 4s)
  const uint32_t busy_backoff_ms[3] = {1000, 2000, 4000};
  uint8_t last_status = 0xFF;

  for (int attempt = 0; attempt < 3; attempt++) {
    peer->write_done = false;
    while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
    while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}

    ESP_LOGI(TAG, "Sending SESSION_INIT (0x70) REQUEST (attempt %d/3, seq=%u, %u bytes)...",
             attempt + 1, seq, tx_len);

    int rc = ble_gattc_write_flat(peer->conn_handle, peer->rx_val_handle,
                                  tx_buf, tx_len, ble_on_cmd_write, NULL);
    if (rc != 0) {
      ESP_LOGE(TAG, "Failed to write SESSION_INIT frame; rc=%d", rc);
      return ESP_FAIL;
    }

    if (xSemaphoreTake(peer->write_sem, pdMS_TO_TICKS(4000)) != pdTRUE || !peer->write_done) {
      ESP_LOGE(TAG, "SESSION_INIT write ACK timed out");
      return ESP_ERR_TIMEOUT;
    }

    // Wait for SESSION_INIT RESPONSE
    if (xSemaphoreTake(peer->notify_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
      ESP_LOGE(TAG, "Timeout waiting for SESSION_INIT RESPONSE from device");
      return ESP_ERR_TIMEOUT;
    }

    ble_frame_t resp;
    if (!ble_frame_parse(peer->rx_notify_buf, peer->rx_notify_len, &resp)) {
      ESP_LOGE(TAG, "Failed to parse SESSION_INIT RESPONSE (CRC/format error)");
      return ESP_ERR_INVALID_RESPONSE;
    }

    if (resp.cmd != BLE_CMD_SESSION_INIT || resp.type != BLE_MSG_TYPE_RESP) {
      ESP_LOGE(TAG, "Unexpected frame in SESSION_INIT response: cmd=0x%02X, type=0x%02X",
               resp.cmd, resp.type);
      return ESP_ERR_INVALID_RESPONSE;
    }

    last_status = resp.status;
    if (resp.status == BLE_STATUS_BUSY) {
      ESP_LOGW(TAG, "Device returned BUSY (0x06). Exponential backoff %lu ms (attempt %d/3)...",
               (unsigned long)busy_backoff_ms[attempt], attempt + 1);
      vTaskDelay(pdMS_TO_TICKS(busy_backoff_ms[attempt]));
      continue;
    }

    if (resp.status != BLE_STATUS_OK) {
      ESP_LOGE(TAG, "SESSION_INIT rejected by device with STATUS=0x%02X (%s)",
               resp.status,
               (resp.status == BLE_STATUS_AUTH_FAILED) ? "AUTH_FAILED" :
               (resp.status == BLE_STATUS_NOT_CLAIMED) ? "NOT_CLAIMED" : "ERROR");
      return (resp.status == BLE_STATUS_AUTH_FAILED) ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
    }

    // d) Verify RESPONSE payload: devEphPub(32) || ackProof(16) = 48 bytes
    if (resp.len < 48) {
      ESP_LOGE(TAG, "SESSION_INIT response payload too short (%u bytes, expected 48)", resp.len);
      return ESP_ERR_INVALID_SIZE;
    }

    memcpy(peer->dev_eph_pub, resp.payload, 32);
    const uint8_t *dev_ack_proof = resp.payload + 32;

    uint8_t ack_input[32 + 8];
    memcpy(ack_input, peer->dev_eph_pub, 32);
    memcpy(ack_input + 32, peer->conn_nonce, 8);

    uint8_t expected_ack[32];
    crypto_hmac_sha256(subkey->local_key, 32, ack_input, sizeof(ack_input), expected_ack);

    if (memcmp(dev_ack_proof, expected_ack, 16) != 0) {
      ESP_LOGE(TAG, "SESSION_INIT ackProof verification FAILED! Local key mismatch or counterfeit device");
      return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "SESSION_INIT ackProof verified successfully!");

    // e) Derive sessionKey = HKDF-SHA256(X25519(myEphPriv, devEphPub), salt=connNonce, info="onloi-gw-session-v1")
    uint8_t shared_secret[32];
    if (!crypto_ecdh_shared_secret(peer->gw_eph_priv, peer->dev_eph_pub, shared_secret)) {
      ESP_LOGE(TAG, "X25519 scalarmult shared secret computation failed");
      return ESP_FAIL;
    }

    const uint8_t *info = (const uint8_t *)"onloi-gw-session-v1";
    if (!crypto_hkdf_sha256(shared_secret, 32, peer->conn_nonce, 8,
                            info, strlen((char *)info), peer->session_key, 32)) {
      ESP_LOGE(TAG, "HKDF sessionKey derivation failed");
      return ESP_FAIL;
    }

    peer->state = PEER_STATE_AUTHENTICATED;
    peer->last_remote_seq = resp.seq;
    peer->has_remote_seq = true;
    peer->reconnect_backoff_sec = 5;
    strncpy(peer->device_id, subkey->device_id, sizeof(peer->device_id) - 1);

    ESP_LOGI(TAG, "=== SESSION_INIT SUCCESSFUL! MAC: " MACSTR " AUTHENTICATED ===", MAC2STR(peer->mac));
    return ESP_OK;
  }

  ESP_LOGE(TAG, "SESSION_INIT failed after 3 BUSY retries (status=0x%02X)", last_status);
  return (last_status == BLE_STATUS_BUSY) ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

// Passthrough command relay (/command, /sub_ota)
static void ble_execute_passthrough_cmd(peer_ctx_t *peer, const ble_lock_cmd_t *cmd) {
  if (!peer || !cmd) return;

  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           cmd->mac[0], cmd->mac[1], cmd->mac[2], cmd->mac[3], cmd->mac[4], cmd->mac[5]);

  // 1. Look up subkey in NVS (or query from server if missing)
  subkey_entry_t subkey;
  esp_err_t key_err = ESP_ERR_NOT_FOUND;
  if (strlen(cmd->target_device_id) > 0) {
    key_err = subkey_get_by_device_id(cmd->target_device_id, &subkey);
  }
  if (key_err != ESP_OK) {
    key_err = subkey_get_by_mac(cmd->mac, &subkey);
  }

  if (key_err != ESP_OK && strlen(cmd->target_device_id) > 0) {
    ESP_LOGW(TAG, "Subkey not found locally in NVS. Requesting from server for device '%s'...",
             cmd->target_device_id);
    if (mqtt_manager_request_subkey(cmd->target_device_id, 4000) == ESP_OK) {
      key_err = subkey_get_by_device_id(cmd->target_device_id, &subkey);
    }
  }

  if (key_err != ESP_OK) {
    ESP_LOGE(TAG, "Subkey missing for device (MAC: %s, targetDeviceId: '%s')",
             mac_str, cmd->target_device_id);
    mqtt_manager_publish_command_ack(cmd->request_id, cmd->target_device_id, cmd->mac,
                                     "error", "key_not_found");
    return;
  }

  // 2. Ensure SESSION_INIT completed before writing command
  if (peer->state != PEER_STATE_AUTHENTICATED) {
    esp_err_t s_err = ble_execute_session_init(peer, &subkey);
    if (s_err != ESP_OK) {
      ESP_LOGE(TAG, "SESSION_INIT failed before passthrough command");
      mqtt_manager_publish_command_ack(cmd->request_id, cmd->target_device_id, cmd->mac,
                                       "error",
                                       (s_err == ESP_ERR_INVALID_RESPONSE) ? "AUTH_FAILED" : "SESSION_INIT_FAILED");
      return;
    }
  }

  // 3. Set active command context for notify relay correlation (preserved across multi-step flows)
  memcpy(&peer->active_cmd, cmd, sizeof(ble_lock_cmd_t));
  peer->has_active_cmd = true;
  if (strlen(cmd->request_id) > 0) {
    strncpy(peer->last_request_id, cmd->request_id, sizeof(peer->last_request_id) - 1);
  }

  // 3b. Automatic Downlink Codec: encrypt plaintext / AUTH_TAG v2.19 frames under sessionKey
  const uint8_t *tx_ptr = cmd->cmd_data;
  uint16_t tx_len = cmd->cmd_len;
  uint8_t *enc_tx_buf = NULL;

  if (!cmd->is_encrypted && cmd->cmd_len >= 19 && cmd->cmd_data[0] == BLE_FRAME_SOF) {
    ble_frame_t in_frame;
    if (ble_frame_parse_with_auth(cmd->cmd_data, cmd->cmd_len, &in_frame, NULL, NULL) &&
        in_frame.cmd != BLE_CMD_SESSION_INIT && in_frame.cmd != BLE_CMD_KEX_INIT) {
      uint16_t enc_payload_len = in_frame.len + CRYPTO_AES_TAG_SIZE;
      enc_tx_buf = malloc(19U + enc_payload_len + enc_payload_len);
      if (enc_tx_buf != NULL) {
        uint8_t *enc_payload = enc_tx_buf + 19U + enc_payload_len;
        uint16_t seq = ++peer->local_seq;
        uint8_t iv[CRYPTO_AES_IV_SIZE];
        build_session_gcm_nonce(peer->conn_nonce, seq, iv);

        uint8_t dummy = 0;
        const uint8_t *pt = (in_frame.len > 0) ? in_frame.payload : &dummy;
        if (crypto_aes_gcm_encrypt(peer->session_key, iv, pt, in_frame.len, NULL, 0,
                                   enc_payload, enc_payload + in_frame.len)) {
          tx_len = ble_frame_build_with_type(
              in_frame.type, in_frame.cmd, seq, in_frame.status,
              in_frame.timestamp, enc_payload, enc_payload_len, enc_tx_buf);
          tx_ptr = enc_tx_buf;
          ESP_LOGI(TAG, "Downlink Codec: encrypted cmd=0x%02X seq=%u (%u -> %u bytes)",
                   in_frame.cmd, seq, cmd->cmd_len, tx_len);
        } else {
          free(enc_tx_buf);
          enc_tx_buf = NULL;
        }
      }
    }
  }

  // 4. Send command with exponential backoff for BUSY (1s, 2s, 4s)
  const uint32_t busy_backoff_ms[3] = {1000, 2000, 4000};
  bool success = false;

  for (int attempt = 0; attempt < 3; attempt++) {
    peer->write_done = false;
    while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
    while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}

    ESP_LOGI(TAG, "Writing passthrough command (%u bytes) to handle %d (attempt %d/3)...",
             tx_len, peer->rx_val_handle, attempt + 1);

    int rc = ble_gattc_write_flat(peer->conn_handle, peer->rx_val_handle,
                                  tx_ptr, tx_len, ble_on_cmd_write, NULL);
    if (rc != 0) {
      ESP_LOGE(TAG, "Failed to write command frame; rc=%d", rc);
      mqtt_manager_publish_command_ack(cmd->request_id, cmd->target_device_id, cmd->mac,
                                       "error", "GATT write failed");
      peer->has_active_cmd = false;
      if (enc_tx_buf) free(enc_tx_buf);
      return;
    }

    if (xSemaphoreTake(peer->write_sem, pdMS_TO_TICKS(4000)) != pdTRUE || !peer->write_done) {
      ESP_LOGE(TAG, "GATT write ACK timed out or failed");
      mqtt_manager_publish_command_ack(cmd->request_id, cmd->target_device_id, cmd->mac,
                                       "error", "Write timeout");
      peer->has_active_cmd = false;
      if (enc_tx_buf) free(enc_tx_buf);
      return;
    }

    // Wait for peripheral notification response
    uint32_t wait_timeout = (cmd->timeout_ms > 0) ? cmd->timeout_ms : 8000;
    if (xSemaphoreTake(peer->notify_sem, pdMS_TO_TICKS(wait_timeout)) == pdTRUE) {
      if (peer->rx_notify_len >= 9 && peer->rx_notify_buf[8] == BLE_STATUS_BUSY) {
        ESP_LOGW(TAG, "Peripheral response was BUSY (0x06). Retrying after backoff %lu ms...",
                 (unsigned long)busy_backoff_ms[attempt]);
        vTaskDelay(pdMS_TO_TICKS(busy_backoff_ms[attempt]));
        continue;
      }
      success = true;
      break;
    } else {
      ESP_LOGW(TAG, "Notification wait timeout (%lu ms) reached", (unsigned long)wait_timeout);
      break;
    }
  }

  if (enc_tx_buf) free(enc_tx_buf);
  peer->has_active_cmd = false;

  if (!success) {
    ESP_LOGE(TAG, "Passthrough command failed or timed out for MAC %s", mac_str);
    mqtt_manager_publish_command_ack(cmd->request_id, cmd->target_device_id, cmd->mac,
                                     "error", "Command timeout/busy");
  } else {
    ESP_LOGI(TAG, "Passthrough command executed successfully. Maintaining persistent connection.");
  }
}

// Sub-Device Claim (Section 3)
static void ble_execute_device_claim(peer_ctx_t *peer, const ble_lock_cmd_t *cmd) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           cmd->mac[0], cmd->mac[1], cmd->mac[2], cmd->mac[3], cmd->mac[4], cmd->mac[5]);

  ESP_LOGI(TAG, "==================================================");
  ESP_LOGI(TAG, "STARTING SUB-DEVICE CLAIM FOR MAC: %s (ReqID: %s)", mac_str, cmd->request_id);
  ESP_LOGI(TAG, "==================================================");

  if (peer->rx_val_handle == 0 || peer->tx_val_handle == 0) {
    ESP_LOGE(TAG, "Claim failed: RX/TX characteristics missing (rx=%d, tx=%d)",
             peer->rx_val_handle, peer->tx_val_handle);
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "GATT Chars Missing");
    return;
  }

  // STEP 1: KEX_INIT (0x71) REQUEST
  uint8_t gw_priv[CRYPTO_X25519_KEY_SIZE];
  uint8_t gw_pub[CRYPTO_X25519_KEY_SIZE];
  if (!crypto_generate_x25519_keypair(gw_priv, gw_pub)) {
    ESP_LOGE(TAG, "Failed to generate Gateway X25519 keypair");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "Keygen Failed");
    return;
  }

  uint8_t kex_req_payload[33];
  kex_req_payload[0] = 0x01; // Role: Gateway
  memcpy(&kex_req_payload[1], gw_pub, CRYPTO_X25519_KEY_SIZE);

  uint8_t tx_buf[128];
  uint16_t tx_len = ble_frame_build_req(BLE_CMD_KEX_INIT, 1, kex_req_payload,
                                        sizeof(kex_req_payload), tx_buf);

  peer->write_done = false;
  while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
  while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}

  int rc = ble_gattc_write_flat(peer->conn_handle, peer->rx_val_handle, tx_buf, tx_len,
                                ble_on_cmd_write, NULL);
  if (rc != 0 || xSemaphoreTake(peer->write_sem, pdMS_TO_TICKS(4000)) != pdTRUE || !peer->write_done) {
    ESP_LOGE(TAG, "Failed to write KEX_INIT frame");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "KEX_INIT Write Failed");
    return;
  }

  if (xSemaphoreTake(peer->notify_sem, pdMS_TO_TICKS(6000)) != pdTRUE) {
    ESP_LOGE(TAG, "Timeout waiting for KEX_INIT RESPONSE from device");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "KEX_INIT Timeout");
    return;
  }

  ble_frame_t kex_resp;
  if (!ble_frame_parse(peer->rx_notify_buf, peer->rx_notify_len, &kex_resp)) {
    ESP_LOGE(TAG, "Failed to parse KEX_INIT RESPONSE frame");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "Invalid KEX_INIT Frame");
    return;
  }

  if (kex_resp.cmd != BLE_CMD_KEX_INIT || kex_resp.type != BLE_MSG_TYPE_RESP ||
      kex_resp.status != BLE_STATUS_OK || kex_resp.len < 56) {
    ESP_LOGE(TAG, "KEX_INIT RESPONSE rejected: cmd=0x%02X status=0x%02X len=%u",
             kex_resp.cmd, kex_resp.status, kex_resp.len);
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "KEX_INIT Rejected");
    return;
  }

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

  ESP_LOGI(TAG, "KEX_INIT Success! Lock deviceId='%s', deviceCode='%s'",
           lock_device_id, lock_device_code);

  // STEP 2: Derive claimSessionKey
  uint8_t shared_secret[CRYPTO_X25519_KEY_SIZE];
  if (!crypto_ecdh_shared_secret(gw_priv, lock_pub, shared_secret)) {
    ESP_LOGE(TAG, "ECDH shared secret failed");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "ECDH Failed");
    return;
  }

  uint8_t device_id_16[16] = {0};
  memcpy(device_id_16, &kex_resp.payload[32], 16);

  const uint8_t *info = (const uint8_t *)"onloi-gw-session-v1";
  uint8_t claim_session_key[CRYPTO_AES_KEY_SIZE];
  if (!crypto_hkdf_sha256(shared_secret, sizeof(shared_secret), device_id_16,
                          sizeof(device_id_16), info, strlen((char *)info),
                          claim_session_key, sizeof(claim_session_key))) {
    ESP_LOGE(TAG, "HKDF derivation failed");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "HKDF Failed");
    return;
  }

  // STEP 3: KEY_DELIVER (0x72) REQUEST
  uint8_t new_local_key[CRYPTO_LOCAL_KEY_SIZE];
  if (!crypto_generate_random(new_local_key, sizeof(new_local_key))) {
    ESP_LOGE(TAG, "RNG failed for new localKey");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "RNG Failed");
    return;
  }

  uint8_t iv[CRYPTO_AES_IV_SIZE];
  crypto_generate_random(iv, sizeof(iv));

  uint8_t ciphertext[CRYPTO_LOCAL_KEY_SIZE];
  uint8_t tag[CRYPTO_AES_TAG_SIZE];
  uint8_t purpose = 0x01; // CLAIM

  if (!crypto_aes_gcm_encrypt(claim_session_key, iv, new_local_key,
                              sizeof(new_local_key), NULL, 0, ciphertext, tag)) {
    ESP_LOGE(TAG, "AES-GCM encryption of localKey failed");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "GCM Encrypt Failed");
    return;
  }

  uint8_t deliver_payload[61];
  deliver_payload[0] = purpose;
  memcpy(&deliver_payload[1], iv, CRYPTO_AES_IV_SIZE);
  memcpy(&deliver_payload[13], ciphertext, CRYPTO_LOCAL_KEY_SIZE);
  memcpy(&deliver_payload[45], tag, CRYPTO_AES_TAG_SIZE);

  tx_len = ble_frame_build_req(BLE_CMD_KEY_DELIVER, 2, deliver_payload,
                               sizeof(deliver_payload), tx_buf);

  peer->write_done = false;
  while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
  while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}

  rc = ble_gattc_write_flat(peer->conn_handle, peer->rx_val_handle, tx_buf, tx_len,
                            ble_on_cmd_write, NULL);
  if (rc != 0 || xSemaphoreTake(peer->write_sem, pdMS_TO_TICKS(4000)) != pdTRUE || !peer->write_done) {
    ESP_LOGE(TAG, "KEY_DELIVER write failed or timed out");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "KEY_DELIVER Write Failed");
    return;
  }

  if (xSemaphoreTake(peer->notify_sem, pdMS_TO_TICKS(6000)) != pdTRUE) {
    ESP_LOGE(TAG, "Timeout waiting for KEY_DELIVER RESPONSE");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "KEY_DELIVER Timeout");
    return;
  }

  ble_frame_t deliver_resp;
  if (!ble_frame_parse(peer->rx_notify_buf, peer->rx_notify_len, &deliver_resp) ||
      deliver_resp.cmd != BLE_CMD_KEY_DELIVER || deliver_resp.status != BLE_STATUS_OK) {
    ESP_LOGE(TAG, "KEY_DELIVER rejected by lock");
    mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, NULL, NULL, NULL, 0, "error", "KEY_DELIVER Rejected");
    return;
  }

  // STEP 4: Store subkey in NVS BEFORE publishing claim ack! (Requirement 1)
  subkey_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  memcpy(entry.mac, cmd->mac, 6);
  entry.addr_type = cmd->addr_type;
  strncpy(entry.device_id, lock_device_id, sizeof(entry.device_id) - 1);
  memcpy(entry.local_key, new_local_key, 32);
  entry.key_version = 1;
  subkey_put(&entry);

  ESP_LOGI(TAG, "Saved new localKey to NVS subkey store BEFORE publishing claim ack.");

  // Keep connection open and authenticated
  peer->state = PEER_STATE_AUTHENTICATED;
  peer->last_remote_seq = deliver_resp.seq;
  strncpy(peer->device_id, lock_device_id, sizeof(peer->device_id) - 1);
  memcpy(peer->session_key, claim_session_key, 32);

  // STEP 5: Publish claim ack
  char local_key_b64[64] = {0};
  size_t b64_len = 0;
  mbedtls_base64_encode((unsigned char *)local_key_b64, sizeof(local_key_b64),
                        &b64_len, new_local_key, sizeof(new_local_key));
  local_key_b64[b64_len] = '\0';

  ESP_LOGI(TAG, "=== SUB-DEVICE CLAIM SUCCESSFUL! ===");
  ESP_LOGI(TAG, "MAC: %s | DeviceId: %s | DeviceCode: %s", mac_str, lock_device_id, lock_device_code);

  mqtt_manager_publish_claim_ack(cmd->request_id, mac_str, lock_device_id,
                                 lock_device_code, local_key_b64, 1, "ok", NULL);
}

// Sub-Device Soft Unclaim (0x73) (Section 10 Step 2, Section 11.6)
static void ble_execute_device_unclaim(peer_ctx_t *peer, const ble_lock_cmd_t *cmd) {
  if (!peer || !cmd) return;

  subkey_entry_t subkey;
  esp_err_t key_err = subkey_get_by_mac(cmd->mac, &subkey);
  if (key_err != ESP_OK && strlen(cmd->target_device_id) > 0) {
    key_err = subkey_get_by_device_id(cmd->target_device_id, &subkey);
  }

  if (key_err != ESP_OK) {
    ESP_LOGE(TAG, "Cannot unclaim: Subkey not found in NVS");
    mqtt_manager_publish_unclaim_ack(cmd->request_id, cmd->target_device_id, "error", "Device not in NVS");
    return;
  }

  if (peer->state != PEER_STATE_AUTHENTICATED) {
    if (ble_execute_session_init(peer, &subkey) != ESP_OK) {
      ESP_LOGE(TAG, "SESSION_INIT failed before UNCLAIM");
      mqtt_manager_publish_unclaim_ack(cmd->request_id, cmd->target_device_id, "error", "SESSION_INIT failed");
      return;
    }
  }

  // Encrypt UNCLAIM (0x73) under sessionKey:
  // reason: 0x01 (user request)
  uint8_t iv[12];
  crypto_generate_random(iv, 12);
  uint8_t reason = 0x01;
  uint8_t ciphertext[1];
  uint8_t tag[16];

  if (!crypto_aes_gcm_encrypt(peer->session_key, iv, &reason, 1, NULL, 0, ciphertext, tag)) {
    ESP_LOGE(TAG, "Failed to encrypt UNCLAIM payload");
    mqtt_manager_publish_unclaim_ack(cmd->request_id, cmd->target_device_id, "error", "Encrypt failed");
    return;
  }

  uint8_t unclaim_payload[29];
  memcpy(unclaim_payload, iv, 12);
  unclaim_payload[12] = ciphertext[0];
  memcpy(unclaim_payload + 13, tag, 16);

  uint8_t tx_buf[64];
  uint16_t seq = ++peer->local_seq;
  uint16_t tx_len = ble_frame_build_req(BLE_CMD_UNCLAIM, seq, unclaim_payload,
                                        sizeof(unclaim_payload), tx_buf);

  peer->write_done = false;
  while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
  while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}

  int rc = ble_gattc_write_flat(peer->conn_handle, peer->rx_val_handle,
                                tx_buf, tx_len, ble_on_cmd_write, NULL);
  if (rc != 0 || xSemaphoreTake(peer->write_sem, pdMS_TO_TICKS(4000)) != pdTRUE || !peer->write_done) {
    ESP_LOGE(TAG, "Failed to write UNCLAIM frame");
    mqtt_manager_publish_unclaim_ack(cmd->request_id, cmd->target_device_id, "error", "Write failed");
    return;
  }

  if (xSemaphoreTake(peer->notify_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Timeout waiting for UNCLAIM RESPONSE");
    mqtt_manager_publish_unclaim_ack(cmd->request_id, cmd->target_device_id, "error", "Timeout");
    return;
  }

  ble_frame_t resp;
  if (!ble_frame_parse(peer->rx_notify_buf, peer->rx_notify_len, &resp) ||
      resp.cmd != BLE_CMD_UNCLAIM || resp.status != BLE_STATUS_OK) {
    ESP_LOGE(TAG, "UNCLAIM rejected by device (status 0x%02X)", resp.status);
    mqtt_manager_publish_unclaim_ack(cmd->request_id, cmd->target_device_id, "error", "Device rejected");
    return;
  }

  // Delete key from NVS after device confirms OK!
  subkey_delete_by_mac(peer->mac);
  if (strlen(cmd->target_device_id) > 0) {
    subkey_delete_by_device_id(cmd->target_device_id);
  }

  ESP_LOGI(TAG, "UNCLAIM successful for MAC " MACSTR "! Subkey erased from NVS.", MAC2STR(peer->mac));
  mqtt_manager_publish_unclaim_ack(cmd->request_id, cmd->target_device_id, "ok", NULL);

  // Terminate connection as device is now unbonded
  ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

// Sub-Device Key Rotate (KEY_DELIVER purpose=0x02) (Section 10 Step 1)
static void ble_execute_key_rotate(peer_ctx_t *peer, const ble_lock_cmd_t *cmd) {
  if (!peer || !cmd) return;

  subkey_entry_t subkey;
  esp_err_t key_err = subkey_get_by_mac(cmd->mac, &subkey);
  if (key_err != ESP_OK && strlen(cmd->target_device_id) > 0) {
    key_err = subkey_get_by_device_id(cmd->target_device_id, &subkey);
  }

  if (key_err != ESP_OK) {
    ESP_LOGE(TAG, "Cannot rotate key: Subkey not found in NVS");
    mqtt_manager_publish_keyrot_ack(cmd->request_id, cmd->target_device_id, NULL, 0, "error", "Device not in NVS");
    return;
  }

  if (peer->state != PEER_STATE_AUTHENTICATED) {
    if (ble_execute_session_init(peer, &subkey) != ESP_OK) {
      ESP_LOGE(TAG, "SESSION_INIT failed before KEY_ROTATE");
      mqtt_manager_publish_keyrot_ack(cmd->request_id, cmd->target_device_id, NULL, 0, "error", "SESSION_INIT failed");
      return;
    }
  }

  uint8_t new_local_key[32];
  if (!crypto_generate_random(new_local_key, 32)) {
    ESP_LOGE(TAG, "Failed to generate new random key");
    mqtt_manager_publish_keyrot_ack(cmd->request_id, cmd->target_device_id, NULL, 0, "error", "RNG failed");
    return;
  }

  uint8_t iv[12];
  crypto_generate_random(iv, 12);
  uint8_t ciphertext[32];
  uint8_t tag[16];

  if (!crypto_aes_gcm_encrypt(peer->session_key, iv, new_local_key, 32, NULL, 0, ciphertext, tag)) {
    ESP_LOGE(TAG, "Failed to encrypt new localKey under sessionKey");
    mqtt_manager_publish_keyrot_ack(cmd->request_id, cmd->target_device_id, NULL, 0, "error", "Encrypt failed");
    return;
  }

  uint8_t deliver_payload[61];
  deliver_payload[0] = 0x02; // ROTATE
  memcpy(deliver_payload + 1, iv, 12);
  memcpy(deliver_payload + 13, ciphertext, 32);
  memcpy(deliver_payload + 45, tag, 16);

  uint8_t tx_buf[128];
  uint16_t seq = ++peer->local_seq;
  uint16_t tx_len = ble_frame_build_req(BLE_CMD_KEY_DELIVER, seq, deliver_payload,
                                        sizeof(deliver_payload), tx_buf);

  peer->write_done = false;
  while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
  while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}

  int rc = ble_gattc_write_flat(peer->conn_handle, peer->rx_val_handle,
                                tx_buf, tx_len, ble_on_cmd_write, NULL);
  if (rc != 0 || xSemaphoreTake(peer->write_sem, pdMS_TO_TICKS(4000)) != pdTRUE || !peer->write_done) {
    ESP_LOGE(TAG, "Failed to write KEY_DELIVER frame");
    mqtt_manager_publish_keyrot_ack(cmd->request_id, cmd->target_device_id, NULL, 0, "error", "Write failed");
    return;
  }

  if (xSemaphoreTake(peer->notify_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Timeout waiting for KEY_DELIVER RESPONSE");
    mqtt_manager_publish_keyrot_ack(cmd->request_id, cmd->target_device_id, NULL, 0, "error", "Timeout");
    return;
  }

  ble_frame_t resp;
  if (!ble_frame_parse(peer->rx_notify_buf, peer->rx_notify_len, &resp) ||
      resp.cmd != BLE_CMD_KEY_DELIVER || resp.status != BLE_STATUS_OK) {
    ESP_LOGE(TAG, "KEY_DELIVER (ROTATE) rejected by device (status 0x%02X)", resp.status);
    mqtt_manager_publish_keyrot_ack(cmd->request_id, cmd->target_device_id, NULL, 0, "error", "Device rejected");
    return;
  }

  // Update NVS key
  subkey.key_version = (cmd->current_key_version > 0) ? cmd->current_key_version + 1 : subkey.key_version + 1;
  memcpy(subkey.local_key, new_local_key, 32);
  subkey_put(&subkey);

  char b64_key[64];
  size_t b64_len = 0;
  mbedtls_base64_encode((unsigned char *)b64_key, sizeof(b64_key), &b64_len, new_local_key, 32);
  b64_key[b64_len] = '\0';

  ESP_LOGI(TAG, "KEY_ROTATE successful! New keyVersion=%lu stored in NVS", (unsigned long)subkey.key_version);
  mqtt_manager_publish_keyrot_ack(cmd->request_id, cmd->target_device_id, b64_key,
                                  subkey.key_version, "ok", NULL);
}

static void build_session_gcm_nonce(const uint8_t conn_nonce[8], uint16_t seq, uint8_t out_iv[12]) {
  memset(out_iv, 0, 12);
  memcpy(out_iv, conn_nonce, 8);
  uint64_t s = (uint64_t)seq;
  for (int i = 0; i < 8; i++) {
    out_iv[4 + i] ^= (uint8_t)((s >> ((7 - i) * 8)) & 0xFF);
  }
}

static void ble_write_be32(uint8_t *p, uint32_t v) {
  p[0] = (v >> 24) & 0xFF;
  p[1] = (v >> 16) & 0xFF;
  p[2] = (v >> 8) & 0xFF;
  p[3] = v & 0xFF;
}

static void ble_write_be16(uint8_t *p, uint16_t v) {
  p[0] = (v >> 8) & 0xFF;
  p[1] = v & 0xFF;
}

static esp_err_t ble_send_encrypted_frame(peer_ctx_t *peer, uint8_t cmd_code, bool is_request,
                                          const uint8_t *plaintext, uint16_t pt_len,
                                          uint8_t *work_buf, uint16_t work_buf_size,
                                          uint32_t resp_timeout_ms, ble_frame_t *out_resp) {
  uint16_t enc_len = pt_len + CRYPTO_AES_TAG_SIZE;
  if ((uint32_t)(19U + enc_len + enc_len) > work_buf_size) {
    return ESP_ERR_NO_MEM;
  }

  uint16_t seq = ++peer->local_seq;
  uint8_t iv[CRYPTO_AES_IV_SIZE];
  build_session_gcm_nonce(peer->conn_nonce, seq, iv);

  uint8_t *enc_payload = work_buf;
  uint8_t *tx_buf = work_buf + enc_len;

  if (pt_len > 0) {
    if (!crypto_aes_gcm_encrypt(peer->session_key, iv, plaintext, pt_len, NULL, 0,
                                enc_payload, enc_payload + pt_len)) {
      return ESP_FAIL;
    }
  } else {
    // 0-byte plaintext -> 16-byte GCM auth tag
    uint8_t dummy = 0;
    if (!crypto_aes_gcm_encrypt(peer->session_key, iv, &dummy, 0, NULL, 0,
                                enc_payload, enc_payload)) {
      return ESP_FAIL;
    }
  }

  uint16_t tx_len = is_request
      ? ble_frame_build_req(cmd_code, seq, enc_payload, enc_len, tx_buf)
      : ble_frame_build_cmd(cmd_code, seq, enc_payload, enc_len, tx_buf);

  peer->write_done = false;
  while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
  if (is_request) {
    while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}
  }

  int rc = ble_gattc_write_flat(peer->conn_handle, peer->rx_val_handle,
                                tx_buf, tx_len, ble_on_cmd_write, NULL);
  if (rc != 0 || xSemaphoreTake(peer->write_sem, pdMS_TO_TICKS(4000)) != pdTRUE || !peer->write_done) {
    return ESP_FAIL;
  }

  if (!is_request) {
    return ESP_OK;
  }

  if (xSemaphoreTake(peer->notify_sem, pdMS_TO_TICKS(resp_timeout_ms)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  if (out_resp != NULL) {
    if (!ble_frame_parse(peer->rx_notify_buf, peer->rx_notify_len, out_resp)) {
      return ESP_ERR_INVALID_RESPONSE;
    }
  }
  return ESP_OK;
}

static void ble_send_ota_abort(peer_ctx_t *peer, uint8_t reason, uint8_t *work_buf, uint16_t work_buf_size) {
  ble_frame_t resp;
  ble_send_encrypted_frame(peer, BLE_CMD_BLE_OTA_ABORT, true, &reason, 1,
                           work_buf, work_buf_size, 3000, &resp);
}

// Sub-device BLE OTA Transfer Engine (0x78, 0x79, 0x7A, 0x7B)
static void ble_execute_sub_ota(peer_ctx_t *peer, const ble_lock_cmd_t *cmd) {
  if (!peer || !cmd) return;
  const ble_sub_ota_meta_t *m = &cmd->sub_ota;

  subkey_entry_t subkey;
  esp_err_t key_err = subkey_get_by_mac(cmd->mac, &subkey);
  if (key_err != ESP_OK && strlen(cmd->target_device_id) > 0) {
    key_err = subkey_get_by_device_id(cmd->target_device_id, &subkey);
  }
  if (key_err != ESP_OK) {
    mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                         FW_STATE_FAILED, 0, BLE_STATUS_SUB_NOT_FOUND, 0, 0);
    return;
  }

  if (peer->state != PEER_STATE_AUTHENTICATED) {
    if (ble_execute_session_init(peer, &subkey) != ESP_OK) {
      mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                           FW_STATE_FAILED, 0, BLE_STATUS_AUTH_FAILED, 0, 0);
      return;
    }
  }

  const esp_partition_t *sub_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "sub_ota");
  if (!sub_part) {
    mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                         FW_STATE_FAILED, 0, BLE_STATUS_INSUFFICIENT_STORAGE, 0, 0);
    return;
  }

  // Allocate heap buffers to avoid overflowing task stack (WP7)
  const uint16_t work_buf_size = 1024;
  uint8_t *work_buf = malloc(work_buf_size);
  uint8_t *pt_buf = malloc(512);
  if (!work_buf || !pt_buf) {
    free(work_buf);
    free(pt_buf);
    mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                         FW_STATE_FAILED, 0, BLE_STATUS_INSUFFICIENT_STORAGE, 0, 0);
    return;
  }

  uint16_t chunk_size = 240;
  if (peer->att_mtu > 64) {
    uint16_t max_att_data = peer->att_mtu - 3;
    // Frame overhead: 17 hdr + 2 crc + 16 gcm_tag + 6 chunk_hdr = 41 bytes
    if (max_att_data > 41 + 240 && max_att_data >= 41 + 480) {
      chunk_size = 480;
    } else if (max_att_data < 41 + chunk_size) {
      chunk_size = (max_att_data > 41 + 32) ? (max_att_data - 41) : 128;
    }
  }

  // Step 1: BLE_OTA_START (0x78) - 54 bytes plaintext
  memset(pt_buf, 0, 54);
  ble_write_be32(pt_buf + 0, m->ota_request_id);
  pt_buf[4] = m->unit_target;
  ble_write_be16(pt_buf + 5, m->product_id);
  pt_buf[7] = m->hw_rev;
  ble_write_be32(pt_buf + 8, m->fw_version);
  ble_write_be32(pt_buf + 12, m->total_length);
  ble_write_be32(pt_buf + 16, m->plain_fw_crc32);
  memcpy(pt_buf + 20, m->plain_fw_sha256, 32);
  ble_write_be16(pt_buf + 52, chunk_size);

  ble_frame_t resp;
  esp_err_t err = ble_send_encrypted_frame(peer, BLE_CMD_BLE_OTA_START, true, pt_buf, 54,
                                           work_buf, work_buf_size, 5000, &resp);
  if (err != ESP_OK || resp.cmd != BLE_CMD_BLE_OTA_START || resp.status != BLE_STATUS_OK) {
    uint8_t st = (err == ESP_OK) ? resp.status : BLE_STATUS_TIMEOUT;
    ESP_LOGE(TAG, "BLE_OTA_START failed (err=%d, status=0x%02X)", err, st);
    mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                         FW_STATE_FAILED, 0, st, 0, 0);
    free(work_buf);
    free(pt_buf);
    return;
  }

  if (resp.len >= 2) {
    uint16_t dev_max_chunk = ((uint16_t)resp.payload[0] << 8) | resp.payload[1];
    if (dev_max_chunk >= 32 && dev_max_chunk < chunk_size) {
      chunk_size = dev_max_chunk;
    }
  }
  ESP_LOGI(TAG, "BLE_OTA_START OK! Transferring %lu bytes in %u-byte chunks...",
           (unsigned long)m->total_length, chunk_size);

  // Step 2: BLE_OTA_CHUNK (0x79) Windowed ACK Loop (window = 16)
  uint32_t offset = 0;
  uint8_t pkt_in_win = 0;
  uint8_t retries = 0;
  uint8_t last_pct = 0;

  while (offset < m->total_length) {
    if (ota_manager_is_aborted(m->ota_request_id)) {
      ESP_LOGW(TAG, "BLE_OTA aborted by server!");
      ble_send_ota_abort(peer, 0x01, work_buf, work_buf_size);
      mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                           FW_STATE_ABORTED, last_pct, 0, offset, 0);
      free(work_buf);
      free(pt_buf);
      return;
    }

    uint32_t rem = m->total_length - offset;
    uint16_t n = (rem > chunk_size) ? chunk_size : (uint16_t)rem;
    bool is_win_end = ((pkt_in_win + 1) >= 16) || ((offset + n) >= m->total_length);

    ble_write_be32(pt_buf + 0, offset);
    ble_write_be16(pt_buf + 4, n);
    if (esp_partition_read(sub_part, offset, pt_buf + 6, n) != ESP_OK) {
      ble_send_ota_abort(peer, 0x02, work_buf, work_buf_size);
      mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                           FW_STATE_FAILED, last_pct, BLE_STATUS_INSUFFICIENT_STORAGE, offset, 0);
      free(work_buf);
      free(pt_buf);
      return;
    }

    err = ble_send_encrypted_frame(peer, BLE_CMD_BLE_OTA_CHUNK, is_win_end, pt_buf, (uint16_t)(6 + n),
                                   work_buf, work_buf_size, 5000, is_win_end ? &resp : NULL);
    if (err != ESP_OK) {
      if (retries < 3) {
        retries++;
        pkt_in_win = 0;
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      ble_send_ota_abort(peer, 0x02, work_buf, work_buf_size);
      mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                           FW_STATE_FAILED, last_pct, BLE_STATUS_TIMEOUT, offset, 0);
      free(work_buf);
      free(pt_buf);
      return;
    }

    if (is_win_end) {
      if (resp.cmd != BLE_CMD_BLE_OTA_CHUNK || resp.status != BLE_STATUS_OK) {
        if (retries < 3) {
          retries++;
          pkt_in_win = 0;
          continue;
        }
        mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                             FW_STATE_FAILED, last_pct, resp.status, offset, 0);
        free(work_buf);
        free(pt_buf);
        return;
      }
      uint32_t next_expected = offset + n;
      if (resp.len >= 4) {
        next_expected = ((uint32_t)resp.payload[0] << 24) | ((uint32_t)resp.payload[1] << 16) |
                        ((uint32_t)resp.payload[2] << 8) | resp.payload[3];
      }
      if (next_expected < offset + n) {
        if (retries < 3) {
          ESP_LOGW(TAG, "BLE_OTA_CHUNK rewind: nextExpected=%lu < %lu (retry %u/3)",
                   (unsigned long)next_expected, (unsigned long)(offset + n), retries + 1);
          offset = next_expected;
          pkt_in_win = 0;
          retries++;
          continue;
        }
        ble_send_ota_abort(peer, 0x02, work_buf, work_buf_size);
        mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                             FW_STATE_FAILED, last_pct, BLE_STATUS_TIMEOUT, offset, 0);
        free(work_buf);
        free(pt_buf);
        return;
      }
      offset = next_expected;
      pkt_in_win = 0;
      retries = 0;
    } else {
      offset += n;
      pkt_in_win++;
    }

    uint8_t pct = (uint8_t)(((uint64_t)offset * 100ULL) / m->total_length);
    if (pct >= last_pct + 10 || offset >= m->total_length) {
      last_pct = pct;
      mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                           FW_STATE_BLE_SUB_TRANSFERRING, pct, 0, offset, 0);
    }
  }

  // Step 3: BLE_OTA_FINISH (0x7A)
  mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                       FW_STATE_BLE_SUB_VERIFYING, 100, 0, m->total_length, 0);

  err = ble_send_encrypted_frame(peer, BLE_CMD_BLE_OTA_FINISH, true, NULL, 0,
                                 work_buf, work_buf_size, 15000, &resp);
  free(work_buf);
  free(pt_buf);

  if (err != ESP_OK || resp.cmd != BLE_CMD_BLE_OTA_FINISH || resp.status != BLE_STATUS_OK) {
    uint8_t st = (err == ESP_OK) ? resp.status : BLE_STATUS_TIMEOUT;
    ESP_LOGE(TAG, "BLE_OTA_FINISH failed (err=%d, status=0x%02X)", err, st);
    mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                         FW_STATE_FAILED, 100, st, m->total_length, 0);
    return;
  }

  if (m->unit_target == 0x01) {
    ESP_LOGI(TAG, "BLE_OTA_FINISH OK for sub-device ESP32! Device will reboot.");
    mqtt_manager_publish_firmware_status(m->ota_request_id, m->firmware_target,
                                         FW_STATE_COMPLETED, 100, 0, m->total_length, m->fw_version);
  } else {
    ESP_LOGI(TAG, "BLE_OTA_FINISH OK for sub-device STM32! Listening for STM32 UART status events...");
  }
}

// WP6: GET_FIRMWARE_INFO (0x5C) -> FIRMWARE_INFO (0x3A) for BLE Sub-device
static void ble_execute_get_fw_info(peer_ctx_t *peer, const ble_lock_cmd_t *cmd) {
  if (!peer || !cmd) return;

  subkey_entry_t subkey;
  esp_err_t key_err = subkey_get_by_mac(cmd->mac, &subkey);
  if (key_err != ESP_OK && strlen(cmd->target_device_id) > 0) {
    key_err = subkey_get_by_device_id(cmd->target_device_id, &subkey);
  }
  if (key_err != ESP_OK) {
    mqtt_manager_publish_firmware_status(cmd->sub_ota.ota_request_id, cmd->sub_ota.firmware_target,
                                         FW_STATE_FAILED, 0, BLE_STATUS_SUB_NOT_FOUND, 0, 0);
    return;
  }

  if (peer->state != PEER_STATE_AUTHENTICATED) {
    if (ble_execute_session_init(peer, &subkey) != ESP_OK) {
      mqtt_manager_publish_firmware_status(cmd->sub_ota.ota_request_id, cmd->sub_ota.firmware_target,
                                           FW_STATE_FAILED, 0, BLE_STATUS_AUTH_FAILED, 0, 0);
      return;
    }
  }

  uint8_t pl[21];
  memset(pl, 0, sizeof(pl));
  ble_write_be32(pl + 0, cmd->sub_ota.ota_request_id);
  pl[4] = cmd->sub_ota.firmware_target;
  strncpy((char *)(pl + 5), subkey.device_id, 16);

  uint8_t tx_buf[64];
  uint16_t seq = ++peer->local_seq;
  uint16_t tx_len = ble_frame_build_req(BLE_CMD_GET_FIRMWARE_INFO, seq, pl, sizeof(pl), tx_buf);

  peer->write_done = false;
  while (xSemaphoreTake(peer->write_sem, 0) == pdTRUE) {}
  while (xSemaphoreTake(peer->notify_sem, 0) == pdTRUE) {}

  int rc = ble_gattc_write_flat(peer->conn_handle, peer->rx_val_handle,
                                tx_buf, tx_len, ble_on_cmd_write, NULL);
  if (rc != 0 || xSemaphoreTake(peer->write_sem, pdMS_TO_TICKS(4000)) != pdTRUE || !peer->write_done) {
    mqtt_manager_publish_firmware_status(cmd->sub_ota.ota_request_id, cmd->sub_ota.firmware_target,
                                         FW_STATE_FAILED, 0, BLE_STATUS_TIMEOUT, 0, 0);
    return;
  }

  if (xSemaphoreTake(peer->notify_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
    ble_frame_t resp;
    if (ble_frame_parse(peer->rx_notify_buf, peer->rx_notify_len, &resp) &&
        resp.cmd == BLE_CMD_FIRMWARE_INFO) {
      mqtt_manager_publish_proto_event(peer->rx_notify_buf, peer->rx_notify_len);
    }
  }
}

// Background BLE Command & Persistent Connection Listener Task
static void ble_cmd_listener_task(void *pvParameters) {
  ble_lock_cmd_t cmd;
  ESP_LOGI(TAG, "BLE Command Listener Task started (stack watermark: %u bytes).",
           (unsigned)BLE_CMD_TASK_STACK_SIZE);

  while (1) {
    if (xQueueReceive(ble_cmd_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
      ESP_LOGI(TAG, "Processing operation %d for MAC " MACSTR ", ReqID: %s, target: %s",
               cmd.op_type, MAC2STR(cmd.mac), cmd.request_id, cmd.target_device_id);

      peer_ctx_t *peer = peer_find_by_mac(cmd.mac);
      if (peer == NULL && strlen(cmd.target_device_id) > 0) {
        peer = peer_find_by_device_id(cmd.target_device_id);
      }
      if (peer == NULL) {
        peer = peer_alloc_slot(cmd.mac, cmd.addr_type);
      }

      if (peer == NULL && cmd.op_type != BLE_OP_AUTO_CONNECT) {
        // All slots occupied by other MACs: close an idle slot to serve new target MAC
        for (int i = 0; i < BLE_MAX_PEERS; i++) {
          if (s_peers[i].state != PEER_STATE_DISCONNECTED &&
              s_peers[i].conn_handle != BLE_HS_CONN_HANDLE_NONE &&
              !s_peers[i].has_active_cmd) {
            ESP_LOGW(TAG, "Evicting idle peer " MACSTR " to connect new target " MACSTR,
                     MAC2STR(s_peers[i].mac), MAC2STR(cmd.mac));
            ble_gap_terminate(s_peers[i].conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            vTaskDelay(pdMS_TO_TICKS(300));
            peer_reset_state(&s_peers[i]);
            memcpy(s_peers[i].mac, cmd.mac, BLE_MAC_LEN);
            s_peers[i].addr_type = cmd.addr_type;
            peer = &s_peers[i];
            break;
          }
        }
      }

      if (peer == NULL) {
        ESP_LOGE(TAG, "Cannot allocate peer slot for MAC " MACSTR " (all %d slots full)",
                 MAC2STR(cmd.mac), BLE_MAX_PEERS);
        if (cmd.op_type == BLE_OP_PASSTHROUGH_CMD) {
          mqtt_manager_publish_command_ack(cmd.request_id, cmd.target_device_id, cmd.mac, "error", "No BLE connection slots");
        } else if (cmd.op_type == BLE_OP_DEVICE_CLAIM) {
          char mac_str[18];
          snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                   cmd.mac[0], cmd.mac[1], cmd.mac[2], cmd.mac[3], cmd.mac[4], cmd.mac[5]);
          mqtt_manager_publish_claim_ack(cmd.request_id, mac_str, NULL, NULL, NULL, 0, "error", "No connection slots");
        }
        continue;
      }

      if (strlen(cmd.target_device_id) > 0) {
        strncpy(peer->device_id, cmd.target_device_id, sizeof(peer->device_id) - 1);
      }

      // Connect if disconnected
      if (peer->state == PEER_STATE_DISCONNECTED) {
        if (ble_connect_to_peer(peer) != ESP_OK) {
          ESP_LOGE(TAG, "Failed to connect to peripheral " MACSTR, MAC2STR(cmd.mac));
          if (cmd.op_type == BLE_OP_PASSTHROUGH_CMD) {
            mqtt_manager_publish_command_ack(cmd.request_id, cmd.target_device_id, cmd.mac, "error", "BLE Connect failed");
          } else if (cmd.op_type == BLE_OP_DEVICE_CLAIM) {
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                     cmd.mac[0], cmd.mac[1], cmd.mac[2], cmd.mac[3], cmd.mac[4], cmd.mac[5]);
            mqtt_manager_publish_claim_ack(cmd.request_id, mac_str, NULL, NULL, NULL, 0, "error", "BLE Connect failed");
          } else if (cmd.op_type == BLE_OP_DEVICE_UNCLAIM) {
            mqtt_manager_publish_unclaim_ack(cmd.request_id, cmd.target_device_id, "error", "BLE Connect failed");
          } else if (cmd.op_type == BLE_OP_KEY_ROTATE) {
            mqtt_manager_publish_keyrot_ack(cmd.request_id, cmd.target_device_id, NULL, 0, "error", "BLE Connect failed");
          } else if (cmd.op_type == BLE_OP_SUB_OTA || cmd.op_type == BLE_OP_GET_FW_INFO) {
            mqtt_manager_publish_firmware_status(cmd.sub_ota.ota_request_id, cmd.sub_ota.firmware_target,
                                                 FW_STATE_FAILED, 0, BLE_STATUS_TIMEOUT, 0, 0);
          }
          continue;
        }
      }

      // Dispatch operation
      if (cmd.op_type == BLE_OP_AUTO_CONNECT) {
        subkey_entry_t sk;
        if (subkey_get_by_mac(peer->mac, &sk) == ESP_OK) {
          ble_execute_session_init(peer, &sk);
        }
      } else if (cmd.op_type == BLE_OP_PASSTHROUGH_CMD) {
        ble_execute_passthrough_cmd(peer, &cmd);
      } else if (cmd.op_type == BLE_OP_DEVICE_CLAIM) {
        ble_execute_device_claim(peer, &cmd);
      } else if (cmd.op_type == BLE_OP_DEVICE_UNCLAIM) {
        ble_execute_device_unclaim(peer, &cmd);
      } else if (cmd.op_type == BLE_OP_KEY_ROTATE) {
        ble_execute_key_rotate(peer, &cmd);
      } else if (cmd.op_type == BLE_OP_SUB_OTA) {
        ble_execute_sub_ota(peer, &cmd);
      } else if (cmd.op_type == BLE_OP_GET_FW_INFO) {
        ble_execute_get_fw_info(peer, &cmd);
      }

      // Keep connection open for telemetry (Do not disconnect!)
    } else {
      // Idle cycle: check if any claimed device in NVS can be auto-connected
      subkey_entry_t all_subkeys[SUBKEY_MAX_DEVICES];
      size_t count = 0;
      if (subkey_get_all(all_subkeys, SUBKEY_MAX_DEVICES, &count) == ESP_OK && count > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        for (size_t i = 0; i < count; i++) {
          peer_ctx_t *peer = peer_find_by_mac(all_subkeys[i].mac);
          if (peer == NULL) {
            peer = peer_alloc_slot(all_subkeys[i].mac, all_subkeys[i].addr_type);
          }
          if (peer != NULL && peer->state == PEER_STATE_DISCONNECTED && now_ms >= peer->next_reconnect_ms) {
            strncpy(peer->device_id, all_subkeys[i].device_id, sizeof(peer->device_id) - 1);
            if (ble_connect_to_peer(peer) == ESP_OK) {
              ble_execute_session_init(peer, &all_subkeys[i]);
            }
            break;
          }
        }
      }
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

  int rc = nimble_port_init();
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to init NimBLE port; rc=%d", rc);
    return ESP_FAIL;
  }

  // Initialize peer contexts and semaphores
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    memset(&s_peers[i], 0, sizeof(peer_ctx_t));
    s_peers[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_peers[i].state = PEER_STATE_DISCONNECTED;
    s_peers[i].reconnect_backoff_sec = 5;
    s_peers[i].op_sem = xSemaphoreCreateBinary();
    s_peers[i].write_sem = xSemaphoreCreateBinary();
    s_peers[i].notify_sem = xSemaphoreCreateBinary();
  }

  ble_hs_cfg.sync_cb = ble_on_sync;
  ble_hs_cfg.reset_cb = ble_on_reset;

  ble_server_init();

  ble_scan_queue = xQueueCreate(20, sizeof(ble_scan_report_t));
  ble_cmd_queue = xQueueCreate(10, sizeof(ble_lock_cmd_t));
  if (ble_scan_queue == NULL || ble_cmd_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create BLE queues");
    return ESP_ERR_NO_MEM;
  }

  xTaskCreate(ble_cmd_listener_task, "ble_cmd_task", BLE_CMD_TASK_STACK_SIZE,
              NULL, 5, NULL);
  nimble_port_freertos_init(ble_host_task);

  ESP_LOGI(TAG, "NimBLE Stack initialized successfully (max concurrent peers: %d)", BLE_MAX_PEERS);
  return ESP_OK;
}

esp_err_t ble_manager_start(void) {
  ESP_LOGI(TAG, "Starting BLE Manager scanning & services...");
  if (s_ble_synced) {
    ble_app_scan();
    ble_server_start_adv();
  }
  return ESP_OK;
}

esp_err_t ble_manager_stop(void) {
  ESP_LOGI(TAG, "Stopping BLE Manager...");
  ble_app_scan_cancel();
  ble_server_stop_adv();
  return ESP_OK;
}

bool ble_manager_is_scanning(void) { return s_scanning; }

void ble_manager_trigger_scan(void) {
  ESP_LOGI(TAG, "Triggering BLE Scan restart...");
  ble_app_scan_cancel();
  vTaskDelay(pdMS_TO_TICKS(100));
  ble_app_scan();
}
