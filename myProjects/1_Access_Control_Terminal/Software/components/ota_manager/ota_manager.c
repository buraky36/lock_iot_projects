#include "ota_manager.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "app_state_machine.h"

static const char *TAG = "OTA_MGR";

static ota_status_cb_t s_status_cb = NULL;
static bool s_ota_running = false;
static bool s_abort_requested = false;
static uint32_t s_active_request_id = 0;

// Standard IEEE 802.3 CRC32 lookup table
static uint32_t s_crc32_table[256];
static bool s_crc32_table_initialized = false;

static void init_crc32_table(void) {
    if (s_crc32_table_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320L ^ (c >> 1)) : (c >> 1);
        }
        s_crc32_table[i] = c;
    }
    s_crc32_table_initialized = true;
}

static uint32_t update_crc32(uint32_t crc, const uint8_t *data, size_t len) {
    if (!s_crc32_table_initialized) init_crc32_table();
    for (size_t i = 0; i < len; i++) {
        crc = s_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void report_status(uint32_t req_id, uint8_t target, uint8_t state,
                          uint8_t progress_pct, uint8_t error_code,
                          uint32_t bytes_transferred, uint32_t reported_version) {
    ESP_LOGI(TAG, "OTA State -> 0x%02X (%s), Pct: %d%%, Err: 0x%02X, Bytes: %lu",
             state,
             (state == OTA_STATE_ACCEPTED) ? "ACCEPTED" :
             (state == OTA_STATE_DOWNLOADING) ? "DOWNLOADING" :
             (state == OTA_STATE_VERIFYING) ? "VERIFYING" :
             (state == OTA_STATE_DOWNLOAD_VERIFIED) ? "DOWNLOAD_VERIFIED" :
             (state == OTA_STATE_ESP_APPLYING) ? "ESP_APPLYING" :
             (state == OTA_STATE_COMPLETED) ? "COMPLETED" :
             (state == OTA_STATE_ABORTED) ? "ABORTED" : "FAILED",
             progress_pct, error_code, (unsigned long)bytes_transferred);

    if (s_status_cb) {
        s_status_cb(req_id, target, state, progress_pct, error_code, bytes_transferred, reported_version);
    }
}

void ota_manager_set_status_cb(ota_status_cb_t cb) {
    s_status_cb = cb;
}

bool ota_manager_is_running(void) {
    return s_ota_running;
}

bool ota_manager_abort(uint32_t request_id, uint8_t reason) {
    if (!s_ota_running) {
        ESP_LOGW(TAG, "FIRMWARE_ABORT received, but no OTA task running.");
        report_status(request_id, OTA_TARGET_UNIT1_ESP, OTA_STATE_ABORTED, 0, OTA_ERR_NONE, 0, 0);
        return true;
    }

    if (s_active_request_id != 0 && s_active_request_id != request_id) {
        ESP_LOGW(TAG, "FIRMWARE_ABORT request_id mismatch (active: 0x%08lX, requested: 0x%08lX)",
                 (unsigned long)s_active_request_id, (unsigned long)request_id);
    }

    ESP_LOGI(TAG, "Signaling OTA task to abort (reason: 0x%02X)...", reason);
    s_abort_requested = true;
    return true;
}

static void ota_task(void *pvParameter) {
    ota_update_params_t *params = (ota_update_params_t *)pvParameter;
    s_active_request_id = params->request_id;
    s_abort_requested = false;

    ESP_LOGI(TAG, "=== Starting Onloi OTA Update ===");
    ESP_LOGI(TAG, "Request ID : 0x%08lX", (unsigned long)params->request_id);
    ESP_LOGI(TAG, "Target     : 0x%02X", params->firmware_target);
    ESP_LOGI(TAG, "Version    : 0x%08lX", (unsigned long)params->firmware_version);
    ESP_LOGI(TAG, "Length     : %lu bytes", (unsigned long)params->plain_fw_length);
    ESP_LOGI(TAG, "Expected CRC: 0x%08lX (has: %d)", (unsigned long)params->plain_fw_crc32, params->has_crc32);
    ESP_LOGI(TAG, "URL        : %s", params->url);

    app_set_state(STATE_OTA_UPDATING);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "Passive OTA partition not found!");
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 0, OTA_ERR_FLASH_WRITE, 0, 0);
        goto cleanup_exit;
    }

    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx (size %lu)",
             update_partition->subtype, (unsigned long)update_partition->address, (unsigned long)update_partition->size);

    if (params->plain_fw_length > update_partition->size) {
        ESP_LOGE(TAG, "Firmware size (%lu) exceeds partition size (%lu)!",
                 (unsigned long)params->plain_fw_length, (unsigned long)update_partition->size);
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 0, OTA_ERR_IMAGE_TOO_LARGE, 0, 0);
        goto cleanup_exit;
    }

    esp_http_client_config_t http_config = {
        .url = params->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 0, OTA_ERR_NETWORK, 0, 0);
        goto cleanup_exit;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 0, OTA_ERR_NETWORK, 0, 0);
        goto cleanup_exit;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status: %d, Content-Length: %d", status_code, content_length);

    if (status_code != 200 && status_code != 206) {
        ESP_LOGE(TAG, "Server returned HTTP status %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 0, OTA_ERR_NETWORK, 0, 0);
        goto cleanup_exit;
    }

    esp_ota_handle_t ota_handle = 0;
    size_t ota_size = (params->plain_fw_length > 0) ? params->plain_fw_length : OTA_SIZE_UNKNOWN;
    err = esp_ota_begin(update_partition, ota_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 0, OTA_ERR_FLASH_WRITE, 0, 0);
        goto cleanup_exit;
    }

    // Initialize running SHA-256 and CRC32
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0); // 0 = SHA-256
    uint32_t running_crc32 = 0xFFFFFFFF;

    uint8_t *buffer = malloc(4096);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate 4KB OTA chunk buffer");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        mbedtls_sha256_free(&sha_ctx);
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 0, OTA_ERR_FLASH_WRITE, 0, 0);
        goto cleanup_exit;
    }

    uint32_t bytes_transferred = 0;
    uint8_t last_reported_pct = 0;
    int64_t last_report_time = esp_timer_get_time() / 1000;
    uint32_t total_expected = (params->plain_fw_length > 0) ? params->plain_fw_length :
                              ((content_length > 0) ? (uint32_t)content_length : 0);

    // Initial DOWNLOADING status at 0%
    report_status(params->request_id, params->firmware_target, OTA_STATE_DOWNLOADING, 0, OTA_ERR_NONE, 0, params->firmware_version);

    bool download_success = false;

    while (1) {
        if (s_abort_requested) {
            ESP_LOGW(TAG, "OTA Abort detected during download!");
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            mbedtls_sha256_free(&sha_ctx);
            free(buffer);
            app_set_state(STATE_IDLE);
            report_status(params->request_id, params->firmware_target, OTA_STATE_ABORTED, last_reported_pct, OTA_ERR_NONE, bytes_transferred, 0);
            goto cleanup_exit;
        }

        int data_read = esp_http_client_read(client, (char *)buffer, 4096);
        if (data_read < 0) {
            ESP_LOGE(TAG, "HTTP read error: %d", data_read);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            mbedtls_sha256_free(&sha_ctx);
            free(buffer);
            report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, last_reported_pct, OTA_ERR_NETWORK, bytes_transferred, 0);
            goto cleanup_exit;
        } else if (data_read == 0) {
            // End of stream
            ESP_LOGI(TAG, "HTTP download finished. Total bytes read: %lu", (unsigned long)bytes_transferred);
            download_success = true;
            break;
        }

        // Write chunk to flash
        err = esp_ota_write(ota_handle, buffer, data_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            mbedtls_sha256_free(&sha_ctx);
            free(buffer);
            report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, last_reported_pct, OTA_ERR_FLASH_WRITE, bytes_transferred, 0);
            goto cleanup_exit;
        }

        // Update running checksums
        mbedtls_sha256_update(&sha_ctx, buffer, data_read);
        running_crc32 = update_crc32(running_crc32, buffer, data_read);

        bytes_transferred += data_read;

        // Calculate progress percentage
        uint8_t pct = (total_expected > 0) ? (uint8_t)((bytes_transferred * 100ULL) / total_expected) : 0;
        if (pct > 100) pct = 100;

        int64_t now = esp_timer_get_time() / 1000;
        if (pct >= last_reported_pct + 5 || (now - last_report_time > 2000 && pct != last_reported_pct)) {
            last_reported_pct = pct;
            last_report_time = now;
            report_status(params->request_id, params->firmware_target, OTA_STATE_DOWNLOADING, pct, OTA_ERR_NONE, bytes_transferred, params->firmware_version);
        }
    }

    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!download_success) {
        esp_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, last_reported_pct, OTA_ERR_NETWORK, bytes_transferred, 0);
        goto cleanup_exit;
    }

    if (params->plain_fw_length > 0 && bytes_transferred != params->plain_fw_length) {
        ESP_LOGE(TAG, "Size mismatch: Transferred %lu != Expected %lu",
                 (unsigned long)bytes_transferred, (unsigned long)params->plain_fw_length);
        esp_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 100, OTA_ERR_NETWORK, bytes_transferred, 0);
        goto cleanup_exit;
    }

    // Step 47: State VERIFYING (0x03)
    report_status(params->request_id, params->firmware_target, OTA_STATE_VERIFYING, 100, OTA_ERR_NONE, bytes_transferred, params->firmware_version);

    // Verify SHA-256
    uint8_t calculated_sha[32];
    mbedtls_sha256_finish(&sha_ctx, calculated_sha);
    mbedtls_sha256_free(&sha_ctx);

    if (params->has_sha256) {
        if (memcmp(calculated_sha, params->plain_fw_sha256, 32) != 0) {
            ESP_LOGE(TAG, "SHA-256 Digest verification FAILED!");
            esp_ota_abort(ota_handle);
            report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 100, OTA_ERR_SHA256_MISMATCH, bytes_transferred, 0);
            goto cleanup_exit;
        }
        ESP_LOGI(TAG, "SHA-256 verification PASSED!");
    }

    // Verify CRC32
    uint32_t final_crc32 = running_crc32 ^ 0xFFFFFFFF;
    if (params->has_crc32) {
        if (final_crc32 != params->plain_fw_crc32) {
            ESP_LOGE(TAG, "CRC-32 verification FAILED! Calc: 0x%08lX, Expected: 0x%08lX",
                     (unsigned long)final_crc32, (unsigned long)params->plain_fw_crc32);
            esp_ota_abort(ota_handle);
            report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 100, OTA_ERR_CRC32_MISMATCH, bytes_transferred, 0);
            goto cleanup_exit;
        }
        ESP_LOGI(TAG, "CRC-32 verification PASSED! (0x%08lX)", (unsigned long)final_crc32);
    }

    // Step 47: State DOWNLOAD_VERIFIED (0x04)
    report_status(params->request_id, params->firmware_target, OTA_STATE_DOWNLOAD_VERIFIED, 100, OTA_ERR_NONE, bytes_transferred, params->firmware_version);

    // Finalize flash partition write
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 100, OTA_ERR_FLASH_WRITE, bytes_transferred, 0);
        goto cleanup_exit;
    }

    // Step 48: Self-Apply — State ESP_APPLYING (0x10)
    report_status(params->request_id, params->firmware_target, OTA_STATE_ESP_APPLYING, 100, OTA_ERR_NONE, bytes_transferred, params->firmware_version);

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        report_status(params->request_id, params->firmware_target, OTA_STATE_FAILED, 100, OTA_ERR_FLASH_WRITE, bytes_transferred, 0);
        goto cleanup_exit;
    }

    // Step 47: State COMPLETED (0x06)
    report_status(params->request_id, params->firmware_target, OTA_STATE_COMPLETED, 100, OTA_ERR_NONE, bytes_transferred, params->firmware_version);

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " OTA UPDATE SUCCESSFUL! Rebooting in 2 seconds...");
    ESP_LOGI(TAG, "=================================================");

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

cleanup_exit:
    app_set_state(STATE_IDLE);
    if (params->url) {
        free(params->url);
    }
    free(params);
    s_ota_running = false;
    s_active_request_id = 0;
    vTaskDelete(NULL);
}

bool ota_manager_start_update(const ota_update_params_t *params) {
    if (!params || !params->url || strlen(params->url) == 0) return false;
    if (s_ota_running) {
        ESP_LOGW(TAG, "OTA already in progress, rejecting start_update");
        return false;
    }

    ota_update_params_t *task_params = malloc(sizeof(ota_update_params_t));
    if (!task_params) return false;
    memcpy(task_params, params, sizeof(ota_update_params_t));

    s_ota_running = true;
    BaseType_t ret = xTaskCreate(ota_task, "ota_task", 10240, (void *)task_params, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ota_task");
        if (task_params->url) free(task_params->url);
        free(task_params);
        s_ota_running = false;
        return false;
    }

    return true;
}

bool ota_manager_start(const char *url) {
    if (!url || strlen(url) == 0) return false;

    ota_update_params_t params = {
        .request_id = 1,
        .firmware_target = OTA_TARGET_UNIT1_ESP,
        .firmware_version = 0,
        .plain_fw_length = 0,
        .has_sha256 = false,
        .has_crc32 = false,
        .url = strdup(url),
    };
    return ota_manager_start_update(&params);
}
