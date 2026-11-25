#include "hal_wiegand.h"
#include "hw_config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "HAL_WIEGAND";

// Forward declarations from hal_shift_reg
void hal_shift_reg_set_wiegand_en(bool state);
bool hal_shift_reg_get_wiegand_en(void);

#define MAX_WG_BITS 512
// Standard Wiegand pulses are spaced 1000us-2000us apart (min 80us for fast bursts).
// Noise, ringing or bouncing (<80us) is actively rejected.
#define MIN_WG_PULSE_INTERVAL_US 80
// Standard Wiegand inter-frame timeout (gap after last bit of card/QR).
#define WG_FRAME_TIMEOUT_US 25000

static portMUX_TYPE wg_spinlock = portMUX_INITIALIZER_UNLOCKED;

static volatile uint8_t wg_data_buf[MAX_WG_BITS / 8] = {0};
static volatile uint16_t wg_bit_count = 0;
static volatile int64_t wg_last_bit_time = 0;

static uint8_t wg_ready_data_buf[MAX_WG_BITS / 8] = {0};
static uint16_t wg_ready_bit_count = 0;
static volatile bool wg_available = false;

// Debug and Diagnostic Counters
static volatile uint32_t s_d0_pulses = 0;
static volatile uint32_t s_d1_pulses = 0;
static volatile int64_t s_last_d0_edge_time = 0;
static volatile int64_t s_last_d1_edge_time = 0;
static volatile int32_t s_last_d0_interval_us = 0;
static volatile int32_t s_min_d0_interval_us = 99999999;
static volatile int32_t s_max_d0_interval_us = 0;
static volatile int32_t s_d0_intervals[16] = {0};
static volatile uint8_t s_d0_interval_head = 0;
static volatile uint16_t s_last_bit_count = 0;
static volatile uint32_t s_last_card_id = 0;

static void IRAM_ATTR wiegand_d0_isr_handler(void* arg) {
    int64_t now = esp_timer_get_time();
    int32_t delta = (int32_t)(now - s_last_d0_edge_time);
    s_last_d0_edge_time = now;

    if (delta > 0 && delta < 10000000) {
        if (delta < s_min_d0_interval_us) s_min_d0_interval_us = delta;
        if (delta > s_max_d0_interval_us) s_max_d0_interval_us = delta;
        s_last_d0_interval_us = delta;
        s_d0_intervals[s_d0_interval_head & 0x0F] = delta;
        s_d0_interval_head++;
    }

    // Glitch filter: edges arriving faster than 80us are reflections/ringing
    if (delta < MIN_WG_PULSE_INTERVAL_US) {
        return;
    }

    s_d0_pulses++;
    if (wg_bit_count < MAX_WG_BITS) {
        wg_data_buf[wg_bit_count / 8] &= ~(1 << (7 - (wg_bit_count % 8)));
        wg_bit_count++;
    }
    wg_last_bit_time = now;
}

static void IRAM_ATTR wiegand_d1_isr_handler(void* arg) {
    int64_t now = esp_timer_get_time();
    int32_t delta = (int32_t)(now - s_last_d1_edge_time);
    s_last_d1_edge_time = now;

    // Glitch filter: edges arriving faster than 80us are reflections/ringing
    if (delta < MIN_WG_PULSE_INTERVAL_US) {
        return;
    }

    s_d1_pulses++;
    if (wg_bit_count < MAX_WG_BITS) {
        wg_data_buf[wg_bit_count / 8] |= (1 << (7 - (wg_bit_count % 8)));
        wg_bit_count++;
    }
    wg_last_bit_time = now;
}

void hal_wiegand_init(void) {
    ESP_LOGI(TAG, "Initializing Wiegand Controller HAL (D0: GPIO%d, D1: GPIO%d)...", PIN_WIEGAND_D0, PIN_WIEGAND_D1);

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "GPIO ISR service ready (%s)", esp_err_to_name(err));
    }
    
    // Enable Level Shifter (U11 TXS0102) via Shift Register U2 QF
    hal_shift_reg_set_wiegand_en(true);
    bool oe_en = hal_shift_reg_get_wiegand_en();
    ESP_LOGI(TAG, "Shift Register WIEGAND_EN state: %s", oe_en ? "ACTIVE (1)" : "INACTIVE (0)");

    // Standard GPIO configuration for input with internal pull-up and falling-edge interrupt
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_WIEGAND_D0) | (1ULL << PIN_WIEGAND_D1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t cfg_err = gpio_config(&io_conf);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(cfg_err));
    }
    
    memset((void*)wg_data_buf, 0, sizeof(wg_data_buf));
    wg_bit_count = 0;
    wg_available = false;
    
    esp_err_t err_d0 = gpio_isr_handler_add(PIN_WIEGAND_D0, wiegand_d0_isr_handler, NULL);
    esp_err_t err_d1 = gpio_isr_handler_add(PIN_WIEGAND_D1, wiegand_d1_isr_handler, NULL);
    
    int d0_lvl = gpio_get_level(PIN_WIEGAND_D0);
    int d1_lvl = gpio_get_level(PIN_WIEGAND_D1);
    
    ESP_LOGI(TAG, "D0 ISR add: %s, D1 ISR add: %s", esp_err_to_name(err_d0), esp_err_to_name(err_d1));
    ESP_LOGI(TAG, "Wiegand Line Levels -> D0=%d, D1=%d (Normal Idle = 1, 1)", d0_lvl, d1_lvl);
    if (d0_lvl == 0 || d1_lvl == 0) {
        ESP_LOGW(TAG, "WARNING: One or both lines are LOW at boot! Check wiring/pullup/level translator!");
    }
}

// Reverse the 8 bits of a byte (bit 0 <-> bit 7, bit 1 <-> bit 6, etc.)
// Used to decode Wiegand QR streams where characters are sent LSB-first
static inline uint8_t reverse8(uint8_t b) {
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

uint16_t hal_wiegand_peek_bit_count(void) {
    if (!wg_available) return 0;
    return wg_ready_bit_count;
}

bool hal_wiegand_is_printable_string(void) {
    if (!wg_available || wg_ready_bit_count < 16) return false;
    size_t byte_len = wg_ready_bit_count / 8;
    if (byte_len == 0) return false;

    size_t best_printable = 0;
    for (int rev = 0; rev <= 1; rev++) {
        for (int off = 0; off <= 1; off++) {
            size_t count = 0;
            size_t total = (wg_ready_bit_count >= off) ? (wg_ready_bit_count - off) / 8 : 0;
            if (total == 0) continue;
            for (size_t b = 0; b < total; b++) {
                uint8_t byte_val = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int bit_idx = off + (int)b * 8 + bit;
                    if (wg_ready_data_buf[bit_idx / 8] & (1 << (7 - (bit_idx % 8)))) {
                        byte_val |= (1 << (7 - bit));
                    }
                }
                if (rev) byte_val = reverse8(byte_val);
                byte_val &= 0x7F;
                if ((byte_val >= 0x20 && byte_val <= 0x7E) || byte_val == '\r' || byte_val == '\n' || byte_val == '\t') {
                    count++;
                }
            }
            if (count > best_printable) best_printable = count;
        }
    }
    // If at least 70% of bytes are printable, consider it a string/QR
    return (best_printable * 10 >= byte_len * 7);
}

void hal_wiegand_flush(void) {
    portENTER_CRITICAL(&wg_spinlock);
    wg_available = false;
    wg_ready_bit_count = 0;
    memset(wg_ready_data_buf, 0, sizeof(wg_ready_data_buf));
    portEXIT_CRITICAL(&wg_spinlock);
}

bool hal_wiegand_available(void) {
    if (wg_bit_count > 0) {
        int64_t now = esp_timer_get_time();
        // Frame timeout after last received pulse (standard Wiegand bit gap is 1-2ms).
        // 10ms cleanly finishes cards while wiping 50Hz (20ms) mains noise before next cycle.
        if ((now - wg_last_bit_time) > WG_FRAME_TIMEOUT_US) {
            portENTER_CRITICAL(&wg_spinlock);
            // Ignore incomplete fragments / noise bursts (< 24 bits)
            if (wg_bit_count < 24) {
                memset((void*)wg_data_buf, 0, sizeof(wg_data_buf));
                wg_bit_count = 0;
                portEXIT_CRITICAL(&wg_spinlock);
                return wg_available;
            }

            memcpy(wg_ready_data_buf, (const void*)wg_data_buf, sizeof(wg_data_buf));
            wg_ready_bit_count = wg_bit_count;
            memset((void*)wg_data_buf, 0, sizeof(wg_data_buf));
            wg_bit_count = 0;
            wg_available = true;
            portEXIT_CRITICAL(&wg_spinlock);
            
            s_last_bit_count = wg_ready_bit_count;

            if (wg_ready_bit_count <= 64) {
                uint64_t tmp = 0;
                for (int i = 0; i < wg_ready_bit_count; i++) {
                    tmp <<= 1;
                    if (wg_ready_data_buf[i / 8] & (1 << (7 - (i % 8)))) {
                        tmp |= 1;
                    }
                }
                ESP_LOGI(TAG, "Wiegand Packet Complete: Raw Data = 0x%08llX (Bits: %d, D0 Pulses: %lu, D1 Pulses: %lu)",
                         (unsigned long long)tmp, wg_ready_bit_count,
                         (unsigned long)s_d0_pulses, (unsigned long)s_d1_pulses);
            } else {
                ESP_LOGI(TAG, "Wiegand Packet Complete: Long Payload (Bits: %d, D0 Pulses: %lu, D1 Pulses: %lu)",
                         wg_ready_bit_count, (unsigned long)s_d0_pulses, (unsigned long)s_d1_pulses);
            }
        }
    }
    return wg_available;
}

uint32_t hal_wiegand_read(uint8_t *bits) {
    if (!wg_available) return 0;
    
    // Safety check: Any payload > 64 bits is NEVER an RFID card (it's QR / long payload).
    // Reject it immediately to prevent uint64_t overflow / undefined behavior.
    if (wg_ready_bit_count > 64) {
        ESP_LOGW(TAG, "Rejected RFID card read for oversized payload (%d bits > 64 bits)", wg_ready_bit_count);
        wg_available = false;
        if (bits) *bits = 0;
        return 0;
    }
    
    uint64_t tmp = 0;
    for (int i = 0; i < wg_ready_bit_count; i++) {
        tmp <<= 1;
        if (wg_ready_data_buf[i / 8] & (1 << (7 - (i % 8)))) {
            tmp |= 1;
        }
    }
    
    uint32_t card_id = 0;
    
    // Auto-strip parity if the length indicates standard formats
    if (wg_ready_bit_count == 26 || wg_ready_bit_count == 25) {
        card_id = (tmp >> 1) & 0xFFFFFF; 
    } else if (wg_ready_bit_count == 34 || wg_ready_bit_count == 33) {
        card_id = (tmp >> 1) & 0xFFFFFFFF;
    } else {
        card_id = tmp & 0xFFFFFFFF;
    }
    
    if (bits) *bits = (uint8_t)wg_ready_bit_count;
    wg_available = false;
    s_last_card_id = card_id;

    ESP_LOGI(TAG, "Wiegand Read: Card ID = %lu (Bits: %d)", (unsigned long)card_id, wg_ready_bit_count);
    
    return card_id;
}

bool hal_wiegand_read_string(char *out_str, size_t max_len, uint16_t *bit_len_out) {
    if (!wg_available || out_str == NULL || max_len == 0) return false;
    
    if (bit_len_out) *bit_len_out = wg_ready_bit_count;
    
    char best_str[128] = {0};
    int best_score = -1;

    // Search across LSB-first (rev=1) and MSB-first (rev=0), bit offsets 0..1
    for (int rev = 1; rev >= 0; rev--) {
        for (int off = 0; off <= 1; off++) {
            char cand[128] = {0};
            size_t c_idx = 0;
            size_t total = (wg_ready_bit_count >= off) ? (wg_ready_bit_count - off) / 8 : 0;
            if (total >= sizeof(cand)) total = sizeof(cand) - 1;

            int printable = 0;
            for (size_t b = 0; b < total && c_idx < sizeof(cand) - 1; b++) {
                uint8_t byte_val = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int bit_idx = off + (int)b * 8 + bit;
                    if (wg_ready_data_buf[bit_idx / 8] & (1 << (7 - (bit_idx % 8)))) {
                        byte_val |= (1 << (7 - bit));
                    }
                }
                if (rev) byte_val = reverse8(byte_val);
                byte_val &= 0x7F; // 7-bit ASCII
                if ((byte_val >= 0x20 && byte_val <= 0x7E) || byte_val == '\r' || byte_val == '\n' || byte_val == '\t') {
                    cand[c_idx++] = (char)byte_val;
                    printable++;
                }
            }
            cand[c_idx] = '\0';
            while (c_idx > 0 && (cand[c_idx - 1] == '\r' || cand[c_idx - 1] == '\n' || cand[c_idx - 1] == ' ')) {
                cand[--c_idx] = '\0';
            }

            int score = printable * 10;
            if (c_idx >= 3 && strcmp(cand + c_idx - 3, ",QR") == 0) {
                score += 1000; // Strong match for Dynamic QR pattern
            }
            if (score > best_score) {
                best_score = score;
                strncpy(best_str, cand, sizeof(best_str) - 1);
                best_str[sizeof(best_str) - 1] = '\0';
            }
        }
    }

    if (best_score <= 0) {
        wg_available = false;
        out_str[0] = '\0';
        return false;
    }

    strncpy(out_str, best_str, max_len - 1);
    out_str[max_len - 1] = '\0';
    size_t c_idx = strlen(out_str);

    // Dynamic QR Glitch Corrector (recovers bit-6 noise: s -> 3, X -> ,, p -> 0, @ -> ' ')
    // Format: <id>,<YYYY-MM-DD HH:MM:SS.mmm>,QR
    if (c_idx >= 30 && strcmp(out_str + c_idx - 3, ",QR") == 0) {
        char orig[128];
        strncpy(orig, out_str, sizeof(orig) - 1);
        orig[sizeof(orig) - 1] = '\0';
        bool modified = false;

        // 1. Sanitize ID part (indices 0..10): should be digits '0'..'9'
        // If characters in 0x70..0x79 ('p'..'y') appear due to bit-6 noise, map to '0'..'9' (c -= 0x40)
        for (size_t i = 0; i < 11 && i < c_idx; i++) {
            uint8_t ch = (uint8_t)out_str[i];
            if (ch >= 0x70 && ch <= 0x79) {
                out_str[i] = (char)(ch - 0x40);
                modified = true;
            }
        }

        // 2. Comma separator after ID: index 11 (or before date)
        if (c_idx > 11) {
            if (out_str[11] == 'X' || out_str[11] == 'l' || out_str[11] == 0x58 || out_str[11] == 0x6C) {
                out_str[11] = ',';
                modified = true;
            }
        }

        // 3. Date & time part: index 12 to c_idx - 3
        // Format: YYYY-MM-DD HH:MM:SS.mmm
        for (size_t i = 12; i < c_idx - 3; i++) {
            uint8_t ch = (uint8_t)out_str[i];
            // Space between date and time: '@' (0x40) or '`' (0x60) -> ' ' (0x20)
            if (ch == '@' || ch == '`') {
                out_str[i] = ' ';
                modified = true;
            } else if (ch >= 0x70 && ch <= 0x79) {
                // Digits turned into 'p'..'y'
                out_str[i] = (char)(ch - 0x40);
                modified = true;
            }
        }

        if (modified) {
            ESP_LOGW(TAG, "Wiegand QR bit-glitch corrected: '%s' -> '%s'", orig, out_str);
        }
    }
    
    wg_available = false;
    return (c_idx > 0);
}

// Diagnostic getters
uint32_t hal_wiegand_get_d0_pulses(void) {
    return s_d0_pulses;
}

uint32_t hal_wiegand_get_d1_pulses(void) {
    return s_d1_pulses;
}

int32_t hal_wiegand_get_last_d0_interval(void) {
    return s_last_d0_interval_us;
}

int32_t hal_wiegand_get_min_d0_interval(void) {
    return (s_min_d0_interval_us == 99999999) ? 0 : s_min_d0_interval_us;
}

int32_t hal_wiegand_get_max_d0_interval(void) {
    return s_max_d0_interval_us;
}

void hal_wiegand_print_intervals(void) {
    printf("Last 16 D0 intervals (us): [");
    uint8_t h = s_d0_interval_head;
    for (int i = 0; i < 16; i++) {
        uint8_t idx = (h - 16 + i) & 0x0F;
        printf("%ld%s", (long)s_d0_intervals[idx], (i < 15) ? ", " : "");
    }
    printf("]\n");
}

uint16_t hal_wiegand_get_current_bits(void) {
    return wg_bit_count;
}

uint16_t hal_wiegand_get_last_bit_count(void) {
    return s_last_bit_count;
}

uint32_t hal_wiegand_get_last_card_id(void) {
    return s_last_card_id;
}

void hal_wiegand_reset_debug_counters(void) {
    portENTER_CRITICAL(&wg_spinlock);
    s_d0_pulses = 0;
    s_d1_pulses = 0;
    s_last_d0_edge_time = 0;
    s_last_d1_edge_time = 0;
    s_last_d0_interval_us = 0;
    s_min_d0_interval_us = 99999999;
    s_max_d0_interval_us = 0;
    s_d0_interval_head = 0;
    memset((void*)s_d0_intervals, 0, sizeof(s_d0_intervals));
    s_last_bit_count = 0;
    s_last_card_id = 0;
    wg_bit_count = 0;
    memset((void*)wg_data_buf, 0, sizeof(wg_data_buf));
    portEXIT_CRITICAL(&wg_spinlock);
}
