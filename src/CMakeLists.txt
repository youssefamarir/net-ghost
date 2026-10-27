idf_component_register(
    SRCS "main.c" "display.c" "ui.c" "eth.c" "wifi.c" "ble.c" "tools.c" "web.c" "json.c" "settings.c" "battery.c" "packets.c" "poe_decode.c" "speed.c" "switchcfg.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_spi esp_driver_gpio esp_driver_i2c esp_driver_tsens esp_driver_ledc esp_adc esp_lcd esp_eth esp_netif
             esp_wifi esp_coex esp_event esp_timer nvs_flash esp_http_server esp_http_client esp-tls mqtt bt lwip mbedtls libssh2
)
# string fields are deliberately cut to fit the screen / JSON buffers
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
