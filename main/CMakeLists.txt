idf_component_register(
    SRCS "trying.c" "bt_a2dp.c" "bt_hfp.c" "i2s_audio.c"
    INCLUDE_DIRS "."
    REQUIRES bt driver nvs_flash esp_timer
)