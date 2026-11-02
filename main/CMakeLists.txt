# Note: ESP-IDF components can't use file(GLOB ... CONFIGURE_DEPENDS) here (the
# component file is also evaluated in script mode), and a plain GLOB silently
# misses new files until a reconfigure. Keep the explicit list: when adding a
# new .c, add it here too.
idf_component_register(SRCS "main.c"
                            "core/ui_shell.c" "core/ui_desktop.c" "core/ui_multitask.c"
                            "core/ui_theme.c" "core/ui_app.c" "core/gesture.c" "core/auto_rotate.c"
                            "core/boot_anim.c"
                            "apps/ui_clock.c" "apps/clock_faces.c" "apps/ui_settings.c" "apps/ui_wifi.c"
                            "apps/ui_music.c" "apps/ui_nav.c" "apps/ui_gforce.c" "apps/ui_timer.c"
                            "nav/nav_ble.c" "nav/nav_proto.c" "nav/nav_map.c"
                            "drivers/st77916_init.c" "drivers/touch_cst816s.c" "drivers/qmi8658.c"
                            "drivers/rtc_pcf85063.c" "drivers/battery.c" "drivers/audio.c" "drivers/wifi_mgr.c"
                            "fonts/misans_14.c" "fonts/misans_16.c" "fonts/misans_20.c" "fonts/misans_28.c"
                            "fonts/hooge17.c" "fonts/hooge20.c" "fonts/hooge25.c" "fonts/hooge36.c" "fonts/hooge55.c"
                            "fonts/clock_big_font.c"
                            "icons/logo_mark.c" "icons/logo_honda.c" "icons/logo_glow.c"
                       INCLUDE_DIRS "." "core" "apps" "nav" "drivers" "fonts" "icons"
                       PRIV_REQUIRES esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_gpio esp_lcd esp_timer esp_codec_dev esp_wifi esp_netif nvs_flash esp_event lwip bt)
