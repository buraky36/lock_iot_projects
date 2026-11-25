#ifndef OSDP_PD_H
#define OSDP_PD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize RS485 UART and OSDP Peripheral Device (PD) stack.
 *        Configures UART2 on PIN_RS485_TX (26), PIN_RS485_RX (34), PIN_RS485_DE (18).
 * @return ESP_OK on success.
 */
esp_err_t osdp_pd_init(void);

/**
 * @brief Notify OSDP ACU of a card read event (MFRC522 or Wiegand Input).
 * @param card_str String representation of card decimal or hex ID.
 */
void osdp_pd_notify_card(const char *card_str);

/**
 * @brief Notify OSDP ACU of a QR code scan event (XR1300 or Wiegand QR).
 * @param qr_str ASCII QR code string.
 */
void osdp_pd_notify_qr(const char *qr_str);

/**
 * @brief Notify OSDP ACU of a Keypad PIN entry event.
 * @param pin_str String representation of entered PIN.
 */
void osdp_pd_notify_pin(const char *pin_str);

/**
 * @brief Notify OSDP ACU of a single key press.
 * @param key ASCII character ('0'-'9', '*', '#').
 */
void osdp_pd_notify_key(char key);

/**
 * @brief Get active OSDP PD address (0x00 - 0x7E).
 */
uint8_t osdp_pd_get_address(void);

/**
 * @brief Set OSDP PD address and persist to NVS.
 */
void osdp_pd_set_address(uint8_t address);

/**
 * @brief Get active OSDP baudrate (default 9600).
 */
uint32_t osdp_pd_get_baudrate(void);

/**
 * @brief Check if OSDP ACU is actively polling this PD (within last 5 seconds).
 */
bool osdp_pd_is_online(void);

#ifdef __cplusplus
}
#endif

#endif // OSDP_PD_H
