idf_component_register(SRCS "main.c" "p4_display.c" "bambu_app.c" "bambu_config.c" "bambu_video.c" "todo_app.c" "codex_app.c" INCLUDE_DIRS "." EMBED_TXTFILES "printer_settings.html" REQUIRES portable_ui
    PRIV_REQUIRES openh264 esp_driver_jpeg esp_driver_ppa espressif__esp_lcd_touch esp_wifi esp_netif esp_event nvs_flash mqtt esp_http_server esp_http_client esp_timer lwip mbedtls)
