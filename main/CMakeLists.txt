# Note: ESP-IDF components can't use file(GLOB ... CONFIGURE_DEPENDS) here (the
# component file is also evaluated in script mode), and a plain GLOB silently
# misses new files until a reconfigure. Keep the explicit list: when adding a
# new .c, add it here too.
idf_component_register(SRCS "ui_music.c" "main.c" "st77916_init.c" "touch_cst816s.c" "audio.c" "wifi_mgr.c" "battery.c" "qmi8658.c" "auto_rotate.c" "rtc_pcf85063.c" "nav_ble.c" "nav_proto.c" "nav_map.c" "ui_nav.c" "ui_theme.c" "ui_shell.c" "ui_desktop.c" "ui_app.c" "ui_multitask.c" "ui_settings.c" "ui_wifi.c" "ui_gforce.c" "ui_timer.c" "ui_clock.c" "clock_faces.c" "clock_big_font.c" "hooge55.c" "hooge36.c" "hooge25.c" "hooge20.c" "hooge17.c" "misans_14.c" "misans_16.c" "misans_20.c" "misans_28.c" "gesture.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_gpio esp_lcd esp_timer esp_codec_dev esp_wifi esp_netif nvs_flash esp_event lwip bt)
