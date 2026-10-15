#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config_manager.h"
#include "gateway_version.h"
#include "wifi_manager.h"
#include "ble_manager.h"
#include "ble_server.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>
#include "api_manager.h"
#include "mbedtls/base64.h"

// Define to 1 to use DevKit RGB LED, 0 to use Production Discrete LEDs
#define CONFIG_USE_RGB_LED 1

#if CONFIG_USE_RGB_LED
#include "led_strip.h"
#define BLINK_GPIO 8
static led_strip_handle_t led_strip;
#endif

static const char *TAG = "CORE_MGR";

uint8_t g_provisioning_token[32] = {0};

// GPIO Configs based on C6 Guide
#define LED_WIFI      GPIO_NUM_7   // Red LED
#define LED_BLE       GPIO_NUM_6   // Blue LED
#define KEY_SW        GPIO_NUM_9   // DevKit BOOT Button

typedef enum {
    SYS_STATE_UNCONFIGURED = 0,
    SYS_STATE_STA_CONNECTING,
    SYS_STATE_STA_CONNECTED,
    SYS_STATE_STA_FAILED
} system_state_t;

static system_state_t s_sys_state = SYS_STATE_UNCONFIGURED;

// GPIO Initialization
static void init_gpios(void)
{
    ESP_LOGI(TAG, "Initializing Status LEDs and Configuration Button...");
    
#if CONFIG_USE_RGB_LED
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1, // at least one LED on board
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
#else
    // LEDs config
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_WIFI) | (1ULL << LED_BLE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    // Default LEDs off
    gpio_set_level(LED_WIFI, 0);
    gpio_set_level(LED_BLE, 0);
#endif

    // Button config
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << KEY_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Internal pullup
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_conf);
}

// LED Status blink task
static void led_indicator_task(void *pvParameters)
{
    while (1) {
        switch (s_sys_state) {
            case SYS_STATE_UNCONFIGURED:
#if CONFIG_USE_RGB_LED
                led_strip_set_pixel(led_strip, 0, 0, 0, 50); // Blue
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
                led_strip_clear(led_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
#else
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(LED_BLE, 0);
#endif
                break;
                
            case SYS_STATE_STA_CONNECTING:
#if CONFIG_USE_RGB_LED
                led_strip_set_pixel(led_strip, 0, 50, 50, 0); // Yellow
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(200));
                led_strip_clear(led_strip);
                vTaskDelay(pdMS_TO_TICKS(200));
#else
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(150));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(150));
#endif
                break;
                
            case SYS_STATE_STA_CONNECTED:
#if CONFIG_USE_RGB_LED
                led_strip_set_pixel(led_strip, 0, 0, 50, 0); // Green
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(1000));
#else
                gpio_set_level(LED_WIFI, 1);
                gpio_set_level(LED_BLE, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                gpio_set_level(LED_BLE, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
#endif
                break;
                
            case SYS_STATE_STA_FAILED:
#if CONFIG_USE_RGB_LED
                led_strip_set_pixel(led_strip, 0, 50, 0, 0); // Red
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
                led_strip_clear(led_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
#else
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
#endif
                break;
        }
    }
}

// Check for factory reset button long-press (5 seconds)
static void button_monitor_task(void *pvParameters)
{
    int press_ticks = 0;
    ESP_LOGI(TAG, "Button Monitor Task started.");
    
    while (1) {
        // Active-low button
        if (gpio_get_level(KEY_SW) == 0) {
            press_ticks++;
            if (press_ticks % 10 == 0) {
                ESP_LOGW(TAG, "Config button pressed for %d seconds...", press_ticks / 10);
            }
            
            // Pressed for 5 seconds (50 * 100ms)
            if (press_ticks >= 50) {
                ESP_LOGE(TAG, "Factory reset button held for 5 seconds! Resetting configuration...");
                
                // Turn on both LEDs solid to indicate reset action
#if CONFIG_USE_RGB_LED
                led_strip_set_pixel(led_strip, 0, 50, 0, 0); // Solid Red
                led_strip_refresh(led_strip);
#else
                gpio_set_level(LED_WIFI, 1);
                gpio_set_level(LED_BLE, 1);
#endif
                
                config_manager_reset();
                
                vTaskDelay(pdMS_TO_TICKS(1000));
                ESP_LOGI(TAG, "Restarting now...");
                esp_restart();
            }
        } else {
            press_ticks = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  Barfas Smart BLE Gateway v%s starting  ", app_desc->version);
    ESP_LOGI(TAG, "==================================================");

    init_gpios();
    
    // Spawn status tasks
    xTaskCreate(&led_indicator_task, "led_task", 2048, NULL, 2, NULL);
    xTaskCreate(&button_monitor_task, "btn_task", 2048, NULL, 2, NULL);

    // Initialize configuration
    esp_err_t err = config_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fatal: NVS initialization failed.");
        s_sys_state = SYS_STATE_STA_FAILED;
        return;
    }

    // Initialize OTA Manager and handle rollback validation
    ota_manager_init();

    // Initialize Wi-Fi
    err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fatal: Wi-Fi initialization failed.");
        s_sys_state = SYS_STATE_STA_FAILED;
        return;
    }

    // Load device parameters
    gateway_config_t config;
    err = config_manager_load(&config);

    // If device_id in NVS does not match our new clean MAC, erase old config to force fresh onboarding
    char expected_id[17] = {0};
    uint8_t current_mac[6];
    esp_read_mac(current_mac, ESP_MAC_WIFI_STA);
    snprintf(expected_id, sizeof(expected_id), "%02X%02X%02X%02X%02X%02X",
             current_mac[0], current_mac[1], current_mac[2], current_mac[3], current_mac[4], current_mac[5]);
    
    if (err == ESP_OK && strcmp(config.device_id, expected_id) != 0) {
        ESP_LOGW(TAG, "Config MAC (%s) differs from new hardware MAC (%s). Resetting config to start fresh onboarding...", config.device_id, expected_id);
        config_manager_reset();
        config.is_configured = false;
        config.is_claimed = false;
        ble_server_set_claimed(false);
    }

    if (err == ESP_OK && config.is_configured) {
        // Device is configured, connect to router
        s_sys_state = SYS_STATE_STA_CONNECTING;
        
        err = wifi_manager_start_sta(config.wifi_ssid, config.wifi_pass);
        if (err == ESP_OK) {
            s_sys_state = SYS_STATE_STA_CONNECTED;
            ESP_LOGI(TAG, "Wi-Fi Connected. Initializing Gateway peripherals...");

            // Initialize and start BLE manager
            err = ble_manager_init();
            if (err == ESP_OK) {
                ble_manager_start();
            } else {
                ESP_LOGE(TAG, "BLE Manager initialization failed.");
            }

            // We must set the time via SNTP for TLS certificates to be validated properly
            ESP_LOGI(TAG, "Initializing SNTP for TLS Certificate validation...");
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();

            int sntp_retry = 0;
            while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && sntp_retry < 10) {
                ESP_LOGI(TAG, "Waiting for system time to be set... (%d/10)", sntp_retry);
                vTaskDelay(pdMS_TO_TICKS(2000));
                sntp_retry++;
            }
            time_t now;
            time(&now);
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "Current time synced: %s", asctime(&timeinfo));

            // We must perform HTTPS claim to get MQTT credentials
            api_claim_response_t claim_resp;
            memset(&claim_resp, 0, sizeof(claim_resp));
            ESP_LOGI(TAG, "Requesting MQTT credentials from API...");
            err = api_manager_claim_gateway(config.device_id, GATEWAY_DEVICE_CODE, g_provisioning_token, &claim_resp);
            
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "HTTPS Claim successful!");

                if (config.is_claimed && strlen(claim_resp.device_secret) > 0) {
                    char local_key_b64[64] = {0};
                    size_t b64_len = 0;
                    mbedtls_base64_encode((unsigned char *)local_key_b64, sizeof(local_key_b64), &b64_len, config.local_key, 32);
                    ESP_LOGI(TAG, "Syncing Local Key to backend API...");
                    api_manager_sync_local_key(config.device_id, claim_resp.device_secret, local_key_b64);
                }

                // Initialize and start MQTT manager
                const char *chosen_client_id = (strlen(claim_resp.device_id) > 0) ? claim_resp.device_id : NULL;
                err = mqtt_manager_init(claim_resp.mqtt_uri, claim_resp.mqtt_port, config.device_id, chosen_client_id, claim_resp.mqtt_username, claim_resp.mqtt_password, claim_resp.session_epoch);
                if (err == ESP_OK) {
                    mqtt_manager_start();
                } else {
                    ESP_LOGE(TAG, "MQTT Manager initialization failed.");
                }
            } else {
                ESP_LOGW(TAG, "HTTPS Claim returned error (%s). Device may already be claimed on backend.", esp_err_to_name(err));
                ESP_LOGI(TAG, "Proceeding with standard Gateway MQTT credentials...");

                char fallback_client_id[64];
                snprintf(fallback_client_id, sizeof(fallback_client_id), "gw-%s", config.device_id);
                
                err = mqtt_manager_init("mqtt.onloi.com", 1883, config.device_id, fallback_client_id, "mqttuser", "valrhRa27AeW2Ynv", 1);
                if (err == ESP_OK) {
                    mqtt_manager_start();
                } else {
                    ESP_LOGE(TAG, "Fallback MQTT Manager initialization failed.");
                }
            }
        } else {
            // Connection failed
            s_sys_state = SYS_STATE_STA_FAILED;
            ESP_LOGE(TAG, "Wi-Fi connection failed. Dropping back to config AP mode in 10 seconds...");
            
            vTaskDelay(pdMS_TO_TICKS(10000));
            ESP_LOGI(TAG, "Erasing bad config and restarting to enter AP mode...");
            config_manager_reset();
            esp_restart();
        }
    } else {
        // Device is not configured, boot in BLE peripheral mode for onboarding
        s_sys_state = SYS_STATE_UNCONFIGURED;
        ESP_LOGI(TAG, "Device is unconfigured. Starting BLE Manager for Onboarding...");
        
        err = ble_manager_init();
        if (err == ESP_OK) {
            ble_manager_start();
        } else {
            ESP_LOGE(TAG, "BLE Manager initialization failed.");
            s_sys_state = SYS_STATE_STA_FAILED;
        }
    }

    // Monitor system metrics
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        
        // System monitor diagnostics
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_8BIT);
        
        ESP_LOGI(TAG, "[MONITOR] Free Heap: %d bytes, Min Free Heap: %d bytes", 
                 info.total_free_bytes, heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    }
}
