# Reuse the maintained implementation with KEYPOINT's own bus, IRQ and layout.
set(modular_board ${CMAKE_CURRENT_LIST_DIR}/..)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../zitaotech_sofle/modular
                 ${CMAKE_CURRENT_BINARY_DIR}/implementation)

target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/bus_select.c)
