idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_https_ota esp_http_client mbedtls esp-tls bootloader_support app_update esp_partition crypto_manager)
