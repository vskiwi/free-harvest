# Screen / LED / button logic for the T-Dongle-S3. Portable C, but nothing
# on a DevKitC calls it, so it is not compiled into that build. (REQUIRES
# must stay unconditional - ESP-IDF reads it before sdkconfig exists.)
set(srcs)
if(CONFIG_HR_BOARD_T_DONGLE_S3)
    list(APPEND srcs "hr_ui_model.c" "hr_ui_dim.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES hr_protocol
)
