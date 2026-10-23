idf_component_register(
    SRCS "main.c" "dht_native.c"
    INCLUDE_DIRS "../include"
    REQUIRES esp_adc esp_driver_gpio esp_driver_i2c esp_lcd nvs_flash esp_timer
)
