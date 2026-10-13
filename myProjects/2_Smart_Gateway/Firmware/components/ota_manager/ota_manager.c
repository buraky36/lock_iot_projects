#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "mbedtls/base64.h"
#include "crypto_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_manager.h"

static const char *TAG = "OTA_MGR";

static ota_status_cb_t s_status_cb = NULL;

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
