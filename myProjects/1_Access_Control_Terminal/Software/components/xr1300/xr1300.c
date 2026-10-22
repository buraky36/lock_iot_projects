#include "xr1300.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "hw_config.h"
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
volatile bool xr1300_is_responsive = false;

static void xr1300_task(void *arg) {
  uint8_t raw_buf[256];
  static char last_qr[64] = {0};
  static int64_t last_qr_time_ms = 0;

  while (1) {
    // Wait for the very first byte (blocks indefinitely with 0% CPU consumption)
    int len = uart_read_bytes(XR1300_UART_NUM, raw_buf, 1, portMAX_DELAY);
    if (len > 0) {
      int total_len = 1;
      // Gather the rest of the barcode burst with a 30ms inter-byte silence timeout
      while (total_len < (int)sizeof(raw_buf) - 1) {
        int r = uart_read_bytes(XR1300_UART_NUM, raw_buf + total_len,
                                sizeof(raw_buf) - 1 - total_len,
                                pdMS_TO_TICKS(30));
        if (r <= 0) break; // Silence for 30ms -> burst is complete
        total_len += r;
      }
      raw_buf[total_len] = '\0';
      xr1300_is_responsive = true;

      // Clean trailing CR, LF, spaces
      while (total_len > 0 && (raw_buf[total_len - 1] == '\r' || raw_buf[total_len - 1] == '\n' || raw_buf[total_len - 1] == ' ')) {
        raw_buf[--total_len] = '\0';
      }

      // Filter printable ASCII characters
      char clean_str[64] = {0};
      int c_len = 0;
      for (int i = 0; i < total_len && c_len < (int)sizeof(clean_str) - 1; i++) {
        if (raw_buf[i] >= 32 && raw_buf[i] <= 126) {
          clean_str[c_len++] = (char)raw_buf[i];
        }
      }
      clean_str[c_len] = '\0';

      if (c_len > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        // Suppress identical scan repetition within 1.5 seconds so camera doesn't flood server
        if (strcmp(last_qr, clean_str) == 0 && (now_ms - last_qr_time_ms < 1500)) {
          continue;
        }

        strncpy(last_qr, clean_str, sizeof(last_qr) - 1);
        last_qr_time_ms = now_ms;

        printf("[QR XR1300] RAW Rx (%d bytes): %s\n", c_len, clean_str);
        xQueueSend(xr1300_event_queue, clean_str, 0);
      }
    }
  }
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

  // Send factory reset command to XR1300 once to clear any previously configured delays
  const char *factory_reset_cmd = "$010200-BF34\r\n";
  uart_write_bytes(XR1300_UART_NUM, factory_reset_cmd, strlen(factory_reset_cmd));
  vTaskDelay(pdMS_TO_TICKS(200));
  uart_flush_input(XR1300_UART_NUM);

  printf("[QR XR1300] Init at 9600 baud (TX=GPIO%d, RX=GPIO%d) - Fast burst mode active\n",
         XR1300_TXD_PIN, XR1300_RXD_PIN);

  xr1300_event_queue = xQueueCreate(5, 64);

  // Create real-time background task pinned to Core 0
  xTaskCreatePinnedToCore(xr1300_task, "xr1300_task", 4096, NULL, 5, &xr1300_task_handle, 0);

  uart_installed = true;
  xr1300_is_responsive = true;
  return true;
}

// Non-blocking poll from the queue
bool xr1300_read_qr(char *qr_string_out, uint16_t max_len) {
  if (!qr_string_out || max_len == 0)
    return false;

  char temp[64];
  if (xQueueReceive(xr1300_event_queue, temp, 0) == pdTRUE) {
    strncpy(qr_string_out, temp, max_len - 1);
    qr_string_out[max_len - 1] = '\0';
    return true;
  }

  return false;
}
