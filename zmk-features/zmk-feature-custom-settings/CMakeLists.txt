zephyr_include_directories(include)
if(CONFIG_ZMK_CUSTOM_SETTINGS)
    # Simplification P4: const setting descriptors live in rodata
    # (ROM_SECTIONS); the keyspace registry stays a RAM data section.
    zephyr_linker_sources(ROM_SECTIONS include/linker/zmk-custom-settings-rom.ld)
    zephyr_linker_sources(DATA_SECTIONS include/linker/zmk-custom-settings.ld)

    target_sources(app PRIVATE src/custom_settings.c)
    if(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES)
        target_sources(app PRIVATE src/custom_settings_pool.c)
    endif()
    if(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
        target_sources(app PRIVATE src/custom_settings_array.c)
    endif()
    if(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
        target_sources(app PRIVATE src/custom_settings_keyspace.c)
    endif()
    if(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS)
        target_sources(app PRIVATE src/custom_settings_rpc_convert.c)
    endif()
    if(CONFIG_ZMK_CUSTOM_SETTINGS_RECORD)
        target_sources(app PRIVATE src/custom_settings_record.c)
    endif()

    if(CONFIG_ZMK_CUSTOM_SETTINGS_TEST)
        target_sources(app PRIVATE src/test/custom_settings_test.c)
    endif()

    if(CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES)
        target_sources(app PRIVATE src/test/zmk_config_sample_settings.c)
    endif()

    if(CONFIG_ZMK_CUSTOM_SETTINGS_PROTOBUF)
        target_sources(app PRIVATE src/studio/custom_settings_handler.c)

        list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
        include(nanopb)
        set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
        set(NANOPB_GENERATE_CPP_STANDALONE OFF)

        # NOTE: adding to app target directly causes build issues, so we create a library instead
        zephyr_library()
        file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)
        set(NANOPB_IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto)

        nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
        target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR}
                                                                    ${CMAKE_CURRENT_BINARY_DIR}/proto)
        target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

        # app should depend on the generated proto files
        target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
        add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
    endif()
endif()
