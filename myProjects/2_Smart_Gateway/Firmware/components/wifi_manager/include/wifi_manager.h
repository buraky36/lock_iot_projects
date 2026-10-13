#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

/**
 * @brief Initialize Wi-Fi netif and default event handlers
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Scan available Wi-Fi access points
 * @param ap_records Buffer to store AP records
 * @param count Pointer to number of records (in: max records, out: found records)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_scan_ssids(wifi_ap_record_t *ap_records, uint16_t *count);

/**
 * @brief Start Wi-Fi in Station (STA) mode and connect to AP
 * @param ssid Wi-Fi SSID
 * @param password Wi-Fi Password
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_start_sta(const char *ssid, const char *password);

/**
 * @brief Start Wi-Fi in Access Point (AP) mode for provisioning
 * @param ap_ssid SSID for the Gateway's AP
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_start_ap(const char *ap_ssid);

/**
 * @brief Stop Wi-Fi driver and free resources
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief Check if Wi-Fi is connected in STA mode and has got an IP address
 * @return true if connected with IP, false otherwise
 */
bool wifi_manager_is_connected(void);

#endif // WIFI_MANAGER_H
