# Only the T-Dongle-S3 has this panel. Register an empty component on other
# boards so the DevKitC build does not even pull in esp_lcd.
if(NOT CONFIG_HR_DISPLAY)
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "esp_lcd_st7735.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_gpio
)
