cmake_minimum_required(VERSION 3.13.1)

set(CONFIG_APPLICATION_DEFINED_SYSCALL true)

set(ZEPHYR_BOARD_ALIASES "boards/aliases.cmake")
# ZitaoTech: the trackpad / backlight feature modules are vendored in-tree
# under zmk-features/ so the board and shield drivers that use their headers
# (pointing_dpi.h, zitaotech/zmk/trackpad.h, ...) build without a west project.
get_filename_component(ZITAOTECH_FEATURES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../zmk-features ABSOLUTE)
set(ZITAOTECH_FEATURE_MODULES
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-zitaotech-trackpad
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-zitaotech-trackpoint
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-zitaotech-bootmagic
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-zitaotech-backlight
    ${ZITAOTECH_FEATURES_DIR}/zmk-behavior-runtime-sensor-rotate
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-custom-settings
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-runtime-macro
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-runtime-tap-dance
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-runtime-combo
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-default-layer
    ${ZITAOTECH_FEATURES_DIR}/zmk-module-ble-management
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-os-detection
    ${ZITAOTECH_FEATURES_DIR}/zmk-module-settings-rpc
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-matrix-tester
    ${ZITAOTECH_FEATURES_DIR}/zmk-feature-device-info
)

set(ZEPHYR_EXTRA_MODULES "${ZMK_EXTRA_MODULES};${CMAKE_CURRENT_SOURCE_DIR}/module;${CMAKE_CURRENT_SOURCE_DIR}/keymap-module;${ZITAOTECH_FEATURE_MODULES}")

# Find Zephyr. This also loads Zephyr's build system.
find_package(Zephyr REQUIRED HINTS ../zephyr)
project(zmk)

zephyr_linker_sources(SECTIONS include/linker/zmk-behaviors.ld)
zephyr_linker_sources(RODATA include/linker/zmk-events.ld)

if(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
  zephyr_linker_sources(DATA_SECTIONS include/linker/zmk-behavior-local-id-map.ld)
endif()

zephyr_syscall_header(${APPLICATION_SOURCE_DIR}/include/drivers/behavior.h)
zephyr_syscall_header(${APPLICATION_SOURCE_DIR}/include/drivers/input_processor.h)
zephyr_syscall_header(${APPLICATION_SOURCE_DIR}/include/drivers/ext_power.h)

# Add your source file to the "app" target. This must come after
# find_package(Zephyr) which defines the target.
target_include_directories(app PRIVATE include)
add_subdirectory(src/boot)
target_sources(app PRIVATE src/stdlib.c)
target_sources(app PRIVATE src/activity.c)
target_sources(app PRIVATE src/behavior.c)
target_sources_ifdef(CONFIG_ZMK_KSCAN_SIDEBAND_BEHAVIORS app PRIVATE src/kscan_sideband_behaviors.c)
target_sources(app PRIVATE src/matrix_transform.c)
target_sources(app PRIVATE src/physical_layouts.c)
target_sources(app PRIVATE src/sensors.c)
target_sources_ifdef(CONFIG_ZMK_WPM app PRIVATE src/wpm.c)
target_sources(app PRIVATE src/event_manager.c)
target_sources_ifdef(CONFIG_ZMK_PM app PRIVATE src/pm.c)
target_sources_ifdef(CONFIG_ZMK_EXT_POWER app PRIVATE src/ext_power_generic.c)
target_sources_ifdef(CONFIG_ZMK_GPIO_KEY_WAKEUP_TRIGGER app PRIVATE src/gpio_key_wakeup_trigger.c)
target_sources(app PRIVATE src/events/activity_state_changed.c)
target_sources(app PRIVATE src/events/position_state_changed.c)
target_sources(app PRIVATE src/events/sensor_event.c)
target_sources_ifdef(CONFIG_ZMK_WPM app PRIVATE src/events/wpm_state_changed.c)
target_sources_ifdef(CONFIG_USB_DEVICE_STACK app PRIVATE src/events/usb_conn_state_changed.c)
target_sources(app PRIVATE src/behaviors/behavior_reset.c)
target_sources_ifdef(CONFIG_ZMK_EXT_POWER app PRIVATE src/behaviors/behavior_ext_power.c)
target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SOFT_OFF app PRIVATE src/behaviors/behavior_soft_off.c)
add_subdirectory_ifdef(CONFIG_ZMK_POINTING src/pointing/)
if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE src/hid.c)
  target_sources(app PRIVATE src/behaviors/behavior_key_press.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_KEY_TOGGLE app PRIVATE src/behaviors/behavior_key_toggle.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_HOLD_TAP app PRIVATE src/behaviors/behavior_hold_tap.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_STICKY_KEY app PRIVATE src/behaviors/behavior_sticky_key.c)
  target_sources(app PRIVATE src/behaviors/behavior_caps_word.c)
  target_sources(app PRIVATE src/behaviors/behavior_key_repeat.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_MACRO app PRIVATE src/behaviors/behavior_macro.c)
  target_sources(app PRIVATE src/behaviors/behavior_momentary_layer.c)
  target_sources(app PRIVATE src/behaviors/behavior_mod_morph.c)
  target_sources(app PRIVATE src/behaviors/behavior_outputs.c)
  target_sources(app PRIVATE src/behaviors/behavior_toggle_layer.c)
  target_sources(app PRIVATE src/behaviors/behavior_to_layer.c)
  target_sources(app PRIVATE src/behaviors/behavior_transparent.c)
  target_sources(app PRIVATE src/behaviors/behavior_none.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SENSOR_ROTATE app PRIVATE src/behaviors/behavior_sensor_rotate.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SENSOR_ROTATE_VAR app PRIVATE src/behaviors/behavior_sensor_rotate_var.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SENSOR_ROTATE_COMMON app PRIVATE src/behaviors/behavior_sensor_rotate_common.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_MOUSE_KEY_PRESS app PRIVATE src/behaviors/behavior_mouse_key_press.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_STUDIO_UNLOCK app PRIVATE src/behaviors/behavior_studio_unlock.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_INPUT_TWO_AXIS app PRIVATE src/behaviors/behavior_input_two_axis.c)
  target_sources(app PRIVATE src/combo.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_TAP_DANCE app PRIVATE src/behaviors/behavior_tap_dance.c)
  target_sources(app PRIVATE src/behavior_queue.c)
  target_sources(app PRIVATE src/conditional_layer.c)
  target_sources(app PRIVATE src/endpoints.c)
  target_sources(app PRIVATE src/events/endpoint_changed.c)
  target_sources(app PRIVATE src/hid_listener.c)
  target_sources(app PRIVATE src/keymap.c)
  target_sources(app PRIVATE src/events/layer_state_changed.c)
  target_sources(app PRIVATE src/events/modifiers_state_changed.c)
  target_sources(app PRIVATE src/events/keycode_state_changed.c)
  target_sources_ifdef(CONFIG_ZMK_HID_INDICATORS app PRIVATE src/hid_indicators.c)

  if (CONFIG_ZMK_BLE)
    target_sources(app PRIVATE src/events/ble_active_profile_changed.c)
    target_sources(app PRIVATE src/behaviors/behavior_bt.c)
    if (CONFIG_ZITAOTECH_BBP9981_UPSTREAM_BLE)
      if (CONFIG_ZITAOTECH_Q20_RECONNECT_RETRY)
        target_sources(app PRIVATE boards/zitaotech/zitaotech_q20/custom_driver/ble_reconnect.c)
      else()
        target_sources(app PRIVATE src/bbp9981_upstream/ble.c)
      endif()
      target_sources(app PRIVATE src/bbp9981_upstream/hog.c src/bbp9981_upstream/compat.c)
    else()
      target_sources(app PRIVATE src/ble.c src/hog.c)
    endif()
  endif()
endif()

target_sources_ifdef(CONFIG_ZMK_RGB_UNDERGLOW app PRIVATE src/behaviors/behavior_rgb_underglow.c)
target_sources_ifdef(CONFIG_ZMK_BACKLIGHT app PRIVATE src/behaviors/behavior_backlight.c)

target_sources_ifdef(CONFIG_ZMK_BATTERY_REPORTING app PRIVATE src/events/battery_state_changed.c)
target_sources_ifdef(CONFIG_ZMK_BATTERY_REPORTING app PRIVATE src/battery.c)

target_sources_ifdef(CONFIG_ZMK_HID_INDICATORS app PRIVATE src/events/hid_indicators_changed.c)
add_subdirectory_ifdef(CONFIG_ZMK_HID_INDICATORS src/indicators)

target_sources_ifdef(CONFIG_ZMK_SPLIT app PRIVATE src/events/split_peripheral_status_changed.c)
target_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL app PRIVATE src/events/split_central_status_changed.c)
add_subdirectory_ifdef(CONFIG_ZMK_SPLIT src/split)

target_sources_ifdef(CONFIG_USB_DEVICE_STACK app PRIVATE src/usb.c)
target_sources_ifdef(CONFIG_ZMK_USB app PRIVATE src/usb_hid.c)
target_sources_ifdef(CONFIG_ZMK_RGB_UNDERGLOW app PRIVATE src/rgb_underglow.c)
target_sources_ifdef(CONFIG_ZMK_BACKLIGHT app PRIVATE src/backlight.c)
target_sources_ifdef(CONFIG_ZMK_LOW_PRIORITY_WORK_QUEUE app PRIVATE src/workqueue.c)
target_sources(app PRIVATE src/main.c)

add_subdirectory(src/display/)
add_subdirectory_ifdef(CONFIG_SETTINGS src/settings/)

if (CONFIG_ZMK_STUDIO_RPC)
  # For some reason this is failing if run from a different sub-file.
  list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)

  include(nanopb)

    # Turn off the default nanopb behavior
  set(NANOPB_GENERATE_CPP_STANDALONE OFF)
  # ZitaoTech: the studio messages are vendored under app/proto/zmk/ instead of
  # being read from the zmk-studio-messages module, because the ZitaoTech
  # custom subsystem (`zmk.custom`, used by the trackpad/backlight Studio tabs
  # and by the other ZitaoTech modules) adds a subsystem to studio.proto that
  # the upstream module does not have.  The vendored files are copies of the
  # pinned module revision plus that addition, and they have to be kept in sync
  # when the module is updated.
  #
  # The protos import each other with bare names (e.g. `import "meta.proto";`),
  # so the directory that holds them has to be part of protoc's import path.
  # Generating them relative to the app directory puts the output in
  # <build>/proto/zmk/*.pb.{c,h}, which is where <proto/zmk/studio.pb.h> and
  # friends are expected to live.
  list(APPEND NANOPB_IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto/zmk)

  nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR}
                        ${CMAKE_CURRENT_SOURCE_DIR}/proto/zmk/studio.proto
                        ${CMAKE_CURRENT_SOURCE_DIR}/proto/zmk/meta.proto
                        ${CMAKE_CURRENT_SOURCE_DIR}/proto/zmk/core.proto
                        ${CMAKE_CURRENT_SOURCE_DIR}/proto/zmk/behaviors.proto
                        ${CMAKE_CURRENT_SOURCE_DIR}/proto/zmk/keymap.proto
                        ${CMAKE_CURRENT_SOURCE_DIR}/proto/zmk/custom.proto
  )

  target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
  target_sources(app PRIVATE ${proto_srcs} ${proto_hdrs})

  add_subdirectory(src/studio)
endif()

zephyr_cc_option(-Wfatal-errors)
if((CONFIG_BOARD_SOFLE_DONGLE_LEFT OR CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) AND NOT CONFIG_ZMK_SETTINGS_RESET_ON_START)
  target_sources(app PRIVATE src/mo_priority.c)
endif()
target_sources(app PRIVATE src/behaviors/behavior_force_layer.c)
target_sources(app PRIVATE src/behaviors/behavior_keypoint_display_mode.c)
