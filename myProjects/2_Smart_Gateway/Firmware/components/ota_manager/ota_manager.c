#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_rom_crc.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "crypto_manager.h"
#include "config_manager.h"
#include "ble_manager.h"
#include "ble_frame_parser.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_manager.h"

static const char *TAG = "OTA_MGR";

static ota_status_cb_t s_status_cb = NULL;
static ota_fw_status_cb_t s_fw_status_cb = NULL;
static volatile bool s_abort_requested = false;
static volatile uint32_t s_abort_request_id = 0;

// Default Onloi OTA Root Public Key for Ed25519 verification
static const uint8_t s_onloi_pubkey[32] = {
    0x4f, 0x4e, 0x4c, 0x4f, 0x49, 0x5f, 0x47, 0x41,
    0x54, 0x45, 0x57, 0x41, 0x59, 0x5f, 0x50, 0x55,
    0x42, 0x4b, 0x45, 0x59, 0x5f, 0x56, 0x31, 0x2e,
    0x32, 0x5f, 0x52, 0x4f, 0x4f, 0x54, 0x30, 0x31
};

void ota_manager_set_status_cb(ota_status_cb_t cb)
{
    s_status_cb = cb;
}

static void report_status(const char *status, const char *reason)
{
    if (s_status_cb != NULL) {
        s_status_cb(status, reason);
    }
}

void ota_manager_set_fw_status_cb(ota_fw_status_cb_t cb)
{
    s_fw_status_cb = cb;
}

static void report_fw_status(uint32_t request_id, uint8_t target, uint8_t state,
                             uint8_t pct, uint8_t err_code, uint32_t bytes_tx,
                             uint32_t reported_ver)
{
    if (s_fw_status_cb != NULL) {
        s_fw_status_cb(request_id, target, state, pct, err_code, bytes_tx, reported_ver);
    }
}

void ota_manager_abort(uint32_t request_id, uint8_t reason)
{
    ESP_LOGW(TAG, "OTA abort requested (reqId=%lu, reason=0x%02X)", (unsigned long)request_id, reason);
    s_abort_request_id = request_id;
    s_abort_requested = true;
}

bool ota_manager_is_aborted(uint32_t request_id)
{
    if (!s_abort_requested) return false;
    if (request_id == 0 || s_abort_request_id == 0 || s_abort_request_id == request_id) {
        return true;
    }
    return false;
}

void ota_manager_clear_abort(void)
{
    s_abort_requested = false;
    s_abort_request_id = 0;
}

static void ota_task(void *pvParameter)
{
    ota_update_params_t *params = (ota_update_params_t *)pvParameter;
    ESP_LOGI(TAG, "=== Starting Onloi v1.2 OTA Update ===");
    ESP_LOGI(TAG, "Target URL : %s", params->url);
    ESP_LOGI(TAG, "Version    : %s", params->version[0] ? params->version : "(unspecified)");
    ESP_LOGI(TAG, "SHA256     : %s", params->sha256[0] ? params->sha256 : "(unspecified)");
    ESP_LOGI(TAG, "Signature  : %s", params->signature[0] ? params->signature : "(none)");

    report_status("in_progress", NULL);

    esp_http_client_config_t http_config = {
        .url = params->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .keep_alive_enable = true,
        .timeout_ms = 15000,
    };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
#pragma GCC diagnostic pop

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK || ota_handle == NULL) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        report_status("failed", "download_error");
        free(params);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Connected to OTA server. Downloading and writing firmware...");
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA Download incomplete or error: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        report_status("failed", "download_error");
        free(params);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "OTA Download completed successfully.");

    // Step 2: Checksum Verification (Section 9 Step 2)
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition != NULL && strlen(params->sha256) > 0) {
        uint8_t partition_sha256[32] = {0};
        if (esp_partition_get_sha256(update_partition, partition_sha256) == ESP_OK) {
            char hex_sha256[65] = {0};
            for (int i = 0; i < 32; i++) {
                sprintf(hex_sha256 + (i * 2), "%02x", partition_sha256[i]);
            }
            ESP_LOGI(TAG, "Partition SHA-256 : %s", hex_sha256);
            ESP_LOGI(TAG, "Expected SHA-256  : %s", params->sha256);

            if (strcasecmp(hex_sha256, params->sha256) != 0) {
                ESP_LOGE(TAG, "Checksum mismatch! Aborting OTA.");
                esp_https_ota_abort(ota_handle);
                report_status("failed", "checksum_mismatch");
                free(params);
                vTaskDelete(NULL);
                return;
            }
            ESP_LOGI(TAG, "Checksum verification PASSED!");
        }
    }

    // Step 3: Signature Verification (Section 9 Step 3: Ed25519 on sha256 || version)
    if (strlen(params->signature) > 0) {
        char signed_msg[128] = {0};
        snprintf(signed_msg, sizeof(signed_msg), "%s%s", params->sha256, params->version);

        uint8_t sig_bytes[64] = {0};
        size_t sig_len = 0;
        int b64_ret = mbedtls_base64_decode(sig_bytes, sizeof(sig_bytes), &sig_len,
                                           (const unsigned char *)params->signature,
                                           strlen(params->signature));
        if (b64_ret == 0 && sig_len == 64) {
            if (!crypto_ed25519_verify(s_onloi_pubkey, (const uint8_t *)signed_msg, strlen(signed_msg), sig_bytes)) {
                ESP_LOGE(TAG, "Signature verification failed! Aborting OTA.");
                esp_https_ota_abort(ota_handle);
                report_status("failed", "signature_invalid");
                free(params);
                vTaskDelete(NULL);
                return;
            }
            ESP_LOGI(TAG, "Ed25519 Signature verification PASSED!");
        } else {
            ESP_LOGW(TAG, "Invalid base64 signature format or length (%d)", (int)sig_len);
        }
    }

    // Step 4: Finish OTA and switch boot partition
    err = esp_https_ota_finish(ota_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA Update successfully installed! Rebooting into new image in 2s...");
        report_status("success", NULL);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        report_status("failed", "download_error");
    }

    free(params);
    vTaskDelete(NULL);
}

esp_err_t ota_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA Manager...");

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGI(TAG, "First boot after OTA. Validating new firmware image...");
                esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Firmware marked as VALID. Rollback cancelled.");
                } else {
                    ESP_LOGE(TAG, "Failed to cancel rollback: %s", esp_err_to_name(ret));
                }
            }
        }
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Current firmware version: %s (project: %s)", app_desc->version, app_desc->project_name);

    return ESP_OK;
}

esp_err_t ota_manager_start_update(const ota_update_params_t *params)
{
    if (params == NULL || strlen(params->url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_update_params_t *copy = malloc(sizeof(ota_update_params_t));
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, params, sizeof(ota_update_params_t));

    BaseType_t ret = xTaskCreate(&ota_task, "ota_task", 8192, copy, 5, NULL);
    if (ret != pdPASS) {
        free(copy);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool mac_is_zero(const uint8_t *mac)
{
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) return false;
    }
    return true;
}

static void ota_v2_gw_self_update(const ota_v2_params_t *p)
{
    report_fw_status(p->request_id, p->firmware_target, FW_STATE_ACCEPTED, 0, 0, 0, 0);
    report_fw_status(p->request_id, p->firmware_target, FW_STATE_DOWNLOADING, 0, 0, 0, 0);

    esp_http_client_config_t http_config = {
        .url = p->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .keep_alive_enable = true,
        .timeout_ms = 15000,
    };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
#pragma GCC diagnostic pop

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK || ota_handle == NULL) {
        ESP_LOGE(TAG, "GW v2 OTA begin failed: %s", esp_err_to_name(err));
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_DOWNLOAD_FAILED, 0, 0);
        return;
    }

    int last_pct = -10;
    while (1) {
        if (ota_manager_is_aborted(p->request_id)) {
            esp_https_ota_abort(ota_handle);
            report_fw_status(p->request_id, p->firmware_target, FW_STATE_ABORTED, 0, 0, 0, 0);
            return;
        }
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int dl_bytes = esp_https_ota_get_image_len_read(ota_handle);
        if (p->plain_fw_length > 0) {
            int pct = (int)(((uint64_t)dl_bytes * 100ULL) / p->plain_fw_length);
            if (pct > 100) pct = 100;
            if (pct >= last_pct + 10) {
                last_pct = pct;
                report_fw_status(p->request_id, p->firmware_target, FW_STATE_DOWNLOADING, (uint8_t)pct, 0, (uint32_t)dl_bytes, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "GW v2 OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_DOWNLOAD_FAILED, 0, 0);
        return;
    }

    report_fw_status(p->request_id, p->firmware_target, FW_STATE_VERIFYING, 100, 0, p->plain_fw_length, 0);
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part != NULL) {
        uint8_t part_sha[32] = {0};
        if (esp_partition_get_sha256(update_part, part_sha) == ESP_OK) {
            if (memcmp(part_sha, p->plain_fw_sha256, 32) != 0) {
                ESP_LOGE(TAG, "GW v2 OTA SHA-256 mismatch!");
                esp_https_ota_abort(ota_handle);
                report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_CHECKSUM_MISMATCH, 0, 0);
                return;
            }
        }
    }

    report_fw_status(p->request_id, p->firmware_target, FW_STATE_ESP_APPLYING, 100, 0, p->plain_fw_length, 0);
    err = esp_https_ota_finish(ota_handle);
    if (err == ESP_OK) {
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_COMPLETED, 100, 0, p->plain_fw_length, p->fw_version);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } else {
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_DOWNLOAD_FAILED, 0, 0);
    }
}

static void ota_v2_sub_staging_update(const ota_v2_params_t *p)
{
    // 1. Resolve sub-device from NVS (subDeviceId first, then subMac)
    if (p->sub_device_id[0] == '\0' && mac_is_zero(p->sub_mac)) {
        ESP_LOGE(TAG, "Sub-device OTA rejected: both subDeviceId and subMac are empty");
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_SUB_ADDR_REQUIRED, 0, 0);
        return;
    }

    subkey_entry_t sk;
    memset(&sk, 0, sizeof(sk));
    esp_err_t sk_err = ESP_ERR_NOT_FOUND;
    if (p->sub_device_id[0] != '\0') {
        sk_err = subkey_get_by_device_id(p->sub_device_id, &sk);
    }
    if (sk_err != ESP_OK && !mac_is_zero(p->sub_mac)) {
        sk_err = subkey_get_by_mac(p->sub_mac, &sk);
    }
    if (sk_err != ESP_OK) {
        ESP_LOGE(TAG, "Sub-device '%s' not found in NVS subkey store", p->sub_device_id);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_SUB_NOT_FOUND, 0, 0);
        return;
    }

    const esp_partition_t *sub_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "sub_ota");
    if (sub_part == NULL) {
        ESP_LOGE(TAG, "Partition 'sub_ota' not found!");
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_INSUFFICIENT_STORAGE, 0, 0);
        return;
    }

    if (p->plain_fw_length == 0 || p->plain_fw_length > sub_part->size) {
        ESP_LOGE(TAG, "Invalid firmware length %lu (partition size %lu)",
                 (unsigned long)p->plain_fw_length, (unsigned long)sub_part->size);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_INSUFFICIENT_STORAGE, 0, 0);
        return;
    }

    report_fw_status(p->request_id, p->firmware_target, FW_STATE_ACCEPTED, 0, 0, 0, 0);
    report_fw_status(p->request_id, p->firmware_target, FW_STATE_DOWNLOADING, 0, 0, 0, 0);

    size_t erase_len = ((p->plain_fw_length + 4095U) / 4096U) * 4096U;
    ESP_LOGI(TAG, "Erasing sub_ota partition (%u bytes)...", (unsigned)erase_len);
    if (esp_partition_erase_range(sub_part, 0, erase_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase sub_ota partition");
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_INSUFFICIENT_STORAGE, 0, 0);
        return;
    }

    uint8_t *buf = malloc(4096);
    if (!buf) {
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_INSUFFICIENT_STORAGE, 0, 0);
        return;
    }

    esp_http_client_config_t http_cfg = {
        .url = p->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    if (!http) {
        free(buf);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_DOWNLOAD_FAILED, 0, 0);
        return;
    }

    esp_err_t err = esp_http_client_open(http, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http);
        free(buf);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_DOWNLOAD_FAILED, 0, 0);
        return;
    }

    int content_len = esp_http_client_fetch_headers(http);
    int status_code = esp_http_client_get_status_code(http);
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "HTTP status %d (content_len=%d)", status_code, content_len);
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        free(buf);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_DOWNLOAD_FAILED, 0, 0);
        return;
    }

    uint32_t offset = 0;
    int last_pct = 0;
    while (offset < p->plain_fw_length) {
        if (ota_manager_is_aborted(p->request_id)) {
            ESP_LOGW(TAG, "Sub-device staging download aborted");
            esp_http_client_close(http);
            esp_http_client_cleanup(http);
            free(buf);
            report_fw_status(p->request_id, p->firmware_target, FW_STATE_ABORTED, 0, 0, offset, 0);
            return;
        }
        size_t to_read = p->plain_fw_length - offset;
        if (to_read > 4096) to_read = 4096;
        int r = esp_http_client_read(http, (char *)buf, (int)to_read);
        if (r <= 0) {
            ESP_LOGE(TAG, "HTTP read error/EOF at offset %lu/%lu",
                     (unsigned long)offset, (unsigned long)p->plain_fw_length);
            break;
        }
        if (esp_partition_write(sub_part, offset, buf, (size_t)r) != ESP_OK) {
            ESP_LOGE(TAG, "Flash write error at offset %lu", (unsigned long)offset);
            offset = 0;
            break;
        }
        offset += (uint32_t)r;
        int pct = (int)(((uint64_t)offset * 100ULL) / p->plain_fw_length);
        if (pct >= last_pct + 10) {
            last_pct = pct;
            report_fw_status(p->request_id, p->firmware_target, FW_STATE_DOWNLOADING, (uint8_t)pct, 0, offset, 0);
        }
    }

    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    if (offset != p->plain_fw_length) {
        free(buf);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_DOWNLOAD_FAILED, offset, 0);
        return;
    }

    // Verify SHA-256 and CRC32 over staging flash
    report_fw_status(p->request_id, p->firmware_target, FW_STATE_VERIFYING, 100, 0, p->plain_fw_length, 0);

    psa_hash_operation_t sha_op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&sha_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(buf);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_CHECKSUM_MISMATCH, 0, 0);
        return;
    }
    uint32_t calc_crc = 0;

    for (uint32_t pos = 0; pos < p->plain_fw_length; pos += 4096) {
        size_t chunk = p->plain_fw_length - pos;
        if (chunk > 4096) chunk = 4096;
        if (esp_partition_read(sub_part, pos, buf, chunk) != ESP_OK) {
            psa_hash_abort(&sha_op);
            free(buf);
            report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_CHECKSUM_MISMATCH, pos, 0);
            return;
        }
        psa_hash_update(&sha_op, buf, chunk);
        calc_crc = esp_rom_crc32_le(calc_crc, buf, (uint32_t)chunk);
    }

    uint8_t calc_sha[32] = {0};
    size_t sha_len = 0;
    psa_hash_finish(&sha_op, calc_sha, sizeof(calc_sha), &sha_len);
    free(buf);

    if (memcmp(calc_sha, p->plain_fw_sha256, 32) != 0 || calc_crc != p->plain_fw_crc32) {
        ESP_LOGE(TAG, "Sub-device staging checksum mismatch! calcCRC=0x%08lX expCRC=0x%08lX",
                 (unsigned long)calc_crc, (unsigned long)p->plain_fw_crc32);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_CHECKSUM_MISMATCH, p->plain_fw_length, 0);
        return;
    }

    ESP_LOGI(TAG, "Sub-device staging image verified OK! Enqueueing BLE OTA transfer...");
    report_fw_status(p->request_id, p->firmware_target, FW_STATE_BLE_SUB_IMAGE_READY, 100, 0, p->plain_fw_length, 0);

    if (ble_cmd_queue == NULL) {
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_TIMEOUT, 0, 0);
        return;
    }

    ble_lock_cmd_t ble_cmd;
    memset(&ble_cmd, 0, sizeof(ble_cmd));
    ble_cmd.op_type = BLE_OP_SUB_OTA;
    memcpy(ble_cmd.mac, sk.mac, 6);
    ble_cmd.addr_type = sk.addr_type;
    strncpy(ble_cmd.target_device_id, sk.device_id, sizeof(ble_cmd.target_device_id) - 1);
    snprintf(ble_cmd.service_uuid, sizeof(ble_cmd.service_uuid), "%s", ONLOI_NUS_SVC_UUID);
    snprintf(ble_cmd.char_uuid, sizeof(ble_cmd.char_uuid), "%s", ONLOI_NUS_RX_UUID);
    snprintf(ble_cmd.tx_char_uuid, sizeof(ble_cmd.tx_char_uuid), "%s", ONLOI_NUS_TX_UUID);
    snprintf(ble_cmd.request_id, sizeof(ble_cmd.request_id), "%lu", (unsigned long)p->request_id);
    ble_cmd.timeout_ms = 15000;

    ble_cmd.sub_ota.ota_request_id = p->request_id;
    ble_cmd.sub_ota.firmware_target = p->firmware_target;
    ble_cmd.sub_ota.unit_target = (p->firmware_target == FW_TARGET_UNIT4_BLE_SUB_STM) ? 0x02 : 0x01;
    ble_cmd.sub_ota.product_id = p->product_id;
    ble_cmd.sub_ota.hw_rev = p->hw_rev;
    ble_cmd.sub_ota.fw_version = p->fw_version;
    ble_cmd.sub_ota.total_length = p->plain_fw_length;
    ble_cmd.sub_ota.plain_fw_crc32 = p->plain_fw_crc32;
    memcpy(ble_cmd.sub_ota.plain_fw_sha256, p->plain_fw_sha256, 32);

    if (xQueueSend(ble_cmd_queue, &ble_cmd, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to enqueue BLE_OP_SUB_OTA");
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_BUSY, 0, 0);
    }
}

static void ota_v2_task(void *pvParameter)
{
    ota_v2_params_t *p = (ota_v2_params_t *)pvParameter;
    ota_manager_clear_abort();

    if (p->firmware_target == FW_TARGET_UNIT1_ESP) {
        ota_v2_gw_self_update(p);
    } else if (p->firmware_target == FW_TARGET_UNIT3_BLE_SUB_ESP ||
               p->firmware_target == FW_TARGET_UNIT4_BLE_SUB_STM) {
        ota_v2_sub_staging_update(p);
    } else {
        ESP_LOGE(TAG, "Unsupported firmwareTarget: 0x%02X", p->firmware_target);
        report_fw_status(p->request_id, p->firmware_target, FW_STATE_FAILED, 0, BLE_STATUS_TARGET_INVALID, 0, 0);
    }

    free(p);
    vTaskDelete(NULL);
}

esp_err_t ota_manager_start_v2_update(const ota_v2_params_t *params)
{
    if (!params || strlen(params->url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_v2_params_t *copy = malloc(sizeof(ota_v2_params_t));
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, params, sizeof(ota_v2_params_t));

    BaseType_t ret = xTaskCreate(&ota_v2_task, "ota_v2_task", 8192, copy, 5, NULL);
    if (ret != pdPASS) {
        free(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}
