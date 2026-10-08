zephyr_include_directories(include)

if(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    target_sources(app PRIVATE src/trackpad.c)

    if(CONFIG_ZITAOTECH_TRACKPAD_CONTROL_STUDIO_RPC)
        target_sources(app PRIVATE src/studio/trackpad_handler.c)

        list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)

        # Inside a Zephyr module CMAKE_C_COMPILE_FEATURES only holds the C
        # dialect Zephyr has picked for the build so far, so FindNanopb's
        #   target_compile_features(nanopb PUBLIC c_std_11)
        # fails with "the compiler feature c_std_11 is not known" unless C11
        # is in that list.  Add it before including nanopb.
        if(NOT "c_std_11" IN_LIST CMAKE_C_COMPILE_FEATURES)
            list(APPEND CMAKE_C_COMPILE_FEATURES c_std_11)
        endif()

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
