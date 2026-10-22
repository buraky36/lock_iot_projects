#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CMD_NFC_ENROLL_START 0x44
#define CMD_NFC_ENROLL_READY 0x22
#define CMD_NFC_ENROLLED     0x23
#define CMD_REMOTE_OPEN      0x40
#define CMD_REMOTE_CLOSE     0x4C
#define CMD_OFFLINE_LOG_BATCH 0x35
#define CMD_LOCAL_FACTORY_RESET 0x61
#define CMD_OTA_START        0x60

// Onloi Protocol v2.18 OTA Commands
#define CMD_FIRMWARE_UPDATE_REQUEST 0x38
#define CMD_FIRMWARE_STATUS         0x39
#define CMD_FIRMWARE_INFO           0x3A
#define CMD_FIRMWARE_UPDATE         0x53
#define CMD_FIRMWARE_ABORT          0x5A
#define CMD_GET_FIRMWARE_INFO       0x5C
#define CMD_SERVER_DISCONNECT       0x59
#define CMD_WIFI_RESET              0x58
#define CMD_FACTORY_RESET           0x50

// Initialize API Client
void api_client_init(void);

// Reload cloud configuration from NVS and start MQTT if online
void api_client_reload_config(void);

// To be called periodically from main loop
void api_client_tick(void);

// Sends a "pass" event to the Onloi API via MQTT.
int api_client_send_pass_event(const char *credential);
void api_client_send_nfc_enroll_ready_event(void);
void api_client_send_lock_opened_event(void);
void api_client_send_lock_closed_event(void);
void api_client_reset_claim_status(void);
void api_client_send_offline_logs(void);
void api_client_send_local_factory_reset_event(void);

// OTA Event functions (Onloi Protocol v2.18)
void api_client_send_firmware_status(uint32_t requestId, uint8_t target, uint8_t state, uint8_t progressPct, uint8_t errorCode, uint32_t bytesTransferred, uint32_t reportedVersion);
void api_client_send_firmware_info(uint32_t requestId, uint8_t count);
void api_client_send_firmware_update_request(void);

// Sends an NFC enrolled event (0x23)
void api_client_send_nfc_enrolled_event(uint32_t card_id);

// Sends an NFC enroll ready event (0x22)
void api_client_send_nfc_enroll_ready_event(void);

// Triggers a time sync request via MQTT.
int api_client_send_get_time(void);

/**
 * @brief Send Lock Opened event to MQTT backend
 */
void api_client_send_lock_opened_event(void);

/**
 * @brief Reset MQTT claim status and clear from NVS. Used when a new Provision token arrives over BLE.
 */
void api_client_reset_claim_status(void);

#endif
