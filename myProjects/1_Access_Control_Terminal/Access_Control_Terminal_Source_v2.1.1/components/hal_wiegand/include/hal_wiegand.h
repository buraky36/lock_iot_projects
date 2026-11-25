#ifndef HAL_WIEGAND_H
#define HAL_WIEGAND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WIEGAND_D0_PIN 22
#define WIEGAND_D1_PIN 23

// Initialize the Wiegand interface (sets GPIO pins to input mode)
void hal_wiegand_init(void);

// Read Wiegand data (Controller Mode)
bool hal_wiegand_available(void);
uint16_t hal_wiegand_peek_bit_count(void);
bool hal_wiegand_is_printable_string(void);
void hal_wiegand_flush(void);
uint32_t hal_wiegand_read(uint8_t *bit_len_out);
bool hal_wiegand_read_string(char *out_str, size_t max_len, uint16_t *bit_len_out);

// Diagnostic and debug helpers
uint32_t hal_wiegand_get_d0_pulses(void);
uint32_t hal_wiegand_get_d1_pulses(void);
int32_t hal_wiegand_get_last_d0_interval(void);
int32_t hal_wiegand_get_min_d0_interval(void);
int32_t hal_wiegand_get_max_d0_interval(void);
void hal_wiegand_print_intervals(void);
uint16_t hal_wiegand_get_current_bits(void);
uint16_t hal_wiegand_get_last_bit_count(void);
uint32_t hal_wiegand_get_last_card_id(void);
void hal_wiegand_reset_debug_counters(void);

#endif
