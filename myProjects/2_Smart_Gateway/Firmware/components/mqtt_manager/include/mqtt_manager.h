#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize the MQTT client configuration
 * @param uri MQTT broker URI
 * @param port MQTT broker Port
 * @param device_id ID of this gateway device
 * @param client_id MQTT clientId from the HTTPS claim response
 * @param username MQTT username from the HTTPS claim response
 * @param password MQTT password from the HTTPS claim response
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mqtt_manager_init(const char *uri, uint16_t port,
                            const char *device_id, const char *client_id,
                            const char *username, const char *password,
                            uint16_t session_epoch);

/**
 * @brief Start the MQTT client connection and publishing tasks
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mqtt_manager_start(void);

/**
 * @brief Stop MQTT client
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mqtt_manager_stop(void);

/**
 * @brief Check if MQTT is connected
 * @return true if connected
 */
bool mqtt_manager_is_connected(void);

/**
 * @brief Publish a notification from a BLE device
 */
void mqtt_manager_publish_notification(const uint8_t *mac, const uint8_t *data,
                                       size_t data_len);

/**
 * @brief Publish a notification with requestId and targetDeviceId
 */
void mqtt_manager_publish_notification_ext(const uint8_t *mac,
                                           const char *request_id,
                                           const char *target_device_id,
                                           const uint8_t *data,
                                           size_t data_len);

/**
 * @brief Publish OTA update status
 */
void mqtt_manager_publish_ota_status(const char *status, const char *reason);

/**
 * @brief Publish claim ack to /gateway/{deviceId}/device_claim/ack
 */
void mqtt_manager_publish_claim_ack(const char *request_id, const char *mac,
                                    const char *device_id,
                                    const char *device_code,
                                    const char *local_key_b64, int key_version,
                                    const char *result, const char *error_msg);

#endif // MQTT_MANAGER_H
