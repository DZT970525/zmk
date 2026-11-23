if(NOT CONFIG_SOFLE_MODULAR_POINTING)
if(NOT CONFIG_ZMK_SETTINGS_RESET_ON_START)
zephyr_library()
zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../../zitaotech/zitaotech_sofle/custom_driver_left/a320.c)
zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../../zitaotech/zitaotech_sofle/custom_driver_left/trackpad_led.c)
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../../zitaotech/zitaotech_sofle/custom_driver)
endif()

endif()
