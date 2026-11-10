#include "app_state_machine.h"
#include "api_client.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_io.h"
#include "hal_qr.h"
#include "hal_shift_reg.h"
#include "hal_touch.h"
#include "hal_wiegand.h"
#include "hw_config.h"
#include "mbedtls/sha256.h"
#include "mfrc522.h"
#include "nv_storage.h"
#include "osdp_pd.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *TAG = "APP_SM";

// Normalize QR string for NVS storage and local authentication:
// 1. If it's a dynamic QR with comma separator (e.g. "<id>,<date>,QR"), extract
// the <id> part.
// 2. If <= 31 chars, use as-is.
// 3. If > 31 chars without comma, map with SHA-256 (31 hex chars).
static void app_normalize_qr_string(const char *in_qr, char *out_qr,
                                    size_t max_len) {
  if (!in_qr || !out_qr || max_len == 0)
    return;

  // Check if this is a dynamic comma-separated QR (e.g. "31493017862,2026-11-04
  // 20:31:30.401,QR")
  const char *comma = strchr(in_qr, ',');
  if (comma != NULL) {
    size_t id_len = (size_t)(comma - in_qr);
    if (id_len > 0 && id_len < max_len && id_len <= 31) {
      strncpy(out_qr, in_qr, id_len);
      out_qr[id_len] = '\0';
      ESP_LOGI(TAG, "Dynamic QR normalized to ID prefix: '%s'", out_qr);
      return;
    }
  }

  size_t in_len = strlen(in_qr);
  if (in_len <= 31) {
    strncpy(out_qr, in_qr, max_len - 1);
    out_qr[max_len - 1] = '\0';
  } else {
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256
    mbedtls_sha256_update(&ctx, (const unsigned char *)in_qr, in_len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hex_buf[65];
    for (int i = 0; i < 32; i++) {
      snprintf(hex_buf + (i * 2), 3, "%02x", hash[i]);
    }
    strncpy(out_qr, hex_buf, 31);
    out_qr[31] = '\0';
    ESP_LOGI(TAG, "Long QR (>31 chars) mapped to SHA256-31hex: %s", out_qr);
  }
}
static app_state_t current_state = STATE_BOOTING;
static sys_config_t config;

static uint32_t app_normalize_rfid_card_id(uint32_t raw_card_id,
                                           uint8_t wg_bit_len) {
  if (wg_bit_len == 26 && config.reserved_0 != 26) {
    config.reserved_0 = 26;
    nv_storage_set_config(&config);
    ESP_LOGW(TAG, "Auto-detected 26-bit External Wiegand Reader! Switched RFID "
                  "format to 26-bit mode.");
  } else if (wg_bit_len == 34 && config.reserved_0 != 34) {
    config.reserved_0 = 34;
    nv_storage_set_config(&config);
    ESP_LOGW(TAG, "Auto-detected 34-bit External Wiegand Reader! Switched RFID "
                  "format to 34-bit mode.");
  }

  if (config.reserved_0 == 26) {
    return (raw_card_id & 0x00FFFFFF);
  }
  return raw_card_id;
}

static int64_t state_timer_start = 0;
static int64_t led_revert_timer_ms = 0;
static uint8_t s_failed_auth_count = 0;
static int64_t s_lockout_until_ms = 0;

static const char *state_to_string(app_state_t state) {
  switch (state) {
  case STATE_BOOTING:
    return "BOOTING";
  case STATE_IDLE:
    return "IDLE";
  case STATE_PROGRAMMING:
    return "PROGRAMMING";
  case STATE_FACTORY_RESET:
    return "FACTORY_RESET";
  case STATE_OTA_UPDATING:
    return "OTA_UPDATING";
  case STATE_DOOR_OPEN:
    return "DOOR_OPEN";
  case STATE_ALARM:
    return "ALARM";
  case STATE_HW_ERROR:
    return "HW_ERROR";
  case STATE_USER_PIN_CHANGE:
    return "USER_PIN_CHANGE";
  case STATE_MASTER_ADD_MODE:
    return "MASTER_ADD_MODE";
  case STATE_MASTER_DELETE_MODE:
    return "MASTER_DELETE_MODE";
  case STATE_ONLINE_CARD_ENROLL:
    return "ONLINE_CARD_ENROLL";
  default:
    return "UNKNOWN";
  }
}

typedef enum {
  PROG_STATE_ROOT,
  PROG_STATE_UPDATE_MASTER_CODE_1, // Menu 1
  PROG_STATE_UPDATE_MASTER_CODE_2, // Menu 1
  PROG_STATE_ADD_MASTER_CARD,      // Menu 2
  PROG_STATE_ADD_MASTER_SPECIFIC,  // Menu 2
  PROG_STATE_ADD_USER,             // Menu 3 (ROOT)
  PROG_STATE_ADD_USER_PIN_CONFIRM, // Menu 3 (PIN CONFIRM)
  PROG_STATE_DEL_USER,             // Menu 4
  PROG_STATE_DEL_ALL_CONFIRM,      // Menu 4 (WAIT FOR # AFTER MASTER CARD/QR)
  PROG_STATE_BLOCK_ADD_USER_ID,    // Menu 5
  PROG_STATE_BLOCK_ADD_QUANTITY,   // Menu 5
  PROG_STATE_BLOCK_ADD_CARD,       // Menu 5
  PROG_STATE_RELAY,                // Menu 6
  PROG_STATE_AUDIO_VISUAL,         // Menu 7
  PROG_STATE_DEL_MASTER_CARD,      // Menu 8 - Master Card / Master QR Silme
  PROG_STATE_FACTORY_RESET,        // Menu 9 - Factory Reset (99#)
  PROG_STATE_WIEGAND_FORMAT        // Menu 0 - Wiegand Format (0 26 # / 0 34 #)
} prog_substate_t;

typedef enum {
  PIN_CHANGE_OLD,
  PIN_CHANGE_NEW,
  PIN_CHANGE_REP
} user_pin_substate_t;
static user_pin_substate_t pin_change_state = PIN_CHANGE_OLD;
static user_record_t pin_change_user;
static char temp_new_pin[16];

static prog_substate_t prog_state = PROG_STATE_ROOT;
static char input_buffer[64];
static uint8_t input_len = 0;
static uint16_t pending_user_id = 0;
static uint16_t pending_quantity = 0;
static char temp_master_code[16];
static bool is_entering_master = false;
static bool relay_state = false;
// static char current_master_card[32]; // Track the master card used to enter
// Master Mode

void app_set_state(app_state_t new_state) {
  if (new_state != current_state) {
    ESP_LOGI(TAG, "State Transition: %s -> %s", state_to_string(current_state),
             state_to_string(new_state));
  }
  current_state = new_state;
  state_timer_start = esp_timer_get_time();
  led_revert_timer_ms = 0; // Clear temporary led override

  if (new_state == STATE_PROGRAMMING || new_state == STATE_MASTER_ADD_MODE ||
      new_state == STATE_FACTORY_RESET) {
    s_failed_auth_count = 0;
    s_lockout_until_ms = 0;
  }

  switch (new_state) {
  case STATE_IDLE:
    relay_state = false;
    hal_io_relay_set(false);
    extern bool has_hw_warning;
    if (has_hw_warning) {
      hal_io_led_set(LED_COLOR_ORANGE, LED_MODE_BLINK_FAST);
    } else {
      hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_SLOW);
    }
    input_len = 0;
    is_entering_master = false;
    break;
  case STATE_PROGRAMMING:
    hal_io_led_set(LED_COLOR_BLUE, LED_MODE_BLINK_FAST);
    prog_state = PROG_STATE_ROOT;
    input_len = 0;
    pending_user_id = 0;
    hal_io_buzzer_beep(2000, 200, 1);
    break;
  case STATE_FACTORY_RESET:
    hal_io_led_set(LED_COLOR_ORANGE, LED_MODE_SOLID);
    hal_shift_reg_play_anim(ANIM_FACTORY_RESET, 0);
    hal_io_buzzer_beep(2700, 1000, 1);
    if (config.is_online) {
      api_client_send_local_factory_reset_event();
    }
    break;
  case STATE_OTA_UPDATING:
    hal_io_buzzer_beep(2700, 200, 2);
    break;
  case STATE_DOOR_OPEN:
    relay_state = true;
    hal_io_led_set(LED_COLOR_GREEN, LED_MODE_BLINK_SLOW);
    hal_io_relay_set(true);
    hal_io_buzzer_beep(2700, 100, 1);
    break;
  case STATE_ALARM:
    hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_FAST);
    break;
  case STATE_HW_ERROR:
    hal_io_led_set(LED_COLOR_YELLOW, LED_MODE_BLINK_FAST);
    hal_io_buzzer_beep(1000, 300, 3);
    break;
  case STATE_USER_PIN_CHANGE:
    hal_io_led_set(LED_COLOR_YELLOW, LED_MODE_BLINK_FAST);
    pin_change_state = PIN_CHANGE_OLD;
    input_len = 0;
    memset(input_buffer, 0, sizeof(input_buffer));
    break;
  case STATE_ONLINE_CARD_ENROLL:
    hal_io_led_set(LED_COLOR_YELLOW, LED_MODE_BLINK_FAST);
    hal_io_buzzer_beep(2700, 100, 2);
    break;
  case STATE_MASTER_ADD_MODE:
    hal_io_led_set(LED_COLOR_BLUE, LED_MODE_BLINK_SLOW);
    hal_io_buzzer_beep(2700, 100, 2); // Enter add mode
    input_len = 0;
    pending_user_id = 0;
    break;
  case STATE_MASTER_DELETE_MODE:
    hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_SLOW);
    hal_io_buzzer_beep(2700, 100, 3); // Enter delete mode
    input_len = 0;
    break;
  default:
    break;
  }
}

void app_state_machine_init(void) {
  nv_storage_get_config(&config);
  hal_io_set_sound_enabled(config.sound_enabled);

  // Play startup sweep sound
  hal_io_buzzer_intro_sweep();

  if (current_state != STATE_HW_ERROR) {
    app_set_state(STATE_IDLE);
  }
}

void app_trigger_door_open(void) {
  s_failed_auth_count = 0;
  s_lockout_until_ms = 0;
  uint32_t dur = config.relay_time > 0 ? (config.relay_time * 1000) : 3000;
  hal_shift_reg_play_anim(ANIM_SUCCESS_TICK, dur);
  if (config.relay_time == 0) {
    relay_state = !relay_state;
    if (relay_state) {
      app_set_state(STATE_DOOR_OPEN);
      if (config.is_online)
        api_client_send_lock_opened_event();
    } else {
      hal_io_relay_set(false);
      app_set_state(STATE_IDLE);
    }
  } else {
    relay_state = true;
    app_set_state(STATE_DOOR_OPEN);
    if (config.is_online)
      api_client_send_lock_opened_event();
  }
}

void app_trigger_door_close(void) {
  if (config.relay_time == 0) {
    relay_state = false;
    hal_io_relay_set(false);
    app_set_state(STATE_IDLE);
    ESP_LOGI(TAG, "Door remotely closed (Toggle mode).");
  } else {
    ESP_LOGW(TAG, "Ignored REMOTE_CLOSE because device is not in toggle mode.");
  }
}

void app_trigger_alarm(void) { app_set_state(STATE_ALARM); }

static void app_set_temp_led(led_color_t color, led_mode_t mode,
                             uint32_t duration_ms) {
  hal_io_led_set(color, mode);
  led_revert_timer_ms = (esp_timer_get_time() / 1000) + duration_ms;
}

void app_trigger_access_denied(void) {
  if (current_state == STATE_IDLE) {
    s_failed_auth_count++;
    if (s_failed_auth_count >= 5) {
      s_lockout_until_ms = (esp_timer_get_time() / 1000) + 30000;
      s_failed_auth_count = 0;
      ESP_LOGW(TAG, "Brute-force protection: 5 failed attempts! Locking inputs for 30 seconds.");
      app_set_temp_led(LED_COLOR_RED, LED_MODE_BLINK_FAST, 30000);
      hal_io_buzzer_beep(1000, 1000, 1);
      return;
    }
  }
  hal_io_buzzer_beep(1000, 300, 3); // 3 Slow Beeps
  hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
}

static bool app_is_access_granted(const char *prefix) {
  if (config.access_mode == 6)
    return true; // Any
  if (config.access_mode == 0 && strcmp(prefix, "RFID") == 0)
    return true; // Card Only
  if (config.access_mode == 1 && strcmp(prefix, "KEYPAD") == 0)
    return true; // PIN Only
  if (config.access_mode == 2 && strcmp(prefix, "QR") == 0)
    return true; // QR Only
  if (config.access_mode == 3 &&
      (strcmp(prefix, "RFID") == 0 || strcmp(prefix, "KEYPAD") == 0))
    return true; // Card or PIN
  if (config.access_mode == 4 &&
      (strcmp(prefix, "RFID") == 0 || strcmp(prefix, "QR") == 0))
    return true; // Card or QR
  if (config.access_mode == 5 &&
      (strcmp(prefix, "KEYPAD") == 0 || strcmp(prefix, "QR") == 0))
    return true; // PIN or QR
  return false;
}

static void process_auth_pass_command(const char *prefix, const char *data) {
  char payload[128];
  snprintf(payload, sizeof(payload), "%s:%s", prefix, data);

  // Notify OSDP ACU
  if (strcmp(prefix, "RFID") == 0) {
    osdp_pd_notify_card(data);
  } else if (strcmp(prefix, "QR") == 0) {
    osdp_pd_notify_qr(data);
  } else if (strcmp(prefix, "KEYPAD") == 0) {
    osdp_pd_notify_pin(data);
  }

  if (!app_is_access_granted(prefix)) {
    ESP_LOGW(TAG, "Access Mode %d Denied Access for %s", config.access_mode,
             payload);
    app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
    hal_io_buzzer_beep(1000, 300, 3);
    hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
    return;
  }

  if (config.is_online) {
    if (wifi_manager_is_connected()) {
      if (strncmp(prefix, "RFID", 4) == 0) {
        uint32_t decimal_val = (uint32_t)strtoul(data, NULL, 10);
        ESP_LOGI(TAG, "Online Mode Auth: %s (Hex: %08lX)", payload,
                 (unsigned long)decimal_val);
      } else {
        ESP_LOGI(TAG, "Online Mode Auth: %s", payload);
      }
      // Send the prefixed payload (e.g., QR:K3LI8T) so api_client can parse it
      int code = api_client_send_pass_event(payload);
      if (code == 200) {
        ESP_LOGI(TAG, "Online Event Sent! Waiting for server...");
        // Note: Do NOT open the door or beep success here. The server will
        // approve and door open will beep.
      } else {
        ESP_LOGW(TAG, "Failed to send MQTT Event! Code: %d", code);
        app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
        hal_io_buzzer_beep(1000, 300, 3); // 3 Slow Beeps
        hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
      }
    } else {
      ESP_LOGW(TAG, "Online Mode but WiFi is disconnected! Strict Deny.");
      app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
      hal_io_buzzer_beep(1000, 300, 3);
      hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
    }
    return; // Strict: Do NOT fallback to local DB if online
  }

  // Offline validation
  user_record_t u;
  bool found = false;
  if (strcmp(prefix, "KEYPAD") == 0)
    found = nv_storage_find_user_by_pin(data, &u);
  else if (strcmp(prefix, "RFID") == 0)
    found = nv_storage_find_user_by_card(data, &u) ||
            nv_storage_find_user_by_qr(data, &u);
  else if (strcmp(prefix, "QR") == 0) {
    char norm_qr[32];
    app_normalize_qr_string(data, norm_qr, sizeof(norm_qr));
    found = nv_storage_find_user_by_qr(norm_qr, &u) ||
            nv_storage_find_user_by_card(norm_qr, &u);
    if (!found && strchr(data, ',') != NULL) {
      char cred_part[32] = {0};
      const char *comma = strchr(data, ',');
      size_t cred_len = (size_t)(comma - data);
      if (cred_len >= sizeof(cred_part))
        cred_len = sizeof(cred_part) - 1;
      strncpy(cred_part, data, cred_len);
      cred_part[cred_len] = '\0';

      found = nv_storage_find_user_by_qr(cred_part, &u) ||
              nv_storage_find_user_by_card(cred_part, &u);
      if (found) {
        ESP_LOGI(TAG, "Offline Dynamic QR Matched by ID: '%s' (UserID: %d)",
                 cred_part, u.user_id);
      }
    }
  }

  if (found) {
    ESP_LOGI(TAG, "Offline Mode Granted Access for %s (UserID: %d)", payload,
             u.user_id);
    app_trigger_door_open();
  } else {
    ESP_LOGW(TAG, "Offline Mode Denied Access for %s", payload);
    app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
    app_trigger_access_denied();
  }

  offline_log_t log = {0};
  log.timestamp = (uint64_t)(esp_timer_get_time() /
                             1000ULL); // Time since boot, ideal would be real
                                       // time but device might not have it
  if (strcmp(prefix, "KEYPAD") == 0)
    log.accessMethod = 0x01;
  else if (strcmp(prefix, "RFID") == 0)
    log.accessMethod = 0x02;
  else if (strcmp(prefix, "QR") == 0)
    log.accessMethod = 0x04;
  else
    log.accessMethod = 0x00;

  log.localId = found ? u.user_id : 0;
  log.result = found ? 0x00 : 0x01;
  nv_storage_add_offline_log(log);
}

void app_state_machine_tick(void) {
  static char s_raw_qr_buf[128];
  hal_io_tick();
  hal_shift_reg_tick();
  int64_t now_us = esp_timer_get_time();
  int64_t elapsed_ms = (now_us - state_timer_start) / 1000;

  if (led_revert_timer_ms > 0 && now_us / 1000 >= led_revert_timer_ms) {
    led_revert_timer_ms = 0;
    if (current_state == STATE_IDLE) {
      extern bool has_hw_warning;
      if (has_hw_warning) {
        hal_io_led_set(LED_COLOR_ORANGE, LED_MODE_BLINK_FAST);
      } else {
        hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_SLOW);
      }
    } else if (current_state == STATE_DOOR_OPEN)
      hal_io_led_set(LED_COLOR_GREEN, LED_MODE_BLINK_SLOW);
    else if (current_state == STATE_PROGRAMMING)
      hal_io_led_set(LED_COLOR_BLUE, LED_MODE_BLINK_FAST);
    else if (current_state == STATE_ALARM)
      hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_FAST);
    else if (current_state == STATE_HW_ERROR)
      hal_io_led_set(LED_COLOR_YELLOW, LED_MODE_BLINK_FAST);
    else if (current_state == STATE_MASTER_ADD_MODE)
      hal_io_led_set(LED_COLOR_BLUE, LED_MODE_BLINK_SLOW);
    else if (current_state == STATE_MASTER_DELETE_MODE)
      hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_SLOW);
    else if (current_state == STATE_USER_PIN_CHANGE)
      hal_io_led_set(LED_COLOR_YELLOW, LED_MODE_BLINK_FAST);
  }

  // [V2.0.6 log cleanup] Periodic heartbeat suppressed
  /*
  static int64_t last_heartbeat = 0;
  if (now_us - last_heartbeat > 25000000) {
    ESP_LOGI(TAG, "Heartbeat - System is alive. Current State: %s, Online: %d",
             state_to_string(current_state), config.is_online);
    last_heartbeat = now_us;
  }
  */

  static int64_t last_anim_tick = 0;
  if (current_state == STATE_IDLE && now_us - last_anim_tick > 3000000) {
    // Only play the idle animation when the 20-second keypad activity window
    // has expired (i.e., no key has been pressed recently).
    if (!hal_shift_reg_is_keypad_active()) {
      if (active_hw_version == HW_VERSION_PIN_QR) {
        hal_shift_reg_play_anim(ANIM_IDLE_SNAKE, '\0');
      } else if (active_hw_version == HW_VERSION_RFID_ONLY) {
        hal_shift_reg_play_anim(ANIM_IDLE_PING_PONG, '\0');
      } else {
        hal_shift_reg_play_anim(ANIM_IDLE_WAVE, '\0');
      }
    }
    last_anim_tick = now_us;
  }

  // Rule: Auto-switch to Online permanently when connected to Wi-Fi
  if (wifi_manager_is_connected() && !config.is_online) {
    config.is_online = true;
    nv_storage_set_config(&config);
    nv_storage_delete_all_users(); // Wipe local users when switching to Online
    nv_storage_clear_offline_logs(
        255); // Wipe all offline logs when switching to Online
    ESP_LOGI(TAG, "WiFi connected! Switched permanently to Online Mode. Local "
                  "users and offline logs wiped.");
    if (current_state == STATE_IDLE) {
      hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_SLOW);
    }
  }

  // Track boot 10s master enrollment window closing for RFID_ONLY
  static bool master_enroll_closed = false;
  int64_t uptime_ms = now_us / 1000;
  if (!master_enroll_closed && uptime_ms > 10000) {
    master_enroll_closed = true;
    if (active_hw_version == HW_VERSION_RFID_ONLY &&
        nv_storage_is_master_enroll_needed()) {
      if (config.master_card_count > 0) {
        nv_storage_set_master_enroll_done();
        ESP_LOGI(TAG,
                 "[BOOT_MASTER] 10s window closed with %d Master Cards. "
                 "Enrollment locked.",
                 config.master_card_count);
      } else {
        ESP_LOGI(TAG, "[BOOT_MASTER] 10s window closed with 0 Master Cards. "
                      "Remains in fresh enroll state for next boot.");
      }
    }
  }

  // Hardware Factory Reset via IN2 (Fail-Safe Anti-Short 12V Handshake):
  // -------------------------------------------------------------------------
  // State 1 (WAIT_HIGH): Within first 10s window (uptime_ms <= 12000), IN2 == 1
  // starts Step 2 immediately. Step 2  (HOLD_HIGH_5S): IN2 must remain
  // continuously HIGH (12V) for 5000 ms. Step 3  (WAIT_FALLING_3S): 600ms beep
  // + yellow LED; user must disconnect 12V within 3000 ms (short-circuit
  // protection). Step 4  (CONFIRM_LOW_3S): 3 warning beeps (450ms x 3); IN2
  // must remain LOW for 3000 ms continuously. Finish: Trigger
  // app_set_state(STATE_FACTORY_RESET).
  typedef enum {
    IN2_RST_WAIT_HIGH = 0,
    IN2_RST_HOLD_HIGH_5S,
    IN2_RST_WAIT_FALLING_3S,
    IN2_RST_CONFIRM_LOW_3S,
    IN2_RST_DONE
  } in2_reset_state_t;

  static in2_reset_state_t in2_rst_state = IN2_RST_WAIT_HIGH;
  static int64_t in2_state_start_ms = 0;
  static int64_t in2_last_periodic_log_ms = 0;

  // Debounce filter for IN2 (filters out mechanical contact bounce < 40ms)
  static int in2_debounced = 0;
  static int in2_stable_count = 0;
  static int in2_last_raw = -1;
  static bool in2_init_done = false;

  int in2_raw = hal_io_read_in2_raw();
  if (!in2_init_done) {
    in2_debounced = in2_raw;
    in2_last_raw = in2_raw;
    in2_stable_count = 4;
    in2_init_done = true;
    ESP_LOGI(TAG,
             "[IN2_RESET] Hardware Reset Monitor STARTED at uptime=%lld ms "
             "(Raw IN2=%d)",
             (long long)uptime_ms, in2_raw);
  } else {
    if (in2_raw == in2_last_raw) {
      if (in2_stable_count < 4) {
        in2_stable_count++;
        if (in2_stable_count >= 4 && in2_debounced != in2_raw) {
          in2_debounced = in2_raw;
          ESP_LOGW(TAG,
                   "[IN2_RESET] >>> IN2 PIN CHANGED: %s (%d) at uptime=%lld ms "
                   "(State: %d) <<<",
                   in2_debounced ? "HIGH (12V)" : "LOW (0V)", in2_debounced,
                   (long long)uptime_ms, (int)in2_rst_state);
        }
      }
    } else {
      in2_stable_count = 0;
      in2_last_raw = in2_raw;
    }
  }

  bool periodic_log = (uptime_ms - in2_last_periodic_log_ms >= 1000);
  if (periodic_log && in2_rst_state != IN2_RST_DONE) {
    in2_last_periodic_log_ms = uptime_ms;
  }

  if (in2_rst_state != IN2_RST_DONE) {
    switch (in2_rst_state) {
    case IN2_RST_WAIT_HIGH:
      if (uptime_ms > 12000) {
        in2_rst_state = IN2_RST_DONE;
        ESP_LOGW(TAG,
                 "[IN2_RESET] [DONE] 10s boot window EXPIRED (uptime=%lld ms). "
                 "IN2 reset monitor is now CLOSED.",
                 (long long)uptime_ms);
      } else if (in2_debounced == 1 || in2_raw == 1) {
        in2_debounced = 1;
        in2_rst_state = IN2_RST_HOLD_HIGH_5S;
        in2_state_start_ms = uptime_ms;
        ESP_LOGW(TAG,
                 "[IN2_RESET] IN2 is HIGH (12V)! Starting 5-second hold "
                 "timer... (uptime=%lld ms)",
                 (long long)uptime_ms);
      } else if (periodic_log) {
        ESP_LOGI(TAG,
                 "[IN2_RESET] [WAIT_HIGH] Boot window OPEN: %lld ms remaining "
                 "(uptime=%lld/12000 ms). Waiting for 12V (IN2=%d)...",
                 (long long)(12000 - uptime_ms), (long long)uptime_ms,
                 in2_debounced);
      }
      break;

    case IN2_RST_HOLD_HIGH_5S:
      if (in2_debounced == 0) {
        int64_t held_ms = uptime_ms - in2_state_start_ms;
        if (uptime_ms <= 12000) {
          in2_rst_state = IN2_RST_WAIT_HIGH;
          ESP_LOGW(TAG,
                   "[IN2_RESET] 12V released early (held %lld ms). Window "
                   "still open (%lld ms left). Waiting for 12V...",
                   (long long)held_ms, (long long)(12000 - uptime_ms));
        } else {
          in2_rst_state = IN2_RST_DONE;
          ESP_LOGW(TAG,
                   "[IN2_RESET] 12V released early (held %lld ms) and 10s boot "
                   "window expired. Reset aborted.",
                   (long long)held_ms);
        }
      } else if (uptime_ms - in2_state_start_ms >= 5000) {
        in2_rst_state = IN2_RST_WAIT_FALLING_3S;
        in2_state_start_ms = uptime_ms;
        ESP_LOGW(TAG,
                 "[IN2_RESET] 5s HIGH completed! Disconnect 12V (set IN2 LOW) "
                 "within 6 seconds... (uptime=%lld ms)",
                 (long long)uptime_ms);
        hal_io_set_sound_enabled(true);
        hal_io_buzzer_beep(2700, 600, 1);
        hal_io_led_set(LED_COLOR_YELLOW, LED_MODE_BLINK_FAST);
      } else if (periodic_log) {
        ESP_LOGW(TAG,
                 "[IN2_RESET] [HOLD_HIGH_5S] 12V Progress: %lld / 5000 ms... "
                 "KEEP HOLDING 12V! (IN2=%d)",
                 (long long)(uptime_ms - in2_state_start_ms), in2_debounced);
      }
      break;

    case IN2_RST_WAIT_FALLING_3S:
      if (in2_debounced == 0) {
        in2_rst_state = IN2_RST_CONFIRM_LOW_3S;
        in2_state_start_ms = uptime_ms;
        ESP_LOGW(TAG, "[IN2_RESET] IN2 disconnected (LOW)! Hold LOW for 3 "
                      "seconds (3 beeps) to confirm Factory Reset...");
        hal_io_set_sound_enabled(true);
        hal_io_buzzer_beep(2700, 450, 3);
      } else if (uptime_ms - in2_state_start_ms >= 6000) {
        in2_rst_state = IN2_RST_DONE;
        ESP_LOGE(TAG, "[IN2_RESET] Timeout! IN2 was NOT disconnected within 6s "
                      "(short-circuit protection). Factory Reset ABORTED.");
        hal_io_set_sound_enabled(true);
        hal_io_buzzer_beep(1000, 200, 2);
        hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_SLOW);
      } else if (periodic_log) {
        ESP_LOGW(TAG,
                 "[IN2_RESET] [WAIT_FALLING_6S] Time left to release 12V: %lld "
                 "/ 6000 ms... RELEASE 12V NOW! (IN2=%d)",
                 (long long)(6000 - (uptime_ms - in2_state_start_ms)),
                 in2_debounced);
      }
      break;

    case IN2_RST_CONFIRM_LOW_3S:
      if (in2_debounced == 1) {
        in2_rst_state = IN2_RST_DONE;
        ESP_LOGE(TAG, "[IN2_RESET] IN2 re-asserted HIGH during confirmation! "
                      "Factory Reset ABORTED.");
        hal_io_set_sound_enabled(true);
        hal_io_buzzer_beep(1000, 200, 2);
        hal_io_led_set(LED_COLOR_RED, LED_MODE_BLINK_SLOW);
      } else if (uptime_ms - in2_state_start_ms >= 3000) {
        in2_rst_state = IN2_RST_DONE;
        ESP_LOGW(TAG,
                 "[IN2_RESET] 3s LOW confirmed! Triggering Factory Reset...");
        app_set_state(STATE_FACTORY_RESET);
      } else if (periodic_log) {
        ESP_LOGW(TAG,
                 "[IN2_RESET] [CONFIRM_LOW_3S] Confirming LOW: %lld / 3000 "
                 "ms... KEEP DISCONNECTED! (IN2=%d)",
                 (long long)(uptime_ms - in2_state_start_ms), in2_debounced);
      }
      break;

    case IN2_RST_DONE:
    default:
      break;
    }
  }

  // Dig_In Hardware Handlers
  if (hal_io_read_exit_button()) { // Exit Button
    if (config.is_online) {
      api_client_send_session_keepalive();
    }
    if (current_state == STATE_IDLE) {
      ESP_LOGI(TAG, "Exit Button Pressed - Opening Door");
      app_trigger_door_open();
    } else if (current_state == STATE_DOOR_OPEN && config.relay_time == 0) {
      ESP_LOGI(TAG, "Exit Button Pressed - Closing Door (Toggle mode)");
      relay_state = false;
      hal_io_relay_set(false);
      app_set_state(STATE_IDLE);
      hal_io_buzzer_beep(2700, 100, 2);
    }
  }

  switch (current_state) {
  /*
   * FACTORY RESET
   * -------------
   * Triggered by entering '0' five times on the keypad within
   * the first 10 seconds after power-on, or via Menu 9 (99#).
   *
   * What gets erased:
   *   - All registered users (cards, PINs, QR codes)
   *   - All master card credentials
   *   - Wi-Fi SSID / password
   *
   * What gets restored to factory defaults:
   *   - Master code       : "123456"
   *   - Relay time        : 5 seconds
   *   - Access mode       : Card + PIN + QR (all allowed)
   *   - Online mode       : disabled
   *   - Wiegand format    : 34-bit
   *
   * The device plays the ANIM_FACTORY_RESET animation for 3 seconds
   * before wiping data, then performs a hard restart.
   */
  case STATE_FACTORY_RESET: {
    if (elapsed_ms >= 3000) { // Wait for animation to finish
      nv_storage_delete_all_users();
      nv_storage_clear_offline_logs(255);
      nv_storage_clear_master_credentials();
      nv_storage_clear_master_enroll_done();
      wifi_manager_clear_credentials();
      nv_storage_clear_onloi_mqtt_config();
      nv_storage_clear_active_ota_job();

      memset(&config, 0, sizeof(config));
      strcpy(config.master_code, "123456");
      config.relay_time = 5;
      config.access_mode = 6;
      config.alarm_time = 1;
      config.sound_enabled = true;
      config.led_always_on = true;
      config.reserved_mode = 0;
      config.is_online = false;
      config.reserved_door_det = 0;
      config.reserved_0 = 26;
      config.reserved_1 = 4;
      config.reserved_2 = false;
      nv_storage_set_config(&config);

      ESP_LOGW(TAG, "Wipe complete. Restarting system...");
      esp_restart();
    }
    break;
  }
  case STATE_IDLE: {
    if ((esp_timer_get_time() / 1000) < s_lockout_until_ms) {
      char discard_key = '\0';
      uint32_t discard_uid = 0;
      bool input_attempted = false;

      if (hal_touch_read_key(&discard_key)) {
        input_attempted = true;
      }
      if (active_hw_version != HW_VERSION_PIN_QR) {
        if (mfrc522_check_card(&discard_uid)) {
          input_attempted = true;
        }
      }
      if (hal_wiegand_available()) {
        uint8_t discard_bits;
        hal_wiegand_read(&discard_bits);
        input_attempted = true;
      }
      if (active_hw_version == HW_VERSION_PIN_QR) {
        if (hal_qr_read(s_raw_qr_buf, sizeof(s_raw_qr_buf))) {
          input_attempted = true;
        }
      }

      if (input_attempted) {
        input_len = 0;
        is_entering_master = false;
        hal_io_buzzer_beep(1000, 150, 2);
      }
      break;
    }

    if ((input_len > 0 || is_entering_master) && elapsed_ms > 5000) {
      ESP_LOGI(TAG, "[IDLE] Keypad timeout, resetting buffer.");
      input_len = 0;
      is_entering_master = false;
    }

    char key = '\0';
    if (hal_touch_read_key(&key)) {
      if (config.is_online) {
        api_client_send_session_keepalive();
      }
      state_timer_start = esp_timer_get_time(); // Reset timeout on keypress
      if (hal_shift_reg_is_blocking_anim_playing())
        break;
      // If this touch just woke the device from screensaver mode,
      // discard it entirely — it's a wakeup event, not user input.
      if (hal_shift_reg_consume_wakeup_touch()) {
        ESP_LOGI(
            TAG,
            "[IDLE] Screensaver wakeup touch consumed (key='%c' discarded).",
            key);
        if (config.is_online) {
          api_client_send_session_start(0x01);
        }
        break;
      }

      ESP_LOGI(TAG, "[IDLE] Key pressed: %c", key);
      if (config.is_online) {
        api_client_send_session_start(0x01);
      }
      osdp_pd_notify_key(key);

      if (key == '*') {
        is_entering_master = true;
        input_len = 0;
        hal_io_buzzer_beep(2700, 50, 1);
      } else if (key == '#') {
        hal_io_buzzer_beep(2700, 50, 1);
        input_buffer[input_len] = '\0';
        if (is_entering_master) {
          if (strcmp(input_buffer, config.master_code) == 0 ||
              (strlen(config.master_code) == 0 &&
               strcmp(input_buffer, "123456") == 0)) {
            hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
            hal_io_buzzer_beep(2700, 100, 2); // 2 Rapid Beeps
            app_set_state(STATE_PROGRAMMING);
          } else {
            uint16_t uid = atoi(input_buffer);
            if (config.is_online) {
              ESP_LOGW(TAG, "PIN Change Rejected: Device is in Online Mode");
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
              hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
            } else if (nv_storage_get_user(uid, &pin_change_user)) {
              app_set_state(STATE_USER_PIN_CHANGE);
            } else {
              ESP_LOGW(
                  TAG,
                  "Master code incorrect, or user not found for PIN change!");
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3); // 3 Slow Beeps
              hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
            }
          }
          is_entering_master = false;
          input_len = 0;
        } else {
          if (input_len >= 4) {
            process_auth_pass_command("KEYPAD", input_buffer);
          } else if (input_len > 0) {
            ESP_LOGW(TAG,
                     "PIN too short (%d digits), minimum is 4. Discarding.",
                     input_len);
          }
          input_len = 0;
        }
      } else {
        if (input_len < 62) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      }
    }

    uint32_t rfid_uid = 0;
    bool rfid_ok = false;
    if (active_hw_version != HW_VERSION_PIN_QR) {
      rfid_ok = mfrc522_check_card(&rfid_uid);
    }
    bool weigand_ok = hal_wiegand_available();

    if (weigand_ok) {
      uint16_t wg_bits = hal_wiegand_peek_bit_count();
      if (hal_shift_reg_is_blocking_anim_playing()) {
        hal_wiegand_flush();
        weigand_ok = false;
        break;
      }

      // Any payload > 64 bits (e.g. 304-bit Wiegand QR) is ALWAYS processed as
      // QR, never as RFID
      if (wg_bits > 64 || (wg_bits >= 48 && (wg_bits % 8 == 0) &&
                           hal_wiegand_is_printable_string())) {
        hal_shift_reg_wakeup_keypad();
        if (config.is_online) {
          api_client_send_session_start(0x01);
        }
        char wg_qr_str[64];
        uint16_t wg_qr_len = 0;
        if (hal_wiegand_read_string(wg_qr_str, sizeof(wg_qr_str), &wg_qr_len)) {
          ESP_LOGI(TAG, "Wiegand QR Received: %s (Bits: %d)", wg_qr_str,
                   wg_qr_len);
          process_auth_pass_command("QR", wg_qr_str);
        } else {
          hal_wiegand_flush();
        }
        weigand_ok = false;
      }
    }

    if (rfid_ok || weigand_ok) {
      if (hal_shift_reg_is_blocking_anim_playing()) {
        if (weigand_ok)
          hal_wiegand_flush();
        break;
      }
      hal_shift_reg_wakeup_keypad();
      if (config.is_online) {
        api_client_send_session_start(0x01);
      }
      uint8_t bit_len = 0;
      uint32_t card_id =
          (rfid_uid != 0) ? rfid_uid : hal_wiegand_read(&bit_len);
      card_id =
          app_normalize_rfid_card_id(card_id, (rfid_uid == 0) ? bit_len : 0);
      if (rfid_uid == 0) {
        ESP_LOGI(TAG,
                 "[IDLE] Card scanned via External Wiegand Reader: CardID=%lu "
                 "(Bits=%d)",
                 (unsigned long)card_id, bit_len);
      } else {
        ESP_LOGI(TAG, "[IDLE] Card scanned via Internal MFRC522: UID=%lu",
                 (unsigned long)card_id);
      }

      if (rfid_uid == 0 && bit_len == 34) {
        user_record_t tmp_usr;
        char master_str[24];
        if (nv_storage_find_user_by_wiegand34(card_id, &tmp_usr)) {
          uint32_t db_id = (uint32_t)strtoul(tmp_usr.card_id, NULL, 10);
          ESP_LOGI(TAG,
                   "Wiegand 34 Smart Match: Replaced Wiegand ID %lu with "
                   "Registered User ID %lu",
                   (unsigned long)card_id, (unsigned long)db_id);
          card_id = db_id;
        } else if (nv_storage_find_master_by_wiegand34(card_id, master_str)) {
          uint32_t db_id = (uint32_t)strtoul(master_str, NULL, 10);
          ESP_LOGI(TAG,
                   "Wiegand 34 Smart Match: Replaced Wiegand ID %lu with "
                   "Master ID %lu",
                   (unsigned long)card_id, (unsigned long)db_id);
          card_id = db_id;
        }
      }

      char card_str[24];
      snprintf(card_str, sizeof(card_str), "%lu", (unsigned long)card_id);

      // -------------------------------------------------------------
      // RFID_ONLY: Boot Master Card Enrollment (First 10s on fresh/reset
      // device)
      // -------------------------------------------------------------
      if (active_hw_version == HW_VERSION_RFID_ONLY && rfid_uid != 0 &&
          nv_storage_is_master_enroll_needed() && (now_us / 1000 <= 10000)) {
        if (nv_storage_is_master_credential(card_str)) {
          ESP_LOGI(TAG, "[BOOT_MASTER] Card %s is ALREADY a Master Card.",
                   card_str);
          hal_io_buzzer_beep(2700, 100, 1);
        } else {
          user_record_t u_chk;
          if (nv_storage_find_user_by_card(card_str, &u_chk)) {
            ESP_LOGW(
                TAG,
                "[BOOT_MASTER] Card %s belongs to User %d (cannot be Master)!",
                card_str, u_chk.user_id);
            app_trigger_access_denied();
          } else if (config.master_card_count >= 10) {
            ESP_LOGW(
                TAG,
                "[BOOT_MASTER] Limit reached (10 cards max)! Denied card: %s",
                card_str);
            app_trigger_access_denied();
          } else {
            if (nv_storage_add_master_credential(0, card_str)) {
              nv_storage_get_config(&config);
              ESP_LOGI(TAG, "[BOOT_MASTER] Master Card %d/10 ADDED: %s",
                       config.master_card_count, card_str);
              hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              app_trigger_access_denied();
            }
          }
        }
        break; // Do not process as normal access during boot enroll window
      }

      if (is_entering_master && nv_storage_is_master_credential(card_str)) {
        ESP_LOGI(TAG, "Master Card read after *! Entering Programming Mode.");
        hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
        app_set_state(STATE_PROGRAMMING);
        is_entering_master = false;
      } else {
        if (nv_storage_is_master_credential(card_str)) {
          int32_t master_id = nv_storage_get_master_id(card_str);
          ESP_LOGI(
              TAG,
              "Master Card (ID: %d) scanned in IDLE. Entering Master Add Mode.",
              (int)master_id);
          hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
          app_set_state(STATE_MASTER_ADD_MODE);
        } else {
          process_auth_pass_command("RFID", card_str);
        }
        is_entering_master = false;
      }
    }

    if (active_hw_version == HW_VERSION_PIN_QR &&
        hal_qr_read(s_raw_qr_buf, sizeof(s_raw_qr_buf))) {
      if (hal_shift_reg_is_blocking_anim_playing())
        break;
      hal_shift_reg_wakeup_keypad();
      if (config.is_online) {
        api_client_send_session_start(0x01);
      }

      char qr_str[32];
      app_normalize_qr_string(s_raw_qr_buf, qr_str, sizeof(qr_str));

      if (is_entering_master && nv_storage_is_master_credential(qr_str)) {
        ESP_LOGI(TAG, "Master QR read after *! Entering Programming Mode.");
        hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
        app_set_state(STATE_PROGRAMMING);
        is_entering_master = false;
      } else {
        if (nv_storage_is_master_credential(qr_str)) {
          int32_t master_id = nv_storage_get_master_id(qr_str);
          ESP_LOGI(
              TAG,
              "Master QR (ID: %d) scanned in IDLE. Entering Master Add Mode.",
              (int)master_id);
          hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
          app_set_state(STATE_MASTER_ADD_MODE);
        } else {
          process_auth_pass_command("QR", s_raw_qr_buf);
        }
        is_entering_master = false;
      }
    }
    break;
  }
  case STATE_USER_PIN_CHANGE: {
    if (elapsed_ms >= 15000) {
      app_set_state(STATE_IDLE);
      hal_io_buzzer_beep(1000, 500, 3);
      break;
    }
    char key = '\0';
    if (hal_touch_read_key(&key)) {
      if (hal_shift_reg_is_blocking_anim_playing())
        break;
      ESP_LOGI(TAG, "[PIN_CHANGE] Key pressed: %c", key);
      state_timer_start = esp_timer_get_time();

      if (key == '*') {
        hal_io_buzzer_beep(1000, 100, 1);
        app_set_state(STATE_IDLE);
      } else if (key == '#') {
        input_buffer[input_len] = '\0';
        if (pin_change_state == PIN_CHANGE_OLD) {
          if (strcmp(input_buffer, pin_change_user.pin) == 0) {
            pin_change_state = PIN_CHANGE_NEW;
            input_len = 0;
            hal_io_buzzer_beep(2700, 100, 1);
          } else {
            hal_io_buzzer_beep(1000, 300, 3);
            app_set_state(STATE_IDLE);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
          }
        } else if (pin_change_state == PIN_CHANGE_NEW) {
          strncpy(temp_new_pin, input_buffer, sizeof(temp_new_pin) - 1);
          pin_change_state = PIN_CHANGE_REP;
          input_len = 0;
          hal_io_buzzer_beep(2700, 100, 1);
        } else if (pin_change_state == PIN_CHANGE_REP) {
          if (strcmp(input_buffer, temp_new_pin) == 0) {
            strncpy(pin_change_user.pin, temp_new_pin,
                    sizeof(pin_change_user.pin) - 1);
            nv_storage_add_user(&pin_change_user); // Updates existing user
            hal_io_buzzer_beep(2700, 100, 2);
            app_set_state(STATE_IDLE);
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          } else {
            hal_io_buzzer_beep(1000, 300, 3);
            app_set_state(STATE_IDLE);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
          }
        }
      } else {
        if (input_len < 10) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      }
    }
    break;
  }
  case STATE_MASTER_ADD_MODE:
  case STATE_MASTER_DELETE_MODE: {
    if (elapsed_ms >= 10000) {
      app_set_state(STATE_IDLE);
      hal_io_buzzer_beep(1000, 500, 3);
      break;
    }

    char key = '\0';
    if (hal_touch_read_key(&key)) {
      if (hal_shift_reg_is_blocking_anim_playing())
        break;
      state_timer_start = esp_timer_get_time();
      if (key == '*') {
        app_set_state(STATE_IDLE);
        hal_io_buzzer_beep(1000, 100, 1);
      } else if (key == '#') {
        input_buffer[input_len] = '\0';
        if (input_len > 0) {
          if (current_state == STATE_MASTER_ADD_MODE) {
            if (pending_user_id == 0) {
              pending_user_id = atoi(input_buffer);
              input_len = 0;
              hal_io_buzzer_beep(2700, 100, 1);
            } else {
              // Add PIN
              user_record_t new_user = {0};
              new_user.user_id = pending_user_id;
              new_user.is_active = true;
              strncpy(new_user.pin, input_buffer, sizeof(new_user.pin) - 1);
              if (nv_storage_add_user(&new_user)) {
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
                hal_io_buzzer_beep(1000, 300, 3);
                hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
              }
              pending_user_id = 0;
              input_len = 0;
            }
          } else {
            // Delete mode: Delete by UserID
            uint16_t uid = atoi(input_buffer);
            if (nv_storage_delete_user(uid)) {
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
              hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
            }
            input_len = 0;
          }
        }
      } else {
        if (input_len < 62) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      }
    }

    uint32_t rfid_uid = 0;
    bool rfid_ok = false;
    if (active_hw_version != HW_VERSION_PIN_QR) {
      rfid_ok = mfrc522_check_card(&rfid_uid);
    }
    bool weigand_ok = hal_wiegand_available();

    if (weigand_ok) {
      uint16_t wg_bits = hal_wiegand_peek_bit_count();
      if (wg_bits > 64 || (wg_bits >= 48 && (wg_bits % 8 == 0) &&
                           hal_wiegand_is_printable_string())) {
        char wg_qr_str[64];
        uint16_t wg_qr_len = 0;
        if (hal_wiegand_read_string(wg_qr_str, sizeof(wg_qr_str), &wg_qr_len)) {
          ESP_LOGI(TAG, "[MASTER_MODE] Wiegand QR read: %s", wg_qr_str);
          state_timer_start = esp_timer_get_time();

          char norm_qr[32];
          app_normalize_qr_string(wg_qr_str, norm_qr, sizeof(norm_qr));

          if (nv_storage_is_master_credential(norm_qr)) {
            if (current_state == STATE_MASTER_ADD_MODE && elapsed_ms < 5000) {
              app_set_state(STATE_MASTER_DELETE_MODE);
            } else if (current_state == STATE_MASTER_DELETE_MODE &&
                       elapsed_ms < 5000) {
              if (nv_storage_delete_all_users()) {
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              }
              app_set_state(STATE_IDLE);
            } else {
              app_set_state(STATE_IDLE);
              hal_io_buzzer_beep(2700, 100, 2);
            }
          } else {
            if (current_state == STATE_MASTER_ADD_MODE) {
              user_record_t new_user = {0};
              new_user.user_id = (pending_user_id > 0)
                                     ? pending_user_id
                                     : nv_storage_get_free_user_id();
              new_user.is_active = true;
              strncpy(new_user.qr_id, norm_qr, sizeof(new_user.qr_id) - 1);
              if (nv_storage_add_user(&new_user)) {
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
                hal_io_buzzer_beep(1000, 300, 3);
                hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
              }
              pending_user_id = 0;
              input_len = 0;
            } else {
              user_record_t u;
              if (nv_storage_find_user_by_qr(norm_qr, &u)) {
                nv_storage_delete_user(u.user_id);
                ESP_LOGI(TAG,
                         "[MASTER_MODE] Wiegand QR User DELETED: UserID:%d",
                         u.user_id);
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                ESP_LOGW(TAG, "[MASTER_MODE] Wiegand QR %s NOT FOUND", norm_qr);
                app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
                hal_io_buzzer_beep(1000, 300, 3);
              }
            }
          }
        }
        weigand_ok = false;
      }
    }

    if (rfid_ok || weigand_ok) {
      hal_shift_reg_wakeup_keypad();
      if (config.is_online) {
        api_client_send_session_start(0x01);
      }
      uint8_t bit_len = 0;
      uint32_t card_id =
          (rfid_uid != 0) ? rfid_uid : hal_wiegand_read(&bit_len);
      card_id =
          app_normalize_rfid_card_id(card_id, (rfid_uid == 0) ? bit_len : 0);

      if (rfid_uid == 0 && bit_len == 34) {
        user_record_t tmp_usr;
        char master_str[24];
        if (nv_storage_find_user_by_wiegand34(card_id, &tmp_usr)) {
          uint32_t db_id = (uint32_t)strtoul(tmp_usr.card_id, NULL, 10);
          ESP_LOGI(TAG,
                   "Wiegand 34 Smart Match: Replaced Wiegand ID %lu with "
                   "Registered User ID %lu",
                   (unsigned long)card_id, (unsigned long)db_id);
          card_id = db_id;
        } else if (nv_storage_find_master_by_wiegand34(card_id, master_str)) {
          uint32_t db_id = (uint32_t)strtoul(master_str, NULL, 10);
          ESP_LOGI(TAG,
                   "Wiegand 34 Smart Match: Replaced Wiegand ID %lu with "
                   "Master ID %lu",
                   (unsigned long)card_id, (unsigned long)db_id);
          card_id = db_id;
        }
      }

      char card_str[24];
      snprintf(card_str, sizeof(card_str), "%lu", (unsigned long)card_id);
      state_timer_start = esp_timer_get_time();

      ESP_LOGI(TAG, "[MASTER_MODE] Card/Wiegand read: %s", card_str);
      if (nv_storage_is_master_credential(card_str)) {
        int32_t master_id = nv_storage_get_master_id(card_str);
        if (current_state == STATE_MASTER_ADD_MODE && elapsed_ms < 5000) {
          ESP_LOGI(
              TAG,
              "[MASTER_MODE] Master card (ID: %d) -> switch to DELETE mode",
              (int)master_id);
          app_set_state(STATE_MASTER_DELETE_MODE);
        } else if (current_state == STATE_MASTER_DELETE_MODE &&
                   elapsed_ms < 5000) {
          ESP_LOGW(
              TAG,
              "[MASTER_MODE] Master card (ID: %d) 3rd scan -> Delete ALL users",
              (int)master_id);
          if (nv_storage_delete_all_users()) {
            ESP_LOGI(TAG, "[MASTER_MODE] ALL users DELETED");
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          }
          app_set_state(STATE_IDLE);
        } else {
          ESP_LOGI(TAG, "[MASTER_MODE] Master card (ID: %d) -> exit to IDLE",
                   (int)master_id);
          app_set_state(STATE_IDLE);
          hal_io_buzzer_beep(2700, 100, 2);
        }
      } else {
        if (current_state == STATE_MASTER_ADD_MODE) {
          user_record_t u;
          if (nv_storage_find_user_by_card(card_str, &u)) {
            ESP_LOGW(TAG,
                     "[MASTER_MODE] Card %s ALREADY registered to UserID:%d",
                     card_str, u.user_id);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
            hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
            pending_user_id = 0;
            input_len = 0;
          } else {
            user_record_t new_user = {0};
            new_user.user_id = (pending_user_id > 0)
                                   ? pending_user_id
                                   : nv_storage_get_free_user_id();
            new_user.is_active = true;
            strncpy(new_user.card_id, card_str, sizeof(new_user.card_id) - 1);
            ESP_LOGI(TAG, "[MASTER_MODE] ADD: UserID=%d, Card=%s",
                     new_user.user_id, card_str);
            if (nv_storage_add_user(&new_user)) {
              ESP_LOGI(TAG, "[MASTER_MODE] User SAVED: UserID=%d",
                       new_user.user_id);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              ESP_LOGW(TAG, "[MASTER_MODE] User FAILED to save: UserID=%d",
                       new_user.user_id);
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
              hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
            }
            pending_user_id = 0;
            input_len = 0;
          }
        } else {
          user_record_t u;
          if (nv_storage_find_user_by_card(card_str, &u)) {
            nv_storage_delete_user(u.user_id);
            ESP_LOGI(TAG, "[MASTER_MODE] DELETE: UserID=%d, Card=%s", u.user_id,
                     card_str);
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          } else {
            ESP_LOGW(TAG, "[MASTER_MODE] DELETE FAILED: Card %s not found",
                     card_str);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
          }
        }
      }
    }

    if (active_hw_version == HW_VERSION_PIN_QR &&
        hal_qr_read(s_raw_qr_buf, sizeof(s_raw_qr_buf))) {
      state_timer_start = esp_timer_get_time();
      hal_shift_reg_wakeup_keypad();
      if (config.is_online) {
        api_client_send_session_start(0x01);
      }
      char qr_str[32];
      app_normalize_qr_string(s_raw_qr_buf, qr_str, sizeof(qr_str));

      if (nv_storage_is_master_credential(qr_str)) {
        if (current_state == STATE_MASTER_ADD_MODE && elapsed_ms < 5000) {
          app_set_state(STATE_MASTER_DELETE_MODE);
        } else if (current_state == STATE_MASTER_DELETE_MODE &&
                   elapsed_ms < 5000) {
          if (nv_storage_delete_all_users()) {
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          }
          app_set_state(STATE_IDLE);
        } else {
          app_set_state(STATE_IDLE);
          hal_io_buzzer_beep(2700, 100, 2);
        }
      } else {
        if (current_state == STATE_MASTER_ADD_MODE) {
          user_record_t new_user = {0};
          new_user.user_id = (pending_user_id > 0)
                                 ? pending_user_id
                                 : nv_storage_get_free_user_id();
          new_user.is_active = true;
          strncpy(new_user.qr_id, qr_str, sizeof(new_user.qr_id) - 1);
          if (nv_storage_add_user(&new_user)) {
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          } else {
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
            hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
          }
          pending_user_id = 0;
          input_len = 0;
        } else {
          user_record_t u;
          if (nv_storage_find_user_by_qr(qr_str, &u)) {
            nv_storage_delete_user(u.user_id);
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          } else {
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
          }
        }
      }
    }
    break;
  }
  case STATE_PROGRAMMING: {
    if (elapsed_ms >= 25000) {
      app_set_state(STATE_IDLE);
      hal_io_buzzer_beep(1000, 500, 3);
      break;
    }

    char key = '\0';
    if (hal_touch_read_key(&key)) {
      ESP_LOGI(TAG, "[PROGRAMMING] Key pressed: %c", key);
      state_timer_start = esp_timer_get_time();

      if (key == '*') {
        hal_io_buzzer_beep(1000, 100, 1);
        app_set_state(STATE_IDLE);
        break;
      }

      if (prog_state == PROG_STATE_ROOT) {
        input_len = 0;
        if (key == '1') {
          prog_state = PROG_STATE_UPDATE_MASTER_CODE_1;
          ESP_LOGI(TAG, "Menu 1: Update Master Code");
          hal_io_buzzer_beep(2700, 50, 1);
        } else if (key == '2') {
          prog_state = PROG_STATE_ADD_MASTER_CARD;
          ESP_LOGI(TAG, "Menu 2: Add Master Card/QR");
          hal_io_buzzer_beep(2700, 50, 1);
        } else if (key == '3') {
          if (config.is_online) {
            ESP_LOGW(TAG, "Menu 3: Locked (Online Mode)");
            hal_io_buzzer_beep(1000, 300, 3);
          } else {
            prog_state = PROG_STATE_ADD_USER;
            ESP_LOGI(TAG, "Menu 3: Add User");
            hal_io_buzzer_beep(2700, 50, 1);
          }
        } else if (key == '4') {
          if (config.is_online) {
            ESP_LOGW(TAG, "Menu 4: Locked (Online Mode)");
            hal_io_buzzer_beep(1000, 300, 3);
          } else {
            prog_state = PROG_STATE_DEL_USER;
            ESP_LOGI(TAG, "Menu 4: Delete User");
            hal_io_buzzer_beep(2700, 50, 1);
          }
        } else if (key == '5') {
          if (config.is_online) {
            ESP_LOGW(TAG, "Menu 5: Locked (Online Mode)");
            hal_io_buzzer_beep(1000, 300, 3);
          } else if (active_hw_version == HW_VERSION_PIN_QR) {
            ESP_LOGW(TAG, "Menu 5: Locked (PIN+QR has no RFID hardware)");
            hal_io_buzzer_beep(1000, 300, 3);
          } else {
            prog_state = PROG_STATE_BLOCK_ADD_USER_ID;
            ESP_LOGI(TAG, "Menu 5: Block Card Add");
            hal_io_buzzer_beep(2700, 50, 1);
          }
        } else if (key == '6') {
          prog_state = PROG_STATE_RELAY;
          ESP_LOGI(TAG, "Menu 6: Relay Time");
          hal_io_buzzer_beep(2700, 50, 1);
        } else if (key == '7') {
          prog_state = PROG_STATE_AUDIO_VISUAL;
          ESP_LOGI(TAG, "Menu 7: Audio/Visual");
          hal_io_buzzer_beep(2700, 50, 1);
        } else if (key == '8') {
          prog_state = PROG_STATE_DEL_MASTER_CARD;
          ESP_LOGI(TAG, "Menu 8: Delete Master Card/QR");
          hal_io_buzzer_beep(2700, 50, 1);
        } else if (key == '9') {
          // Menu 9: Factory Reset (9 9 #)
          prog_state = PROG_STATE_FACTORY_RESET;
          ESP_LOGI(TAG, "Menu 9: Factory Reset");
          hal_io_buzzer_beep(2700, 50, 1);
        } else if (key == '0') {
          prog_state = PROG_STATE_WIEGAND_FORMAT;
          ESP_LOGI(TAG, "Menu 0: Wiegand Format (26 / 34)");
          hal_io_buzzer_beep(2700, 50, 1);
        } else {
          hal_io_buzzer_beep(1000, 200, 1);
        }
      } else if (prog_state == PROG_STATE_UPDATE_MASTER_CODE_1) {
        if (key == '#') {
          input_buffer[input_len] = '\0';
          strncpy(temp_master_code, input_buffer, sizeof(temp_master_code) - 1);
          prog_state = PROG_STATE_UPDATE_MASTER_CODE_2;
          input_len = 0;
          hal_io_buzzer_beep(2700, 100, 1);
        } else if (input_len < 7) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_UPDATE_MASTER_CODE_2) {
        if (key == '#') {
          input_buffer[input_len] = '\0';
          if (strcmp(input_buffer, temp_master_code) == 0) {
            strncpy(config.master_code, temp_master_code,
                    sizeof(config.master_code) - 1);
            nv_storage_set_config(&config);
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2); // 2 Rapid
          } else {
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3); // 3 Slow
            hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
          }
          prog_state = PROG_STATE_ROOT;
        } else if (input_len < 7) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_ADD_MASTER_CARD) {
        if (key == '#') {
          if (input_len == 0) {
            prog_state = PROG_STATE_ROOT;
          } else {
            input_buffer[input_len] = '\0';
            if (input_len == 8 || input_len == 10 || input_len == 17) {
              // Add Auto ID Master with card number
              if (nv_storage_add_master_credential(0, input_buffer)) {
                ESP_LOGI(TAG, "DEBUG: Master ADDED (AutoID) - Card: %s",
                         input_buffer);
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                ESP_LOGW(TAG, "DEBUG: Failed to add Master (AutoID) - Card: %s",
                         input_buffer);
                app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
                hal_io_buzzer_beep(1000, 300, 3);
                hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
              }
              prog_state = PROG_STATE_ADD_MASTER_CARD;
            } else {
              // Specific ID entered
              pending_user_id = atoi(input_buffer);
              prog_state = PROG_STATE_ADD_MASTER_SPECIFIC;
              hal_io_buzzer_beep(2700, 100, 1);
            }
          }
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_ADD_MASTER_SPECIFIC) {
        if (key == '#') {
          if (input_len > 0) {
            input_buffer[input_len] = '\0';
            if (input_len == 8 || input_len == 10 || input_len == 17) {
              if (nv_storage_add_master_credential(pending_user_id,
                                                   input_buffer)) {
                ESP_LOGI(TAG,
                         "DEBUG: Master ADDED (SpecificID) - ID: %d, Card: %s",
                         pending_user_id, input_buffer);
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                ESP_LOGW(TAG,
                         "DEBUG: Failed to add Master (SpecificID) - ID: %d",
                         pending_user_id);
                app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
                hal_io_buzzer_beep(1000, 300, 3);
                hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
              }
              prog_state = PROG_STATE_ADD_MASTER_CARD;
            } else {
              ESP_LOGW(TAG, "Invalid card length for Master Card");
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
              prog_state = PROG_STATE_ADD_MASTER_CARD;
            }
          }
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_ADD_USER) {
        if (key == '#') {
          if (input_len > 0) {
            input_buffer[input_len] = '\0';
            if (input_len == 8 || input_len == 10 || input_len == 17) {
              // Auto ID Add Card User
              user_record_t new_user = {0};
              new_user.user_id = nv_storage_get_free_user_id(); // Auto ID
              new_user.is_active = true;
              strncpy(new_user.card_id, input_buffer,
                      sizeof(new_user.card_id) - 1);
              if (nv_storage_add_user(&new_user)) {
                ESP_LOGI(TAG,
                         "DEBUG: User ADDED (AutoID) - UserID: %d, Card: %s",
                         new_user.user_id, new_user.card_id);
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                ESP_LOGW(TAG, "DEBUG: Failed to add AutoID user (Card: %s)",
                         new_user.card_id);
                app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
                hal_io_buzzer_beep(1000, 300, 3);
                hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
              }
              prog_state = PROG_STATE_ADD_USER;
            } else if (input_len >= 4 && input_len <= 6) {
              // PIN Auto ID - Step 1: Confirm PIN
              strncpy(temp_new_pin, input_buffer, sizeof(temp_new_pin) - 1);
              prog_state = PROG_STATE_ADD_USER_PIN_CONFIRM;
              ESP_LOGI(TAG, "PIN entered. Waiting for confirmation.");
              hal_io_buzzer_beep(2700, 100, 1);
            } else {
              // Invalid input length for Menu 2
              ESP_LOGW(TAG, "Invalid input for Menu 2. Must be PIN (4-6) or "
                            "Card (8,10,17)");
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
              prog_state = PROG_STATE_ADD_USER;
            }
          }
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_ADD_USER_PIN_CONFIRM) {
        if (key == '#') {
          input_buffer[input_len] = '\0';
          if (strcmp(input_buffer, temp_new_pin) == 0) {
            user_record_t new_user = {0};
            new_user.user_id = nv_storage_get_free_user_id();
            new_user.is_active = true;
            strncpy(new_user.pin, input_buffer, sizeof(new_user.pin) - 1);
            if (new_user.user_id > 0 && nv_storage_add_user(&new_user)) {
              ESP_LOGI(TAG, "DEBUG: User ADDED (AutoID) - UserID: %d, PIN: %s",
                       new_user.user_id, new_user.pin);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              ESP_LOGW(TAG, "DEBUG: Failed to add AutoID user (PIN)");
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
          } else {
            ESP_LOGW(TAG, "DEBUG: PIN Confirm failed");
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
          }
          prog_state = PROG_STATE_ADD_USER;
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_BLOCK_ADD_USER_ID) {
        if (key == '#') {
          input_buffer[input_len] = '\0';
          pending_user_id = atoi(input_buffer);
          if (pending_user_id > 0 && pending_user_id <= 9988) {
            prog_state = PROG_STATE_BLOCK_ADD_QUANTITY;
            hal_io_buzzer_beep(2700, 100, 1);
          } else {
            ESP_LOGW(TAG, "Invalid UserID: %d (Must be 1-9988)",
                     pending_user_id);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
            prog_state = PROG_STATE_ROOT;
          }
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_BLOCK_ADD_QUANTITY) {
        if (key == '#') {
          input_buffer[input_len] = '\0';
          if (input_len == 8 || input_len == 10 || input_len == 17) {
            // Specific ID Card Add
            user_record_t u = {0};
            u.user_id = pending_user_id;
            u.is_active = true;
            strncpy(u.card_id, input_buffer, sizeof(u.card_id) - 1);
            if (nv_storage_add_user(&u)) {
              ESP_LOGI(TAG, "DEBUG: Card added to Specific ID %d",
                       pending_user_id);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
            prog_state = PROG_STATE_ROOT;
          } else if (input_len >= 4 && input_len <= 6) {
            // Specific ID PIN Add
            user_record_t u = {0};
            u.user_id = pending_user_id;
            u.is_active = true;
            strncpy(u.pin, input_buffer, sizeof(u.pin) - 1);
            if (nv_storage_add_user(&u)) {
              ESP_LOGI(TAG, "DEBUG: PIN added to Specific ID %d",
                       pending_user_id);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
            prog_state = PROG_STATE_ROOT;
          } else {
            // Block Add Quantity
            pending_quantity = atoi(input_buffer);
            if (pending_quantity > 0) {
              prog_state = PROG_STATE_BLOCK_ADD_CARD;
              hal_io_buzzer_beep(2700, 100, 1);
            } else {
              ESP_LOGW(TAG, "Invalid Quantity: %d", pending_quantity);
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
              prog_state = PROG_STATE_ROOT;
            }
          }
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_BLOCK_ADD_CARD) {
        if (key == '#') {
          input_buffer[input_len] = '\0';
          if (input_len == 8 || input_len == 10 || input_len == 17) {
            long long start_card = atoll(input_buffer);
            int added_count = 0;
            for (uint16_t i = 0; i < pending_quantity; i++) {
              user_record_t new_user = {0};
              new_user.user_id = pending_user_id + i;
              if (new_user.user_id > 9988)
                break; // Reached max capacity
              new_user.is_active = true;
              snprintf(new_user.card_id, sizeof(new_user.card_id), "%lld",
                       start_card + i);
              if (nv_storage_add_user(&new_user)) {
                added_count++;
              }
            }
            if (added_count > 0) {
              ESP_LOGI(TAG, "DEBUG: Block Added %d users starting from ID %d",
                       added_count, pending_user_id);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
          } else {
            ESP_LOGW(TAG, "Invalid card length for Block Add");
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
          }
          prog_state = PROG_STATE_ROOT;
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_DEL_USER) {
        if (key == '#') {
          input_buffer[input_len] = '\0';
          if (strcmp(input_buffer, config.master_code) == 0) {
            if (nv_storage_delete_all_users()) {
              ESP_LOGI(TAG, "DEBUG: ALL USERS DELETED via Master Code!");
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            }
          } else if (input_len == 8 || input_len == 10 || input_len == 17) {
            user_record_t u;
            if (nv_storage_find_user_by_card(input_buffer, &u)) {
              if (nv_storage_delete_user(u.user_id)) {
                ESP_LOGI(TAG, "DEBUG: User DELETED by Card - UserID: %d",
                         u.user_id);
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              }
            } else {
              ESP_LOGW(TAG, "DEBUG: Failed to delete user - Card %s not found",
                       input_buffer);
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
          } else {
            uint16_t uid = atoi(input_buffer);
            if (nv_storage_delete_user(uid)) {
              ESP_LOGI(TAG, "DEBUG: User DELETED by ID - UserID: %d", uid);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              ESP_LOGW(TAG, "DEBUG: Failed to delete user - ID %d not found",
                       uid);
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
          }
          prog_state = PROG_STATE_DEL_USER;
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else if (prog_state == PROG_STATE_DEL_ALL_CONFIRM) {
        if (key == '#') {
          if (nv_storage_delete_all_users()) {
            ESP_LOGI(TAG, "DEBUG: ALL USERS DELETED via Master Card Confirm!");
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          }
          prog_state = PROG_STATE_ROOT;
        } else if (key == '*') {
          prog_state = PROG_STATE_ROOT;
        }
      } else if (prog_state == PROG_STATE_DEL_MASTER_CARD) {
        if (key == '#') {
          if (input_len == 0) {
            prog_state = PROG_STATE_ROOT;
          } else {
            input_buffer[input_len] = '\0';
            if (strcmp(input_buffer, config.master_code) == 0) {
              nv_storage_clear_master_credentials();
              nv_storage_get_config(&config);
              ESP_LOGI(
                  TAG,
                  "DEBUG: ALL MASTER CREDENTIALS DELETED via Master Code!");
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else if (nv_storage_delete_master_credential(input_buffer)) {
              nv_storage_get_config(&config);
              ESP_LOGI(TAG, "DEBUG: Master Credential DELETED - %s",
                       input_buffer);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              uint16_t mid = atoi(input_buffer);
              if (mid >= 10000 && mid <= 10010 &&
                  nv_storage_delete_master_by_id(mid)) {
                nv_storage_get_config(&config);
                ESP_LOGI(TAG, "DEBUG: Master DELETED by ID - %d", mid);
                app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                ESP_LOGW(TAG, "DEBUG: Failed to delete Master - %s",
                         input_buffer);
                app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
                hal_io_buzzer_beep(1000, 300, 3);
                hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
              }
            }
            prog_state = PROG_STATE_DEL_MASTER_CARD;
          }
          input_len = 0;
        } else if (input_len < 18) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      } else {
        // Dynamic Configuration Menu Processing
        if (key == '#') {
          input_buffer[input_len] = '\0';
          int val = atoi(input_buffer);
          bool ok = false;

          switch (prog_state) {
          case PROG_STATE_RELAY:
            if (val <= 99) {
              config.relay_time = val;
              ok = true;
              ESP_LOGI(TAG, "Config: Relay Time = %d", val);
            }
            break;
          case PROG_STATE_AUDIO_VISUAL:
            if (val == 0) {
              config.sound_enabled = false;
              hal_io_set_sound_enabled(false);
              ESP_LOGI(TAG, "Config: Sound Disabled");
            } else if (val == 1) {
              config.sound_enabled = true;
              hal_io_set_sound_enabled(true);
              ESP_LOGI(TAG, "Config: Sound Enabled");
            } else if (val == 2) {
              config.led_always_on = false;
              ESP_LOGI(TAG, "Config: LED Always On Disabled");
            } else if (val == 3) {
              config.led_always_on = true;
              ESP_LOGI(TAG, "Config: LED Always On Enabled");
            }
            ok = true;
            break;
          case PROG_STATE_FACTORY_RESET:
            if (val == 9) {
              // 9 -> 9 -> # => 99# : Factory Reset
              ESP_LOGW(TAG, "Factory Reset triggered from Keypad (99#)");
              app_set_state(STATE_FACTORY_RESET);
              ok = true;
            } else {
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
            break;
          case PROG_STATE_WIEGAND_FORMAT:
            if (val == 26 || val == 34) {
              config.reserved_0 = (uint8_t)val;
              ok = true;
              ESP_LOGI(TAG, "Config: Wiegand/RFID Format set to %d-bit", val);
            }
            break;
          default:
            break;
          }

          if (ok) {
            nv_storage_set_config(&config);
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          } else {
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
          }
          prog_state = PROG_STATE_ROOT;
        } else if (input_len < 10) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      }
    }

    // High-Speed RF/QR Handlers
    uint32_t rfid_uid = 0;
    bool rfid_ok = false;
    if (active_hw_version != HW_VERSION_PIN_QR) {
      rfid_ok = mfrc522_check_card(&rfid_uid);
    }
    bool weigand_ok = hal_wiegand_available();
    if (weigand_ok) {
      uint16_t wg_bits = hal_wiegand_peek_bit_count();
      if (wg_bits > 64 || (wg_bits >= 48 && (wg_bits % 8 == 0) &&
                           hal_wiegand_is_printable_string())) {
        char wg_qr_str[64];
        uint16_t wg_qr_len = 0;
        if (hal_wiegand_read_string(wg_qr_str, sizeof(wg_qr_str), &wg_qr_len)) {
          ESP_LOGI(TAG, "[PROG] Wiegand QR read: %s", wg_qr_str);
          state_timer_start = esp_timer_get_time();
          char qr_str[32];
          app_normalize_qr_string(wg_qr_str, qr_str, sizeof(qr_str));

          if (prog_state == PROG_STATE_ADD_MASTER_CARD) {
            if (nv_storage_add_master_credential(0, qr_str)) {
              ESP_LOGI(TAG, "[PROG] Master QR SAVED: %s", qr_str);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
            prog_state = PROG_STATE_ROOT;
          } else if (prog_state == PROG_STATE_ADD_USER) {
            user_record_t new_user = {0};
            new_user.user_id = nv_storage_get_free_user_id();
            new_user.is_active = true;
            strncpy(new_user.qr_id, qr_str, sizeof(new_user.qr_id) - 1);
            if (nv_storage_add_user(&new_user)) {
              ESP_LOGI(TAG,
                       "[PROG] User SAVED via Wiegand QR: UserID=%d, QR=%s",
                       new_user.user_id, qr_str);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
            prog_state = PROG_STATE_ROOT;
          } else if (prog_state == PROG_STATE_DEL_USER) {
            user_record_t u;
            if (nv_storage_find_user_by_qr(qr_str, &u)) {
              nv_storage_delete_user(u.user_id);
              ESP_LOGI(TAG,
                       "[PROG] User DELETED via Wiegand QR: UserID=%d, QR=%s",
                       u.user_id, qr_str);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
            prog_state = PROG_STATE_ROOT;
          }
        } else {
          hal_wiegand_flush();
        }
        weigand_ok = false;
      }
    }
    if (rfid_ok || weigand_ok) {
      hal_shift_reg_wakeup_keypad();
      if (config.is_online) {
        api_client_send_session_start(0x01);
      }
      uint8_t bit_len = 0;
      uint32_t card_id =
          (rfid_uid != 0) ? rfid_uid : hal_wiegand_read(&bit_len);
      card_id =
          app_normalize_rfid_card_id(card_id, (rfid_uid == 0) ? bit_len : 0);
      state_timer_start = esp_timer_get_time();
      char card_str[24];
      snprintf(card_str, sizeof(card_str), "%lu", (unsigned long)card_id);

      if (prog_state == PROG_STATE_ADD_MASTER_CARD) {
        ESP_LOGI(TAG, "[PROG] RFID read: %s -> Add Master Card (AutoID)",
                 card_str);
        if (nv_storage_add_master_credential(0, card_str)) {
          ESP_LOGI(TAG, "[PROG] Master Card SAVED: %s", card_str);
          app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          hal_io_buzzer_beep(2700, 100, 2);
        } else {
          ESP_LOGW(TAG,
                   "[PROG] Master Card FAILED (already exists or full): %s",
                   card_str);
          app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
          hal_io_buzzer_beep(1000, 300, 3);
          hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
        }
        prog_state = PROG_STATE_ROOT;
      } else if (prog_state == PROG_STATE_ADD_MASTER_SPECIFIC) {
        ESP_LOGI(TAG, "[PROG] RFID read: %s -> Add Master Card (ID:%d)",
                 card_str, pending_user_id);
        if (nv_storage_add_master_credential(pending_user_id, card_str)) {
          ESP_LOGI(TAG, "[PROG] Master Card SAVED: ID=%d, Card=%s",
                   pending_user_id, card_str);
          app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          hal_io_buzzer_beep(2700, 100, 2);
        } else {
          ESP_LOGW(TAG, "[PROG] Master Card FAILED: ID=%d, Card=%s",
                   pending_user_id, card_str);
          app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
          hal_io_buzzer_beep(1000, 300, 3);
          hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
        }
        prog_state = PROG_STATE_ROOT;
      } else if (prog_state == PROG_STATE_ADD_USER) {
        if (nv_storage_is_master_credential(card_str)) {
          ESP_LOGI(TAG,
                   "[PROG] Master card scanned in Add User -> Exit to IDLE");
          app_set_state(STATE_IDLE);
        } else {
          user_record_t u;
          if (nv_storage_find_user_by_card(card_str, &u)) {
            ESP_LOGW(TAG, "[PROG] Card %s ALREADY registered to UserID:%d",
                     card_str, u.user_id);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
            hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
          } else {
            user_record_t new_user = {0};
            new_user.user_id = nv_storage_get_free_user_id();
            new_user.is_active = true;
            strncpy(new_user.card_id, card_str, sizeof(new_user.card_id) - 1);
            ESP_LOGI(TAG, "[PROG] RFID read: %s -> Add User (UserID:%d)",
                     card_str, new_user.user_id);
            if (nv_storage_add_user(&new_user)) {
              ESP_LOGI(TAG, "[PROG] User SAVED: UserID=%d, Card=%s",
                       new_user.user_id, card_str);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              ESP_LOGW(TAG, "[PROG] User FAILED to save: UserID=%d, Card=%s",
                       new_user.user_id, card_str);
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
          }
        }
      } else if (prog_state == PROG_STATE_BLOCK_ADD_QUANTITY) {
        ESP_LOGI(TAG, "[PROG] RFID read: %s -> Specific ID Card Add", card_str);
        user_record_t u = {0};
        u.user_id = pending_user_id;
        u.is_active = true;
        strncpy(u.card_id, card_str, sizeof(u.card_id) - 1);
        if (nv_storage_add_user(&u)) {
          app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          hal_io_buzzer_beep(2700, 100, 2);
        } else {
          app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
          hal_io_buzzer_beep(1000, 300, 3);
        }
        prog_state = PROG_STATE_ROOT;
      } else if (prog_state == PROG_STATE_DEL_USER) {
        ESP_LOGI(TAG, "[PROG] RFID read: %s -> Delete User", card_str);
        if (nv_storage_is_master_credential(card_str)) {
          int32_t master_id = nv_storage_get_master_id(card_str);
          ESP_LOGI(
              TAG,
              "[PROG] Master card (ID: %d) -> Delete ALL users (Waiting for #)",
              (int)master_id);
          prog_state = PROG_STATE_DEL_ALL_CONFIRM;
          hal_io_buzzer_beep(2700, 100, 1);
        } else {
          user_record_t u;
          if (nv_storage_find_user_by_card(card_str, &u)) {
            nv_storage_delete_user(u.user_id);
            ESP_LOGI(TAG, "[PROG] User DELETED: UserID=%d, Card=%s", u.user_id,
                     card_str);
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          } else {
            ESP_LOGW(TAG, "[PROG] Delete FAILED: Card %s not found in DB",
                     card_str);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
          }
        }
      } else if (prog_state == PROG_STATE_DEL_MASTER_CARD) {
        ESP_LOGI(TAG, "[PROG] RFID read: %s -> Delete Master Card", card_str);
        if (nv_storage_delete_master_credential(card_str)) {
          nv_storage_get_config(&config);
          ESP_LOGI(TAG, "[PROG] Master Card DELETED: %s", card_str);
          app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          hal_io_buzzer_beep(2700, 100, 2);
        } else {
          ESP_LOGW(TAG, "[PROG] Delete Master FAILED: Card %s is not a Master",
                   card_str);
          app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
          hal_io_buzzer_beep(1000, 300, 3);
          hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
        }
      }
    }

    if (active_hw_version == HW_VERSION_PIN_QR &&
        hal_qr_read(s_raw_qr_buf, sizeof(s_raw_qr_buf))) {
      state_timer_start = esp_timer_get_time();
      hal_shift_reg_wakeup_keypad();
      if (config.is_online) {
        api_client_send_session_start(0x01);
      }
      char qr_str[32];
      app_normalize_qr_string(s_raw_qr_buf, qr_str, sizeof(qr_str));

      if (prog_state == PROG_STATE_ADD_MASTER_CARD) {
        ESP_LOGI(TAG, "[PROG] QR read: %s -> Add Master Card (AutoID)", qr_str);
        if (nv_storage_add_master_credential(0, qr_str)) {
          ESP_LOGI(TAG, "[PROG] Master QR SAVED: %s", qr_str);
          app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          hal_io_buzzer_beep(2700, 100, 2);
        } else {
          ESP_LOGW(TAG, "[PROG] Master QR FAILED (already exists or full): %s",
                   qr_str);
          app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
          hal_io_buzzer_beep(1000, 300, 3);
          hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
        }
        prog_state = PROG_STATE_ROOT;
      } else if (prog_state == PROG_STATE_ADD_MASTER_SPECIFIC) {
        ESP_LOGI(TAG, "[PROG] QR read: %s -> Add Master Card (ID:%d)", qr_str,
                 pending_user_id);
        if (nv_storage_add_master_credential(pending_user_id, qr_str)) {
          ESP_LOGI(TAG, "[PROG] Master QR SAVED: ID=%d, QR=%s", pending_user_id,
                   qr_str);
          app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          hal_io_buzzer_beep(2700, 100, 2);
        } else {
          ESP_LOGW(TAG, "[PROG] Master QR FAILED: ID=%d, QR=%s",
                   pending_user_id, qr_str);
          app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
          hal_io_buzzer_beep(1000, 300, 3);
          hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
        }
        prog_state = PROG_STATE_ROOT;
      } else if (prog_state == PROG_STATE_ADD_USER) {
        if (nv_storage_is_master_credential(qr_str)) {
          ESP_LOGI(TAG, "[PROG] Master QR scanned in Add User -> Exit to IDLE");
          app_set_state(STATE_IDLE);
        } else {
          user_record_t u;
          if (nv_storage_find_user_by_qr(qr_str, &u)) {
            ESP_LOGW(TAG, "[PROG] QR %s ALREADY registered to UserID:%d",
                     qr_str, u.user_id);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
            hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
          } else {
            user_record_t new_user = {0};
            new_user.user_id = nv_storage_get_free_user_id();
            new_user.is_active = true;
            strncpy(new_user.qr_id, qr_str, sizeof(new_user.qr_id) - 1);
            ESP_LOGI(TAG, "[PROG] QR read: %s -> Add User (UserID:%d)", qr_str,
                     new_user.user_id);
            if (nv_storage_add_user(&new_user)) {
              ESP_LOGI(TAG, "[PROG] User SAVED: UserID=%d, QR=%s",
                       new_user.user_id, qr_str);
              app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
              hal_io_buzzer_beep(2700, 100, 2);
            } else {
              ESP_LOGW(TAG, "[PROG] User FAILED to save: UserID=%d, QR=%s",
                       new_user.user_id, qr_str);
              app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
              hal_io_buzzer_beep(1000, 300, 3);
            }
          }
        }
      } else if (prog_state == PROG_STATE_BLOCK_ADD_QUANTITY) {
        ESP_LOGI(TAG, "[PROG] QR read: %s -> Specific ID QR Add", qr_str);
        user_record_t u = {0};
        u.user_id = pending_user_id;
        u.is_active = true;
        strncpy(u.qr_id, qr_str, sizeof(u.qr_id) - 1);
        if (nv_storage_add_user(&u)) {
          app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          hal_io_buzzer_beep(2700, 100, 2);
        } else {
          app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
          hal_io_buzzer_beep(1000, 300, 3);
        }
        prog_state = PROG_STATE_ROOT;
      } else if (prog_state == PROG_STATE_DEL_USER) {
        ESP_LOGI(TAG, "[PROG] QR read: %s -> Delete User", qr_str);
        if (nv_storage_is_master_credential(qr_str)) {
          ESP_LOGI(TAG, "[PROG] Master QR -> Delete ALL users (Waiting for #)");
          prog_state = PROG_STATE_DEL_ALL_CONFIRM;
          hal_io_buzzer_beep(2700, 100, 1);
        } else {
          user_record_t u;
          if (nv_storage_find_user_by_qr(qr_str, &u)) {
            nv_storage_delete_user(u.user_id);
            ESP_LOGI(TAG, "[PROG] User DELETED: UserID=%d, QR=%s", u.user_id,
                     qr_str);
            app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
            hal_io_buzzer_beep(2700, 100, 2);
          } else {
            ESP_LOGW(TAG, "[PROG] Delete FAILED: QR '%s' not found in DB",
                     qr_str);
            app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
            hal_io_buzzer_beep(1000, 300, 3);
          }
        }
      } else if (prog_state == PROG_STATE_DEL_MASTER_CARD) {
        ESP_LOGI(TAG, "[PROG] QR read: %s -> Delete Master QR", qr_str);
        if (nv_storage_delete_master_credential(qr_str)) {
          nv_storage_get_config(&config);
          ESP_LOGI(TAG, "[PROG] Master QR DELETED: %s", qr_str);
          app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
          hal_io_buzzer_beep(2700, 100, 2);
        } else {
          ESP_LOGW(TAG, "[PROG] Delete Master FAILED: QR '%s' is not a Master",
                   qr_str);
          app_set_temp_led(LED_COLOR_RED, LED_MODE_SOLID, 3000);
          hal_io_buzzer_beep(1000, 300, 3);
          hal_shift_reg_play_anim(ANIM_ERROR_FLASH, 3000);
        }
      }
    }

    break;
  }
  case STATE_DOOR_OPEN: {
    // 1. Timeout for timed mode
    if (config.relay_time > 0 && elapsed_ms >= (config.relay_time * 1000)) {
      relay_state = false;
      hal_io_relay_set(false);
      app_set_state(STATE_IDLE);
      break;
    }

    // 2. Guard window: Ignore credential reads during the first 2500ms
    // to prevent instant toggling or duplicate beeping from the initial
    // presentation.
    if (elapsed_ms < 2500) {
      if (active_hw_version == HW_VERSION_PIN_QR) {
        hal_qr_read(s_raw_qr_buf, sizeof(s_raw_qr_buf));
      }
      if (hal_wiegand_available()) {
        uint8_t discard_bits;
        hal_wiegand_read(&discard_bits);
      }
      break;
    }

    // 3. Scan inputs after guard window (elapsed_ms >= 2500)
    uint32_t rfid_uid = 0;
    bool rfid_ok = false;
    if (active_hw_version != HW_VERSION_PIN_QR) {
      rfid_ok = mfrc522_check_card(&rfid_uid);
    }
    bool weigand_ok = hal_wiegand_available();

    if (weigand_ok) {
      uint16_t wg_bits = hal_wiegand_peek_bit_count();
      if (wg_bits > 64 || (wg_bits >= 48 && (wg_bits % 8 == 0) &&
                           hal_wiegand_is_printable_string())) {
        char wg_qr_str[64];
        uint16_t wg_qr_len = 0;
        if (hal_wiegand_read_string(wg_qr_str, sizeof(wg_qr_str), &wg_qr_len)) {
          ESP_LOGI(TAG, "Wiegand QR Received in DOOR_OPEN: %s", wg_qr_str);
          char norm_wg_qr[32];
          app_normalize_qr_string(wg_qr_str, norm_wg_qr, sizeof(norm_wg_qr));
          if (nv_storage_is_master_credential(norm_wg_qr)) {
            ESP_LOGI(
                TAG,
                "Master QR scanned during DOOR_OPEN. Entering Master Mode.");
            relay_state = false;
            hal_io_relay_set(false);
            hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
            app_set_state(STATE_MASTER_ADD_MODE);
          } else {
            user_record_t u;
            if (nv_storage_find_user_by_qr(norm_wg_qr, &u) ||
                nv_storage_find_user_by_card(norm_wg_qr, &u)) {
              if (config.relay_time == 0) {
                ESP_LOGI(TAG, "Toggle mode: Valid Wiegand QR in DOOR_OPEN -> "
                              "Closing door.");
                relay_state = false;
                hal_io_relay_set(false);
                app_set_state(STATE_IDLE);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                ESP_LOGI(TAG, "Valid Wiegand QR scanned during DOOR_OPEN. "
                              "Extending timer.");
                hal_io_buzzer_beep(2700, 100, 2);
                state_timer_start = esp_timer_get_time();
              }
            } else {
              if (!config.is_online) {
                ESP_LOGW(TAG, "Invalid Wiegand QR scanned during DOOR_OPEN: %s",
                         norm_wg_qr);
                hal_io_buzzer_beep(1000, 300, 3);
              }
            }
          }
        }
        weigand_ok = false;
      }
    }

    if (rfid_ok || weigand_ok) {
      uint8_t bit_len = 0;
      uint32_t card_id =
          (rfid_uid != 0) ? rfid_uid : hal_wiegand_read(&bit_len);
      card_id =
          app_normalize_rfid_card_id(card_id, (rfid_uid == 0) ? bit_len : 0);

      if (rfid_uid == 0 && bit_len == 34) {
        user_record_t tmp_usr;
        char master_str[24];
        if (nv_storage_find_user_by_wiegand34(card_id, &tmp_usr)) {
          card_id = (uint32_t)strtoul(tmp_usr.card_id, NULL, 10);
        } else if (nv_storage_find_master_by_wiegand34(card_id, master_str)) {
          card_id = (uint32_t)strtoul(master_str, NULL, 10);
        }
      }

      char card_str[24];
      snprintf(card_str, sizeof(card_str), "%lu", (unsigned long)card_id);

      if (nv_storage_is_master_credential(card_str)) {
        ESP_LOGI(TAG,
                 "Master card scanned during DOOR_OPEN. Entering Master Mode.");
        relay_state = false;
        hal_io_relay_set(false);
        hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
        app_set_state(STATE_MASTER_ADD_MODE);
      } else {
        user_record_t u;
        if (nv_storage_find_user_by_card(card_str, &u)) {
          if (config.relay_time == 0) {
            ESP_LOGI(TAG,
                     "Toggle mode: Valid card in DOOR_OPEN -> Closing door.");
            relay_state = false;
            hal_io_relay_set(false);
            app_set_state(STATE_IDLE);
            hal_io_buzzer_beep(2700, 100, 2);
          } else {
            ESP_LOGI(TAG,
                     "Valid card scanned during DOOR_OPEN. Extending timer.");
            hal_io_buzzer_beep(2700, 100, 2);
            state_timer_start =
                esp_timer_get_time(); // Reset timer to extend open time
          }
        } else {
          if (!config.is_online) {
            ESP_LOGW(TAG, "Invalid card scanned during DOOR_OPEN.");
            hal_io_buzzer_beep(1000, 300, 3);
          }
        }
      }
    }

    if (active_hw_version == HW_VERSION_PIN_QR &&
        hal_qr_read(s_raw_qr_buf, sizeof(s_raw_qr_buf))) {
      char norm_door_qr[32];
      app_normalize_qr_string(s_raw_qr_buf, norm_door_qr, sizeof(norm_door_qr));
      if (nv_storage_is_master_credential(norm_door_qr)) {
        ESP_LOGI(TAG,
                 "Master QR scanned during DOOR_OPEN. Entering Master Mode.");
        relay_state = false;
        hal_io_relay_set(false);
        hal_shift_reg_play_anim(ANIM_MASTER_WAVE, 0);
        app_set_state(STATE_MASTER_ADD_MODE);
      } else {
        user_record_t u_qr;
        if (nv_storage_find_user_by_qr(norm_door_qr, &u_qr)) {
          if (config.relay_time == 0) {
            ESP_LOGI(TAG,
                     "Toggle mode: Valid QR in DOOR_OPEN -> Closing door.");
            relay_state = false;
            hal_io_relay_set(false);
            app_set_state(STATE_IDLE);
            hal_io_buzzer_beep(2700, 100, 2);
          } else {
            ESP_LOGI(TAG,
                     "Valid QR scanned during DOOR_OPEN. Extending timer.");
            hal_io_buzzer_beep(2700, 100, 2);
            state_timer_start = esp_timer_get_time();
          }
        } else {
          if (!config.is_online) {
            ESP_LOGW(TAG, "Invalid QR scanned during DOOR_OPEN.");
            hal_io_buzzer_beep(1000, 300, 3);
          }
        }
      }
    }

    // Keypad handler for PIN entry in DOOR_OPEN
    if (active_hw_version != HW_VERSION_RFID_ONLY) {
      char key = '\0';
      if (hal_touch_read_key(&key)) {
        hal_shift_reg_wakeup_keypad();
        if (config.is_online) {
          api_client_send_session_start(0x01);
        }
        if (key == '#') {
          if (input_len >= 4) {
            input_buffer[input_len] = '\0';
            user_record_t u_pin;
            if (nv_storage_find_user_by_pin(input_buffer, &u_pin)) {
              if (config.relay_time == 0) {
                ESP_LOGI(
                    TAG,
                    "Toggle mode: Valid PIN in DOOR_OPEN -> Closing door.");
                relay_state = false;
                hal_io_relay_set(false);
                app_set_state(STATE_IDLE);
                hal_io_buzzer_beep(2700, 100, 2);
              } else {
                ESP_LOGI(
                    TAG,
                    "Valid PIN entered during DOOR_OPEN. Extending timer.");
                hal_io_buzzer_beep(2700, 100, 2);
                state_timer_start = esp_timer_get_time();
              }
            } else {
              if (!config.is_online) {
                ESP_LOGW(TAG, "Invalid PIN entered during DOOR_OPEN.");
                hal_io_buzzer_beep(1000, 300, 3);
              }
            }
          }
          input_len = 0;
        } else if (key == '*') {
          input_len = 0;
        } else if (input_len < sizeof(input_buffer) - 1) {
          input_buffer[input_len++] = key;
          hal_io_buzzer_beep(2700, 50, 1);
        }
      }
    }

    break;
  }
  case STATE_HW_ERROR:
    // Device is locked due to hardware failure
    // Poll hardware every 2.5 seconds to see if it recovered (without rebooting)
    if (elapsed_ms >= 2500) {
      ESP_LOGW(TAG, "Hardware unresponsive. Polling modules for hw_version=%d...", active_hw_version);
      extern bool mfrc522_init(void);
      extern bool hal_qr_init(void);
      extern bool hal_touch_init(void);

      bool is_valid = false;
      if (active_hw_version == HW_VERSION_PIN_RFID) {
        bool has_touch = hal_touch_init();
        bool has_mfrc = mfrc522_init();
        if (has_touch && has_mfrc) is_valid = true;
      } else if (active_hw_version == HW_VERSION_PIN_QR) {
        bool has_touch = hal_touch_init();
        bool has_qr = hal_qr_init();
        if (has_touch && has_qr) is_valid = true;
      } else if (active_hw_version == HW_VERSION_RFID_ONLY) {
        bool has_mfrc = mfrc522_init();
        if (has_mfrc) is_valid = true;
      }

      if (is_valid) {
        ESP_LOGI(TAG, "Hardware combination RECOVERED and is VALID! Resuming normal operation.");
        app_state_machine_init(); // Restarts logic and returns to IDLE
        break;
      }

      // Reset state timer so elapsed_ms goes back to 0
      state_timer_start = esp_timer_get_time();
    }
    break;
  case STATE_ALARM:
    // Simplified alarm clear
    if (config.alarm_time > 0 && elapsed_ms >= (config.alarm_time * 60000)) {
      app_set_state(STATE_IDLE);
    }
    break;
  case STATE_ONLINE_CARD_ENROLL: {
    if (elapsed_ms > 30000) {
      // Timeout after 30 seconds
      ESP_LOGW(TAG, "Online Card Enroll timeout");
      hal_io_buzzer_beep(1000, 300, 3);
      app_set_state(STATE_IDLE);
    } else {
      uint32_t rfid_uid = 0;
      bool rfid_ok = false;
      if (active_hw_version != HW_VERSION_PIN_QR) {
        rfid_ok = mfrc522_check_card(&rfid_uid);
      }
      bool weigand_ok = hal_wiegand_available();
      if (weigand_ok && hal_wiegand_peek_bit_count() > 64) {
        hal_wiegand_flush();
        weigand_ok = false;
      }

      if (rfid_ok || weigand_ok) {
        uint8_t bit_len = 0;
        uint32_t card_id =
            (rfid_uid != 0) ? rfid_uid : hal_wiegand_read(&bit_len);
        card_id =
            app_normalize_rfid_card_id(card_id, (rfid_uid == 0) ? bit_len : 0);

        ESP_LOGI(TAG, "Online Enroll Card Read: %lu", (unsigned long)card_id);
        hal_io_buzzer_beep(2700, 100, 2);

        // Send MQTT event
        api_client_send_nfc_enrolled_event(card_id);

        app_set_temp_led(LED_COLOR_GREEN, LED_MODE_SOLID, 1000);
        app_set_state(STATE_IDLE);
      }
    }
    break;
  }
  default:
    break;
  }
}
