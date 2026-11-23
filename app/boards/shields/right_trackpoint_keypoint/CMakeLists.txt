# KEYPOINT standalone: reuse the paired raw-input velocity driver.
if(NOT CONFIG_SOFLE_MODULAR_POINTING AND NOT CONFIG_ZMK_SETTINGS_RESET_ON_START)
zephyr_library()
zephyr_library_sources(../../zitaotech/zitaotech_sofle/custom_driver_right/custom_led.c trackpoint_0x15.c)
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers ../../../include ${CMAKE_SOURCE_DIR}/include
    ../../zitaotech/zitaotech_sofle/custom_driver_right ../../zitaotech/zitaotech_sofle/custom_driver)
zephyr_compile_definitions(MOTION_GPIO_PIN=7)
endif()
