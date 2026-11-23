zephyr_library()
zephyr_library_sources(../zitaotech_sofle/custom_driver/behavior_layer_mod.c
    ../zitaotech_sofle/custom_driver/behavior_mouse_key_toggle.c)
zephyr_library_sources(custom_driver/a320_0x3B.c custom_driver/trackpad_led.c
    custom_driver/layer_indicator.c custom_driver/behavior_trackpad_dpi.c
    custom_driver/behavior_trackpad_mode.c custom_driver/host_pointing_profiles.c)
zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include custom_driver)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/custom_driver/mouse_layer_extension.c)

if(CONFIG_ZMK_SETTINGS_RESET_ON_START)
    zephyr_library_sources(custom_driver/reset_to_bootloader.c)
endif()

zephyr_library_sources_ifdef(CONFIG_ZITAOTECH_Q20_VIAL_MATRIX custom_driver/vial_matrix.c)
