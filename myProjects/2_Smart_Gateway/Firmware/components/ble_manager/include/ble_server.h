#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the BLE GATT Server for Onboarding
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_server_init(void);

/**
 * @brief Start Advertising the Onboarding Service
 */
void ble_server_start_adv(void);

/**
 * @brief Stop Advertising the Onboarding Service
 */
void ble_server_stop_adv(void);

/**
 * @brief Set claimed state and update advertising payload
 */
void ble_server_set_claimed(bool claimed);

/**
 * @brief Check if gateway is claimed
 */
bool ble_server_is_claimed(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_SERVER_H
