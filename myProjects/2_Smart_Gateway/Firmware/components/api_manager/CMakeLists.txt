idf_component_register(SRCS "api_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_client esp-tls log crypto_manager)
