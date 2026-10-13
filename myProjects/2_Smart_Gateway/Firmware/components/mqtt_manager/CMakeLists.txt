idf_component_register(SRCS "mqtt_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ble_manager config_manager esp_app_format app_update ota_manager mbedtls)
