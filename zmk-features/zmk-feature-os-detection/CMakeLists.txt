zephyr_include_directories(include)
if(CONFIG_ZMK_OS_DETECTION)
    target_sources(app PRIVATE src/os_detection_core.c)

    if(CONFIG_ZMK_OS_DETECTION_USB)
        if(NOT CONFIG_USB_DEVICE_STACK)
            message(FATAL_ERROR
                "ZMK_OS_DETECTION_USB observes USB enumeration by linker-wrapping "
                "usb_handle_bos(), which only exists in Zephyr's legacy USB device "
                "stack (CONFIG_USB_DEVICE_STACK). This board/ZMK version does not "
                "have it enabled (e.g. it may use the newer device_next stack), so "
                "USB OS detection cannot work here. Disable ZMK_OS_DETECTION_USB.")
        endif()
        target_sources(app PRIVATE src/os_detection_usb.c)
        zephyr_ld_options(-Wl,--wrap=usb_handle_bos)
    endif()

    if(CONFIG_ZMK_OS_DETECTION_BLE)
        target_sources(app PRIVATE src/os_detection_ble.c)
        if(CONFIG_ZMK_CUSTOM_SETTINGS)
            target_sources(app PRIVATE src/os_detection_settings.c)
        endif()
    endif()

    if(CONFIG_ZMK_OS_DETECTION_STUDIO_RPC)
        target_sources(app PRIVATE src/studio/os_detection_handler.c)

        list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
        include(nanopb)
        set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
        set(NANOPB_GENERATE_CPP_STANDALONE OFF)

        # NOTE: adding to app target directly causes build issues, so we create a library instead
        zephyr_library()
        file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

        nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
        target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
        target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

        # app should depend on the generated proto files
        target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
        add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
    endif()
endif()
