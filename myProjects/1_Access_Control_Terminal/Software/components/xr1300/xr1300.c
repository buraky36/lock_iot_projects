#include "hw_config.h"

#if HW_HAS_QR

#include "xr1300.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"

#define XR1300_UART_NUM UART_NUM_1
#define XR1300_TXD_PIN PIN_QR_TX
#define XR1300_RXD_PIN PIN_QR_RX
#define RX_BUF_SIZE 2048

static QueueHandle_t xr1300_event_queue = NULL;
static TaskHandle_t xr1300_task_handle = NULL;
static bool uart_installed = false;
static bool s_crc16_active = false;
volatile bool xr1300_is_responsive = false;

static uint16_t xr1300_crc16_xmodem(const uint8_t *data, size_t len) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; i++) {
    crc ^= ((uint16_t)data[i] << 8);
    for (int b = 0; b < 8; b++) {
      if (crc & 0x8000) {
        crc = (uint16_t)((crc << 1) ^ 0x1021);
      } else {
        crc = (uint16_t)(crc << 1);
      }
    }
  }
  return crc;
}

static int hex_char_to_nibble(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static void xr1300_task(void *arg) {
  uint8_t line_buf[512];
  static char last_qr[64] = {0};
  static int64_t last_qr_time_ms = 0;
  static int64_t last_retrigger_ms = 0;

  while (1) {
    uint8_t ch = 0;
    // Block waiting for the first byte of a frame (0% CPU consumption)
    if (uart_read_bytes(XR1300_UART_NUM, &ch, 1, portMAX_DELAY) <= 0) {
      continue;
    }
    // Skip idle CR/LF bytes between frames
    if (ch == '\r' || ch == '\n') {
      continue;
    }

    int raw_len = 0;
    line_buf[raw_len++] = ch;
    bool line_overflow = false;

    // Gather a single frame terminated by CR/LF (prevents back-to-back scans from merging)
    while (1) {
      int r = uart_read_bytes(XR1300_UART_NUM, &ch, 1, pdMS_TO_TICKS(30));
      if (r <= 0) {
        break; // 30ms inter-byte silence -> frame complete
      }
      if (ch == '\r' || ch == '\n') {
        break; // End-of-frame terminator reached
      }
      if (raw_len < (int)sizeof(line_buf) - 1) {
        line_buf[raw_len++] = ch;
      } else {
        line_overflow = true;
      }
    }

    line_buf[raw_len] = '\0';
    xr1300_is_responsive = true;

    if (line_overflow) {
      ESP_LOGW("XR1300", "UART frame exceeded %d bytes, dropping oversized frame",
               (int)sizeof(line_buf) - 1);
      continue;
    }

    const uint8_t *frame_ptr = line_buf;
    int frame_len = raw_len;

    // Ignore/strip XR1300 command ACK/NACK tokens if concatenated without CRLF
    while ((frame_len >= 12) &&
           ((memcmp(frame_ptr, "$000100-6148", 12) == 0) ||
            (memcmp(frame_ptr, "$0001FF-A91F", 12) == 0))) {
      frame_ptr += 12;
      frame_len -= 12;
    }

    if (frame_len <= 0) {
      continue;
    }

    {
      int tail_start = (frame_len > 6) ? (frame_len - 6) : 0;
      char tail_hex[32] = {0};
      int th_pos = 0;
      for (int i = tail_start; i < frame_len && th_pos + 3 < (int)sizeof(tail_hex); i++) {
        th_pos += snprintf(tail_hex + th_pos, sizeof(tail_hex) - (size_t)th_pos,
                           (i == tail_start) ? "%02X" : " %02X", frame_ptr[i]);
      }
      ESP_LOGI("XR1300", "RAW Frame (%d bytes): '%.*s' | Tail HEX: [%s]",
               frame_len, frame_len, (const char *)frame_ptr, tail_hex);
    }

    // Ignore any other XR1300 command response frame (e.g. "$xxxxxx-yyyy")
    if (frame_len == 12 && frame_ptr[0] == '$' && frame_ptr[7] == '-') {
      continue;
    }

    // Verify hardware CRC16_XMODEM across the complete frame before truncation
    bool crc_valid = false;
    int payload_len = 0;

    // Check A: 4-char ASCII HEX CRC16 appended to data
    if (frame_len > 4) {
      int n0 = hex_char_to_nibble(frame_ptr[frame_len - 4]);
      int n1 = hex_char_to_nibble(frame_ptr[frame_len - 3]);
      int n2 = hex_char_to_nibble(frame_ptr[frame_len - 2]);
      int n3 = hex_char_to_nibble(frame_ptr[frame_len - 1]);
      if (n0 >= 0 && n1 >= 0 && n2 >= 0 && n3 >= 0) {
        uint16_t rx_crc = (uint16_t)((n0 << 12) | (n1 << 8) | (n2 << 4) | n3);
        uint16_t calc_crc = xr1300_crc16_xmodem(frame_ptr, (size_t)(frame_len - 4));
        if (calc_crc == rx_crc) {
          crc_valid = true;
          payload_len = frame_len - 4;
        }
      }
    }

    // Check B: 2-byte binary CRC16 (MSB-first or LSB-first) appended to data
    if (!crc_valid && frame_len > 2) {
      uint16_t calc_crc = xr1300_crc16_xmodem(frame_ptr, (size_t)(frame_len - 2));
      uint16_t rx_crc_be = ((uint16_t)frame_ptr[frame_len - 2] << 8) | frame_ptr[frame_len - 1];
      uint16_t rx_crc_le = ((uint16_t)frame_ptr[frame_len - 1] << 8) | frame_ptr[frame_len - 2];
      if (calc_crc == rx_crc_be || calc_crc == rx_crc_le) {
        crc_valid = true;
        payload_len = frame_len - 2;
      }
    }

    // Ensure every payload byte is printable ASCII (32..126)
    if (crc_valid) {
      for (int i = 0; i < payload_len; i++) {
        if (frame_ptr[i] < 32 || frame_ptr[i] > 126) {
          crc_valid = false;
          break;
        }
      }
    }

    if (!crc_valid) {
      ESP_LOGW("XR1300", "UART CRC16 check failed (len=%d, raw='%.*s'), dropping frame & re-triggering scan",
               frame_len, frame_len, (const char *)frame_ptr);
      int64_t err_now_ms = esp_timer_get_time() / 1000;
      if (err_now_ms - last_retrigger_ms >= 150) {
        const char *retrigger_cmd = "$108000-ADB0\r\n";
        uart_write_bytes(XR1300_UART_NUM, retrigger_cmd, strlen(retrigger_cmd));
        last_retrigger_ms = err_now_ms;
      }
      continue;
    }

    char clean_str[64] = {0};
    int c_len = (payload_len < (int)sizeof(clean_str) - 1) ? payload_len : ((int)sizeof(clean_str) - 1);
    memcpy(clean_str, frame_ptr, (size_t)c_len);
    clean_str[c_len] = '\0';

    ESP_LOGI("XR1300", "QR CRC16 verified OK (%d bytes): '%s'", c_len, clean_str);

    if (c_len > 0) {
      int64_t now_ms = esp_timer_get_time() / 1000;
      // Suppress rapid consecutive scans:
      // 1) Any QR code: 2500ms cooldown
      // 2) Identical QR code: 3000ms cooldown
      if (last_qr_time_ms > 0) {
        if (now_ms - last_qr_time_ms < 2500) {
          continue;
        }
        if (strcmp(last_qr, clean_str) == 0 && (now_ms - last_qr_time_ms < 3000)) {
          continue;
        }
      }

      strncpy(last_qr, clean_str, sizeof(last_qr) - 1);
      last_qr[sizeof(last_qr) - 1] = '\0';
      last_qr_time_ms = now_ms;

      xQueueSend(xr1300_event_queue, clean_str, 0);
    }
  }
}

static bool xr1300_send_cmd_and_check_ack(const char *name, const char *cmd, int wait_ms) {
  uart_flush_input(XR1300_UART_NUM);
  uart_write_bytes(XR1300_UART_NUM, cmd, strlen(cmd));

  uint8_t ack_buf[64] = {0};
  int total = 0;
  int first = uart_read_bytes(XR1300_UART_NUM, ack_buf, 1, pdMS_TO_TICKS(wait_ms));
  if (first > 0) {
    total = 1;
    while (total < (int)sizeof(ack_buf) - 1) {
      int r = uart_read_bytes(XR1300_UART_NUM, ack_buf + total,
                              sizeof(ack_buf) - 1 - total,
                              pdMS_TO_TICKS(20));
      if (r <= 0) break;
      total += r;
    }
  }
  while (total > 0 && (ack_buf[total - 1] == '\r' || ack_buf[total - 1] == '\n')) {
    ack_buf[--total] = '\0';
  }
  ack_buf[total] = '\0';

  bool ok = (strstr((const char *)ack_buf, "$000100-6148") != NULL);
  if (total == 0) {
    ESP_LOGW("XR1300", "CMD [%s] -> TIMEOUT (no ACK)", name);
  } else if (ok) {
    ESP_LOGI("XR1300", "CMD [%s] -> ACK OK: '%s'", name, (const char *)ack_buf);
  } else {
    ESP_LOGW("XR1300", "CMD [%s] -> RESP: '%s'", name, (const char *)ack_buf);
  }
  return ok;
}

bool xr1300_init(void) {
  if (uart_installed) {
    xr1300_is_responsive = true;
    return true;
  }

  const uart_config_t uart_config = {
      .baud_rate = 9600,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_APB,
  };

  // Install UART driver with direct ring buffer
  uart_driver_install(XR1300_UART_NUM, RX_BUF_SIZE, 0, 0, NULL, 0);

  uart_param_config(XR1300_UART_NUM, &uart_config);
  uart_set_pin(XR1300_UART_NUM, XR1300_TXD_PIN, XR1300_RXD_PIN,
               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_set_line_inverse(XR1300_UART_NUM, UART_SIGNAL_INV_DISABLE);

  // Critical for instant response:
  // Set RX full threshold to 1 so the very first incoming byte generates an interrupt immediately
  uart_set_rx_full_threshold(XR1300_UART_NUM, 1);
  uart_set_rx_timeout(XR1300_UART_NUM, 10);

  // 1. Factory reset causes XR1300 to automatically reboot (~1.2-1.5s)
  const char *factory_reset_cmd = "$010200-BF34\r\n";
  uart_write_bytes(XR1300_UART_NUM, factory_reset_cmd, strlen(factory_reset_cmd));
  ESP_LOGI("XR1300", "Factory reset sent, waiting 1800ms for XR1300 reboot...");
  vTaskDelay(pdMS_TO_TICKS(1800));
  uart_flush_input(XR1300_UART_NUM);

  // 2. Enable Host Command Response Mode 1 ($000100-6148 on ACK, $0001FF-A91F on NACK)
  xr1300_send_cmd_and_check_ack("RESP_MODE_1", "$020B01-0090\r\n", 150);

  // 3. Enable all 1D/2D barcode standards
  xr1300_send_cmd_and_check_ack("ALL_BARCODES", "$280101-3FD8\r\n", 150);

  // 4. Disable XR1300 internal power-up & read-beep 2.7kHz PWM outputs
  xr1300_send_cmd_and_check_ack("MUTE_PWR_BEEP", "$150000-EC9C\r\n", 150);
  xr1300_send_cmd_and_check_ack("MUTE_READ_BEEP", "$150100-9A28\r\n", 150);

  // 5. Minimize re-read intervals so a rejected CRC frame can be re-read immediately
  xr1300_send_cmd_and_check_ack("NO_SWEEP_INTV", "$100300-3441\r\n", 150);
  xr1300_send_cmd_and_check_ack("RESTART_0_3S", "$101304-52D4\r\n", 150);

  // 6. Add CRLF (0x0D, 0x0A) closing terminator
  xr1300_send_cmd_and_check_ack("TERM_CRLF", "$201003-881D\r\n", 150);

  // 7. Enable Hardware Data CRC16_XMODEM Checksum Output (Section 7.6, Page 58)
  s_crc16_active = xr1300_send_cmd_and_check_ack("ENABLE_CRC16", "$202301-9B71\r\n", 150);

  // 8. Save user settings to XR1300 NVM (with ACK verification)
  xr1300_send_cmd_and_check_ack("SAVE_SETTINGS", "$010207-26A3\r\n", 200);

  // 9. Disable runtime command ACK ($000100-6148) so $108000-ADB0 re-triggers never inject into QR data
  const char *resp_off_cmd = "$020B00-33A1\r\n";
  const char *save_cmd = "$010207-26A3\r\n";
  uart_write_bytes(XR1300_UART_NUM, resp_off_cmd, strlen(resp_off_cmd));
  vTaskDelay(pdMS_TO_TICKS(50));
  uart_write_bytes(XR1300_UART_NUM, save_cmd, strlen(save_cmd));
  vTaskDelay(pdMS_TO_TICKS(150));

  uart_flush_input(XR1300_UART_NUM);

  ESP_LOGI("XR1300", "Init at 9600 baud (TX=GPIO%d, RX=GPIO%d) - CRC16_ACK=%s",
           XR1300_TXD_PIN, XR1300_RXD_PIN, s_crc16_active ? "OK" : "NONE");

  xr1300_event_queue = xQueueCreate(1, 64);

  // Create real-time background task pinned to Core 0
  xTaskCreatePinnedToCore(xr1300_task, "xr1300_task", 4096, NULL, 5, &xr1300_task_handle, 0);

  uart_installed = true;
  xr1300_is_responsive = true;
  return true;
}

// Non-blocking poll from the queue
bool xr1300_read_qr(char *qr_string_out, uint16_t max_len) {
  if (!qr_string_out || max_len == 0 || xr1300_event_queue == NULL)
    return false;

  char temp[64];
  if (xQueueReceive(xr1300_event_queue, temp, 0) == pdTRUE) {
    strncpy(qr_string_out, temp, max_len - 1);
    qr_string_out[max_len - 1] = '\0';
    xQueueReset(xr1300_event_queue);
    return true;
  }

  return false;
}

#else

#include "xr1300.h"

bool xr1300_init(void) {
  return false;
}

bool xr1300_read_qr(char *qr_string_out, uint16_t max_len) {
  (void)qr_string_out;
  (void)max_len;
  return false;
}

#endif
