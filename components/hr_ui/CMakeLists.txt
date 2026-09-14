# Screen logic for the T-Dongle-S3 display. Portable C, but nothing on a
# DevKitC calls it, so stay out of that build entirely.
if(NOT CONFIG_HR_DISPLAY)
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "hr_ui_model.c"
    INCLUDE_DIRS "include"
    REQUIRES hr_protocol
)
