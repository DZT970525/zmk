zephyr_library()
zephyr_library_sources(custom_driver/a320.c custom_driver/trackpad_led.c
    custom_driver/layer_indicator.c custom_driver/behavior_pointing_dpi.c
    custom_driver/behavior_trackpad_mode.c)
zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include custom_driver)
