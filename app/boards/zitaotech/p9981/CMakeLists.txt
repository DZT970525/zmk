zephyr_library()
zephyr_library_sources(
    ../zitaotech_sofle/custom_driver/behavior_layer_mod.c
    ../zitaotech_sofle/custom_driver/behavior_mouse_key_toggle.c
    custom_driver/layer_indicator.c)
zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)

if(CONFIG_ZMK_SETTINGS_RESET_ON_START)
    zephyr_library_sources(../zitaotech_q20/custom_driver/reset_to_bootloader.c)
endif()
