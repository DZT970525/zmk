if(CONFIG_BOARD_ZITAOTECH_SOFLE_LEFT OR CONFIG_BOARD_ZITAOTECH_SOFLE_RIGHT)
zephyr_library()
zephyr_library_sources(custom_driver/behavior_pointing_dpi.c)
zephyr_library_sources(custom_driver/behavior_trackpad_mode.c)
if(CONFIG_BOARD_ZITAOTECH_SOFLE_LEFT)
zephyr_library_sources(custom_driver/input_processor_trackpoint_dpi.c)
endif()
# zephyr_library_sources(custom_driver_left/keyboard_backlight.c)
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers)
zephyr_library_include_directories(../../../include)
zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
zephyr_library_include_directories(custom_driver)
endif()



