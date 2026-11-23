if(CONFIG_ZMK_RUNTIME_TAP_DANCE)
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime_tap_dance.c)
endif()
