# Only the T-Dongle-S3 has this panel; the source is not compiled for other
# boards. (REQUIRES must stay unconditional - ESP-IDF reads it before
# sdkconfig exists.)
set(srcs)
if(CONFIG_HR_DISPLAY)
    list(APPEND srcs "esp_lcd_st7735.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_gpio
)
