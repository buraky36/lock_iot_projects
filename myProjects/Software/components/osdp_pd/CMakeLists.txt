idf_component_register(
    SRCS "osdp_pd.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer hal_io nv_storage app_state_machine
)
