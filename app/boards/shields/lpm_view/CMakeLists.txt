if(CONFIG_ZMK_DISPLAY AND CONFIG_NICE_VIEW_WIDGET_STATUS)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(custom_status_screen.c)
  zephyr_library_sources(widgets/bolt.c)
  zephyr_library_sources(widgets/util.c)
  zephyr_library_sources(display_driver/lpm009m360a.c)
  if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(widgets/peripheral_status.c)
  else()
    zephyr_library_sources(widgets/peripheral_status.c)
  endif()
  zephyr_library_sources(widgets/art.c)
  zephyr_library_sources(widgets/screen_config.c)
  zephyr_library_sources(widgets/screen_image_store.c)
  zephyr_library_sources(widgets/screen_transfer.c)

  # Studio RPC for the carousel.  Both the generated Studio protos
  # (<proto/zmk/custom.pb.h>) and the shield's own message types are needed, and
  # only the app target has the former on its include path - so this handler is
  # compiled there, and only on the central half, which is the one Studio talks
  # to.
  if(CONFIG_ZMK_STUDIO_RPC AND CONFIG_ZITAOTECH_SCREEN_STUDIO_RPC)
    if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
      list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
      include(nanopb)
      set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
      set(NANOPB_GENERATE_CPP_STANDALONE OFF)

      # Adding the generated sources to the app target directly breaks the
      # build, so they get their own library (same as the ZitaoTech modules).
      zephyr_library()
      file(GLOB_RECURSE SCREEN_PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)
      nanopb_generate_cpp(screen_proto_srcs screen_proto_hdrs RELPATH
                          ${CMAKE_CURRENT_SOURCE_DIR} ${SCREEN_PROTO_FILES})
      target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
      target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${screen_proto_srcs} ${screen_proto_hdrs})

      target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
      add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
      target_sources(app PRIVATE widgets/screen_rpc.c)
      target_sources(app PRIVATE widgets/screen_usb.c)
    endif()
  endif()

  zephyr_library_sources(widgets/picture/cat.c)
  zephyr_library_sources(widgets/picture/astronaut.c)
  zephyr_library_sources(widgets/picture/blackhole.c)
  zephyr_library_sources(widgets/picture/kanagawa.c)
  zephyr_library_sources(widgets/picture/winnie.c)
  zephyr_library_sources(widgets/picture/red_blue_yellow.c)
  zephyr_library_sources(widgets/picture/panda.c)
  zephyr_library_sources(widgets/picture/king.c)
endif()
