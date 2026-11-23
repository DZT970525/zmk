set(modular_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/generate.py)
if(CONFIG_BOARD_SOFLE_DONGLE_LEFT OR CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT)
  set(modular_side left)
else()
  set(modular_side right)
endif()
if(NOT DEFINED modular_board)
  set(modular_board ${CMAKE_CURRENT_LIST_DIR}/..)
endif()
execute_process(COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/generate.py
  ${modular_board} ${modular_gen} ${modular_side}
  COMMAND_ERROR_IS_FATAL ANY)
target_include_directories(app PRIVATE ${modular_gen} ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/../custom_driver_left ${CMAKE_CURRENT_LIST_DIR}/../custom_driver_right
  ${CMAKE_CURRENT_LIST_DIR}/../custom_driver)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/modular.c
  ${modular_gen}/right_trackpad.c ${modular_gen}/left_trackpoint.c ${modular_gen}/left_curve.c
  ${modular_gen}/extra_profiles.c ${modular_gen}/local_a320.c ${modular_gen}/local_trackpoint.c
  ${modular_gen}/local_pad_led.c ${modular_gen}/local_tp_led.c)
if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE ${modular_gen}/left_tp_processor.c ${modular_gen}/right_pad_processor.c)
endif()
if(CONFIG_ZMK_STUDIO AND CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE ${modular_gen}/right_trackpad_rpc.c ${modular_gen}/left_trackpoint_rpc.c)
endif()
