#include "osdp_pd.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "hw_config.h"
#include "hal_io.h"
#include "nv_storage.h"
#include "app_state_machine.h"

static const char *TAG = "OSDP_PD";

#define OSDP_UART_NUM         UART_NUM_2
#define OSDP_BUF_SIZE         512
#define OSDP_SOM              0x53
#define OSDP_BROADCAST_ADDR   0x7F

// OSDP Command Codes (from ACU)
#define CMD_POLL              0x60
#define CMD_ID                0x61
#define CMD_CAP               0x62
#define CMD_DIAG              0x63
#define CMD_LSTAT             0x64
#define CMD_ISTAT             0x65
#define CMD_OSTAT             0x66
#define CMD_RSTAT             0x67
#define CMD_OUT               0x68
#define CMD_LED               0x69
#define CMD_BUZ               0x6A
#define CMD_TEXT              0x6B
#define CMD_COMSET            0x6D

// OSDP Reply Codes (from PD)
#define REPLY_ACK             0x40
#define REPLY_NAK             0x41
#define REPLY_PDID            0x45
#define REPLY_PDCAP           0x46
#define REPLY_LSTATR          0x48
#define REPLY_ISTATR          0x4A
#define REPLY_OSTATR          0x49
#define REPLY_RAW             0x50
#define REPLY_FMT             0x51
#define REPLY_KEYPAD          0x53
#define REPLY_COM             0x54

// OSDP NAK Error Codes
#define NAK_BAD_CHECKSUM      0x01
#define NAK_BAD_COMMAND       0x02
#define NAK_UNKNOWN_CMD       0x03
#define NAK_BAD_SEQ           0x04

typedef enum {
    EVENT_CARD,
    EVENT_QR,
    EVENT_PIN,
    EVENT_KEY
} osdp_event_type_t;

typedef struct {
    osdp_event_type_t type;
    char data[64];
} osdp_event_t;

static osdp_config_t s_cfg = {
    .address = 0x00,
    .baudrate = 9600,
    .enabled = true
};

static QueueHandle_t s_event_queue = NULL;
static TaskHandle_t s_pd_task_handle = NULL;
static int64_t s_last_poll_ms = 0;
static bool s_initialized = false;

// OSDP Checksum: two's complement of the 8-bit sum of preceding bytes
static uint8_t osdp_calc_checksum(const uint8_t *buf, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += buf[i];
    }
    return (uint8_t)(0x100 - sum);
}

// OSDP CRC16: ITU-T / CCITT CRC-16 polynomial 0x1021, init 0x1D0F
static uint16_t osdp_calc_crc16(const uint8_t *buf, size_t len) {
    uint16_t crc = 0x1D0F;
    for (size_t i = 0; i < len; i++) {
        crc = (uint8_t)(crc >> 8) | (crc << 8);
        crc ^= buf[i];
        crc ^= (uint8_t)(crc & 0xFF) >> 4;
        crc ^= (crc << 12);
        crc ^= ((crc & 0xFF) << 5);
    }
    return crc;
}

// Send OSDP packet to RS485 UART
static void osdp_send_reply(uint8_t seq, bool use_crc, uint8_t reply_code, const uint8_t *payload, size_t payload_len) {
    uint8_t tx_buf[256];
    size_t total_len = 5 + 1 + payload_len + (use_crc ? 2 : 1);

    tx_buf[0] = OSDP_SOM;
    tx_buf[1] = 0x80 | (s_cfg.address & 0x7F);
    tx_buf[2] = (uint8_t)(total_len & 0xFF);
    tx_buf[3] = (uint8_t)((total_len >> 8) & 0xFF);
    tx_buf[4] = (uint8_t)((use_crc ? 0x04 : 0x00) | (seq & 0x03));
    tx_buf[5] = reply_code;

    if (payload && payload_len > 0) {
        memcpy(&tx_buf[6], payload, payload_len);
    }

    if (use_crc) {
        uint16_t crc = osdp_calc_crc16(tx_buf, 6 + payload_len);
        tx_buf[6 + payload_len] = (uint8_t)(crc & 0xFF);
        tx_buf[7 + payload_len] = (uint8_t)((crc >> 8) & 0xFF);
    } else {
        tx_buf[6 + payload_len] = osdp_calc_checksum(tx_buf, 6 + payload_len);
    }

    uart_write_bytes(OSDP_UART_NUM, (const char *)tx_buf, total_len);
    uart_wait_tx_done(OSDP_UART_NUM, pdMS_TO_TICKS(50));
}

static void osdp_send_nak(uint8_t seq, bool use_crc, uint8_t nak_code) {
    uint8_t payload[1] = { nak_code };
    osdp_send_reply(seq, use_crc, REPLY_NAK, payload, 1);
}

static void osdp_send_ack(uint8_t seq, bool use_crc) {
    osdp_send_reply(seq, use_crc, REPLY_ACK, NULL, 0);
}

// Handle incoming validated OSDP command
static void osdp_handle_command(uint8_t cmd, const uint8_t *payload, size_t payload_len, uint8_t seq, bool use_crc) {
    s_last_poll_ms = esp_timer_get_time() / 1000;

    switch (cmd) {
        case CMD_POLL: {
            osdp_event_t evt;
            if (s_event_queue && xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
                if (evt.type == EVENT_CARD) {
                    // Try parsing decimal card number for RAW 32-bit format
                    char *endp = NULL;
                    unsigned long card_num = strtoul(evt.data, &endp, 10);
                    if (card_num > 0 && endp != evt.data && *endp == '\0') {
                        uint8_t raw_data[8];
                        raw_data[0] = 0; // Reader 0
                        raw_data[1] = 1; // Format: Raw bit array / Wiegand
                        raw_data[2] = 32; // 32 bits LSB
                        raw_data[3] = 0;  // 32 bits MSB
                        raw_data[4] = (uint8_t)((card_num >> 24) & 0xFF);
                        raw_data[5] = (uint8_t)((card_num >> 16) & 0xFF);
                        raw_data[6] = (uint8_t)((card_num >> 8) & 0xFF);
                        raw_data[7] = (uint8_t)(card_num & 0xFF);
                        osdp_send_reply(seq, use_crc, REPLY_RAW, raw_data, 8);
                    } else {
                        // Send ASCII Card string via osdp_FMT
                        size_t clen = strlen(evt.data);
                        if (clen > 32) clen = 32;
                        uint8_t fmt_data[36];
                        fmt_data[0] = 0; // Reader 0
                        fmt_data[1] = 0; // Direction: In
                        fmt_data[2] = (uint8_t)clen;
                        memcpy(&fmt_data[3], evt.data, clen);
                        osdp_send_reply(seq, use_crc, REPLY_FMT, fmt_data, 3 + clen);
                    }
                } else if (evt.type == EVENT_QR) {
                    // Send QR code via osdp_FMT
                    size_t qlen = strlen(evt.data);
                    if (qlen > 60) qlen = 60;
                    uint8_t fmt_data[64];
                    fmt_data[0] = 0; // Reader 0
                    fmt_data[1] = 0; // Direction: In
                    fmt_data[2] = (uint8_t)qlen;
                    memcpy(&fmt_data[3], evt.data, qlen);
                    osdp_send_reply(seq, use_crc, REPLY_FMT, fmt_data, 3 + qlen);
                } else if (evt.type == EVENT_KEY || evt.type == EVENT_PIN) {
                    // Send Keypad data via osdp_KEYPAD
                    size_t klen = strlen(evt.data);
                    if (klen > 32) klen = 32;
                    uint8_t kp_data[36];
                    kp_data[0] = 0; // Reader 0
                    kp_data[1] = (uint8_t)klen;
                    memcpy(&kp_data[2], evt.data, klen);
                    osdp_send_reply(seq, use_crc, REPLY_KEYPAD, kp_data, 2 + klen);
                } else {
                    osdp_send_ack(seq, use_crc);
                }
            } else {
                osdp_send_ack(seq, use_crc);
            }
            break;
        }

        case CMD_ID: {
            // Reply with osdp_PDID (12 bytes)
            uint8_t pdid[12];
            pdid[0] = 0x00; // Vendor IEEE OUI
            pdid[1] = 0x1A;
            pdid[2] = 0xEF;
            pdid[3] = (uint8_t)(app_get_product_id() & 0xFF);
            pdid[4] = app_get_hardware_revision();

            // Serial number (from MAC address)
            uint8_t mac[6] = {0};
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            pdid[5] = mac[2];
            pdid[6] = mac[3];
            pdid[7] = mac[4];
            pdid[8] = mac[5];

            // Firmware version: 2.0.9
            pdid[9] = 2;  // Major
            pdid[10] = 0; // Minor
            pdid[11] = 9; // Build
            osdp_send_reply(seq, use_crc, REPLY_PDID, pdid, sizeof(pdid));
            break;
        }

        case CMD_CAP: {
            // Reply with osdp_PDCAP (Capabilities)
            // Each cap entry is 3 bytes: [Function Code, Compliance Level, Number of Items]
            const uint8_t caps[] = {
                0x01, 0x01, 0x02, // Contact Inputs: 2 inputs (Exit button, IN2)
                0x02, 0x01, 0x01, // Output Control: 1 relay
                0x03, 0x02, 0x01, // Card Data Format: 1 reader, RAW & Formatted
                0x04, 0x02, 0x01, // LED Control: 1 tri-color LED
                0x05, 0x02, 0x01, // Audible: 1 buzzer
                0x06, 0x00, 0x00, // Text output: none
                0x07, 0x00, 0x00, // Time keeping: none
                0x08, 0x01, 0x00, // Checksum support: CRC16 compliant
                0x09, 0x00, 0x00  // Secure Channel: none (plaintext)
            };
            osdp_send_reply(seq, use_crc, REPLY_PDCAP, caps, sizeof(caps));
            break;
        }

        case CMD_OUT: {
            // osdp_OUT: payload format: [out_number, ctrl_code, timer_lsb, timer_msb]
            if (payload_len >= 4) {
                uint8_t out_num = payload[0];
                uint8_t ctrl_code = payload[1];
                if (out_num == 0) {
                    if (ctrl_code == 0x02 || ctrl_code == 0x05) { // Permanent On or Timed On
                        ESP_LOGI(TAG, "OSDP CMD_OUT: Door Open triggered by ACU");
                        app_trigger_door_open();
                    } else if (ctrl_code == 0x01) { // Permanent Off
                        hal_io_relay_set(false);
                    }
                }
            }
            osdp_send_ack(seq, use_crc);
            break;
        }

        case CMD_LED: {
            // osdp_LED: payload format:
            // [reader, led_num, temp_ctrl, temp_on_100ms, temp_off_100ms, temp_on_color, temp_off_color, timer_lsb, timer_msb, perm_ctrl, perm_on_100ms, perm_off_100ms, perm_on_color, perm_off_color]
            if (payload_len >= 6) {
                uint8_t temp_color = payload[5]; // 1=black, 2=red, 3=green, 4=amber, 5=blue
                if (temp_color == 2) {
                    hal_io_led_set(LED_COLOR_RED, LED_MODE_SOLID);
                } else if (temp_color == 3) {
                    hal_io_led_set(LED_COLOR_GREEN, LED_MODE_SOLID);
                } else if (temp_color == 4) {
                    hal_io_led_set(LED_COLOR_YELLOW, LED_MODE_SOLID);
                } else if (temp_color == 5) {
                    hal_io_led_set(LED_COLOR_BLUE, LED_MODE_SOLID);
                } else if (temp_color == 1) {
                    hal_io_led_set(LED_COLOR_OFF, LED_MODE_SOLID);
                }
            }
            osdp_send_ack(seq, use_crc);
            break;
        }

        case CMD_BUZ: {
            // osdp_BUZ: [reader, tone_code, on_100ms, off_100ms, count]
            if (payload_len >= 5) {
                uint8_t tone = payload[1];
                uint8_t on_time = payload[2];
                uint8_t count = payload[4];
                if (tone != 0x01) { // 1 = off
                    uint16_t dur_ms = (on_time > 0) ? (on_time * 100) : 100;
                    uint8_t reps = (count > 0) ? count : 1;
                    hal_io_buzzer_beep(2700, dur_ms, reps);
                }
            }
            osdp_send_ack(seq, use_crc);
            break;
        }

        case CMD_COMSET: {
            // osdp_COMSET: [new_addr, baud_b0, baud_b1, baud_b2, baud_b3]
            if (payload_len >= 5) {
                uint8_t new_addr = payload[0];
                uint32_t new_baud = payload[1] | (payload[2] << 8) | (payload[3] << 16) | (payload[4] << 24);

                if (new_addr <= 0x7E && (new_baud == 9600 || new_baud == 19200 || new_baud == 38400 ||
                                         new_baud == 57600 || new_baud == 115200)) {
                    s_cfg.address = new_addr;
                    s_cfg.baudrate = new_baud;
                    nv_storage_set_osdp_config(&s_cfg);

                    uint8_t resp_payload[5];
                    resp_payload[0] = new_addr;
                    resp_payload[1] = (uint8_t)(new_baud & 0xFF);
                    resp_payload[2] = (uint8_t)((new_baud >> 8) & 0xFF);
                    resp_payload[3] = (uint8_t)((new_baud >> 16) & 0xFF);
                    resp_payload[4] = (uint8_t)((new_baud >> 24) & 0xFF);
                    osdp_send_reply(seq, use_crc, REPLY_COM, resp_payload, 5);

                    // Reconfigure UART baudrate
                    uart_set_baudrate(OSDP_UART_NUM, new_baud);
                    ESP_LOGI(TAG, "OSDP COMSET applied: Addr=0x%02X, Baud=%lu", new_addr, (unsigned long)new_baud);
                    return;
                }
            }
            osdp_send_nak(seq, use_crc, NAK_BAD_COMMAND);
            break;
        }

        case CMD_LSTAT: {
            uint8_t lstat[2] = { 0, 0 }; // [0] = tamper (0 = normal), [1] = power (0 = normal)
            osdp_send_reply(seq, use_crc, REPLY_LSTATR, lstat, sizeof(lstat));
            break;
        }

        case CMD_ISTAT: {
            // Inputs: Input 0 = Exit Button, Input 1 = IN2
            uint8_t istat[2];
            istat[0] = hal_io_read_exit_button() ? 1 : 0;
            istat[1] = (hal_io_read_in2_raw() == 0) ? 1 : 0;
            osdp_send_reply(seq, use_crc, REPLY_ISTATR, istat, sizeof(istat));
            break;
        }

        case CMD_OSTAT: {
            // Outputs: Output 0 = Relay
            uint8_t ostat[1];
            ostat[0] = 0; // Status reported as off by default
            osdp_send_reply(seq, use_crc, REPLY_OSTATR, ostat, sizeof(ostat));
            break;
        }

        default:
            // Unsupported / Unknown command
            osdp_send_nak(seq, use_crc, NAK_UNKNOWN_CMD);
            break;
    }
}

// OSDP RX and protocol processing task
static void osdp_pd_task(void *pvParameters) {
    uint8_t rx_buf[OSDP_BUF_SIZE];

    ESP_LOGI(TAG, "OSDP PD Task started (Addr: 0x%02X, Baud: %lu, Half-Duplex RS485)",
             s_cfg.address, (unsigned long)s_cfg.baudrate);

    while (1) {
        // Wait for SOM (0x53)
        uint8_t som = 0;
        int len = uart_read_bytes(OSDP_UART_NUM, &som, 1, pdMS_TO_TICKS(100));
        if (len <= 0 || som != OSDP_SOM) {
            continue;
        }

        // Read ADDR, LEN_LSB, LEN_MSB, CTRL (4 bytes)
        uint8_t hdr[4];
        len = uart_read_bytes(OSDP_UART_NUM, hdr, 4, pdMS_TO_TICKS(50));
        if (len < 4) {
            continue;
        }

        uint8_t target_addr = hdr[0] & 0x7F;
        uint16_t packet_len = (uint16_t)(hdr[1] | (hdr[2] << 8));
        uint8_t ctrl = hdr[3];

        // Validate basic packet bounds
        if (packet_len < 6 || packet_len > OSDP_BUF_SIZE) {
            continue;
        }

        // Assemble full frame in rx_buf
        rx_buf[0] = OSDP_SOM;
        rx_buf[1] = hdr[0];
        rx_buf[2] = hdr[1];
        rx_buf[3] = hdr[2];
        rx_buf[4] = hdr[3];

        // Read remaining packet bytes (packet_len - 5 bytes)
        size_t rem = packet_len - 5;
        len = uart_read_bytes(OSDP_UART_NUM, &rx_buf[5], rem, pdMS_TO_TICKS(50));
        if (len < (int)rem) {
            continue;
        }

        // Check if address matches our PD address or broadcast
        if (target_addr != s_cfg.address && target_addr != OSDP_BROADCAST_ADDR) {
            continue; // Not addressed to us
        }

        bool use_crc = (ctrl & 0x04) != 0;
        uint8_t seq = ctrl & 0x03;

        // Verify checksum / CRC
        if (use_crc) {
            if (packet_len < 7) continue;
            uint16_t rx_crc = (uint16_t)(rx_buf[packet_len - 2] | (rx_buf[packet_len - 1] << 8));
            uint16_t calc_crc = osdp_calc_crc16(rx_buf, packet_len - 2);
            if (rx_crc != calc_crc) {
                osdp_send_nak(seq, use_crc, NAK_BAD_CHECKSUM);
                continue;
            }
        } else {
            uint8_t sum = 0;
            for (size_t i = 0; i < packet_len; i++) {
                sum += rx_buf[i];
            }
            if (sum != 0) {
                osdp_send_nak(seq, use_crc, NAK_BAD_CHECKSUM);
                continue;
            }
        }

        // Command byte is at index 5
        uint8_t cmd = rx_buf[5];
        size_t payload_len = (use_crc ? (packet_len - 2) : (packet_len - 1)) - 6;
        const uint8_t *payload = (payload_len > 0) ? &rx_buf[6] : NULL;

        osdp_handle_command(cmd, payload, payload_len, seq, use_crc);
    }
}

esp_err_t osdp_pd_init(void) {
    if (s_initialized) return ESP_OK;

    nv_storage_get_osdp_config(&s_cfg);
    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "OSDP PD is disabled in configuration.");
        return ESP_OK;
    }

    const uart_config_t uart_config = {
        .baud_rate = (int)s_cfg.baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = uart_driver_install(OSDP_UART_NUM, OSDP_BUF_SIZE * 2, OSDP_BUF_SIZE * 2, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    uart_param_config(OSDP_UART_NUM, &uart_config);
    uart_set_pin(OSDP_UART_NUM, PIN_RS485_TX, PIN_RS485_RX, PIN_RS485_DE, UART_PIN_NO_CHANGE);
    uart_set_mode(OSDP_UART_NUM, UART_MODE_RS485_HALF_DUPLEX);
    uart_set_rx_timeout(OSDP_UART_NUM, 3);

    s_event_queue = xQueueCreate(16, sizeof(osdp_event_t));
    xTaskCreatePinnedToCore(osdp_pd_task, "osdp_pd_task", 4096, NULL, 6, &s_pd_task_handle, 1);

    s_initialized = true;
    ESP_LOGI(TAG, "OSDP PD Initialized (TX=GPIO%d, RX=GPIO%d, RTS=GPIO%d, Addr=0x%02X, Baud=%lu)",
             PIN_RS485_TX, PIN_RS485_RX, PIN_RS485_DE, s_cfg.address, (unsigned long)s_cfg.baudrate);

    return ESP_OK;
}

void osdp_pd_notify_card(const char *card_str) {
    if (!s_event_queue || !card_str) return;
    osdp_event_t evt;
    evt.type = EVENT_CARD;
    strncpy(evt.data, card_str, sizeof(evt.data) - 1);
    evt.data[sizeof(evt.data) - 1] = '\0';
    xQueueSend(s_event_queue, &evt, 0);
}

void osdp_pd_notify_qr(const char *qr_str) {
    if (!s_event_queue || !qr_str) return;
    osdp_event_t evt;
    evt.type = EVENT_QR;
    strncpy(evt.data, qr_str, sizeof(evt.data) - 1);
    evt.data[sizeof(evt.data) - 1] = '\0';
    xQueueSend(s_event_queue, &evt, 0);
}

void osdp_pd_notify_pin(const char *pin_str) {
    if (!s_event_queue || !pin_str) return;
    osdp_event_t evt;
    evt.type = EVENT_PIN;
    strncpy(evt.data, pin_str, sizeof(evt.data) - 1);
    evt.data[sizeof(evt.data) - 1] = '\0';
    xQueueSend(s_event_queue, &evt, 0);
}

void osdp_pd_notify_key(char key) {
    if (!s_event_queue) return;
    osdp_event_t evt;
    evt.type = EVENT_KEY;
    evt.data[0] = key;
    evt.data[1] = '\0';
    xQueueSend(s_event_queue, &evt, 0);
}

uint8_t osdp_pd_get_address(void) {
    return s_cfg.address;
}

void osdp_pd_set_address(uint8_t address) {
    s_cfg.address = address & 0x7E;
    nv_storage_set_osdp_config(&s_cfg);
}

uint32_t osdp_pd_get_baudrate(void) {
    return s_cfg.baudrate;
}

bool osdp_pd_is_online(void) {
    if (s_last_poll_ms == 0) return false;
    int64_t now_ms = esp_timer_get_time() / 1000;
    return (now_ms - s_last_poll_ms < 5000);
}
