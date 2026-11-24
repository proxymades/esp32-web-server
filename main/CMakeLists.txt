idf_component_register(
    SRCS "main.c" "sensor.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_wifi esp_event esp_netif
             esp_http_server esp_timer nvs_flash
    EMBED_FILES "web/index.html"
)
