set(srcs "main.c" "hr_usb.c" "hr_wifi.c" "hr_http.c" "hr_mqtt.c" "hr_log.c" "hr_logring.c" "hr_capture.c" "hr_batchstore.c" "hr_compat.c" "hr_units.c" "hr_quiesce.c" "hr_reboot.c" "hr_netwatch.c")

# Board peripherals (T-Dongle-S3). The sources are only compiled when the
# board is selected, so the DevKitC binary is unchanged. hr_display.c owns
# the hr_ui task that drives whichever of screen, LED and button are on.
#
# REQUIRES cannot be conditional: ESP-IDF evaluates it in an early pass
# before sdkconfig is loaded, so the display components are always listed.
# They are static libraries and nothing references them in the generic
# build, so the linker drops them there.
if(CONFIG_HR_DISPLAY OR CONFIG_HR_LED OR CONFIG_HR_BUTTON)
    list(APPEND srcs "hr_display.c")
endif()
if(CONFIG_HR_DISPLAY)
    list(APPEND srcs "hr_display_screens.c" "hr_display_hw.c" "hr_gfx.c")
endif()
if(CONFIG_HR_LED)
    list(APPEND srcs "hr_led.c")
endif()
if(CONFIG_HR_BUTTON)
    list(APPEND srcs "hr_button.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
    EMBED_FILES "www/img/idle.png" "www/img/freeze.png"
                "www/img/dry.png" "www/img/heat.png"
    REQUIRES hr_protocol nvs_flash esp_wifi esp_http_server esp_netif lwip
             esp_timer app_update esp_hw_support spiffs
             hr_ui esp_lcd_st7735_lilygo esp_lcd
             esp_driver_spi esp_driver_gpio esp_driver_ledc
)
