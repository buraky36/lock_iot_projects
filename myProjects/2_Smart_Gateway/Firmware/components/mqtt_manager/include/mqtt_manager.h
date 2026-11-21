#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t mac[6];
    char target_device_id[32];
    char request_id[64];
    const uint8_t *gw_eph_pub;   // 32 bytes
    const uint8_t *dev_eph_pub;  // 32 bytes
    const uint8_t *conn_nonce;   // 8 bytes
    const uint8_t *frame_data;
    size_t frame_len;
    const uint8_t *dec_frame_data; // Decrypted plaintext v2.19 Onloi frame (optional)
    size_t dec_frame_len;
    uint8_t cmd_code;              // Parsed v2.19 CMD code
    bool has_cmd;
    const char *result;          // "ok" or "error"
    const char *reason;          // NULL or error reason string
} mqtt_notify_relay_t;

/**
 * @brief Initialize the MQTT client configuration
 */
esp_err_t mqtt_manager_init(const char *uri, uint16_t port,
                            const char *device_id, const char *client_id,
                            const char *username, const char *password,
                            uint16_t session_epoch);

/**
 * @brief Start the MQTT client connection and publishing tasks
 */
esp_err_t mqtt_manager_start(void);

/**
 * @brief Stop MQTT client
 */
esp_err_t mqtt_manager_stop(void);

/**
 * @brief Check if MQTT is connected
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
 * @brief Publish notify relay adhering to Section 7.2
 */
void mqtt_manager_publish_notify_relay(const mqtt_notify_relay_t *relay);

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

/**
 * @brief Publish unclaim ack to /gateway/{deviceId}/device_unclaim/ack
 */
void mqtt_manager_publish_unclaim_ack(const char *request_id,
                                      const char *target_device_id,
                                      const char *result,
                                      const char *reason);

/**
 * @brief Publish key rotate ack to /gateway/{deviceId}/key_rotate/ack
 */
void mqtt_manager_publish_keyrot_ack(const char *request_id,
                                     const char *target_device_id,
                                     const char *new_local_key_b64,
                                     int key_version,
                                     const char *result,
                                     const char *reason);

/**
 * @brief Publish command ack / error to /gateway/{deviceId}/command/ack (or notify)
 */
void mqtt_manager_publish_command_ack(const char *request_id,
                                      const char *target_device_id,
                                      const uint8_t *mac,
                                      const char *result,
                                      const char *reason);

/**
 * @brief Publish binary protocol event frame to onloi/v1/d/{deviceId}/event
 */
void mqtt_manager_publish_proto_event(const uint8_t *frame_data, size_t frame_len);

/**
 * @brief Publish FIRMWARE_STATUS (0x39) binary EVENT frame to onloi/v1/d/{deviceId}/event
 */
void mqtt_manager_publish_firmware_status(uint32_t request_id, uint8_t firmware_target,
                                          uint8_t state, uint8_t progress_pct,
                                          uint8_t error_code, uint32_t bytes_transferred,
                                          uint32_t reported_version);

/**
 * @brief Request subkey from server for deviceId via MQTT (/gateway/{id}/subkey_request)
 * @param device_id Device ID string
 * @param timeout_ms Timeout to wait for subkey_response
 * @return esp_err_t ESP_OK if received and stored in NVS, ESP_ERR_TIMEOUT otherwise
 */
esp_err_t mqtt_manager_request_subkey(const char *device_id, uint32_t timeout_ms);

#endif // MQTT_MANAGER_H
