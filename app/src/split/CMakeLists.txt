# Copyright (c) 2022 The ZMK Contributors
# SPDX-License-Identifier: MIT

if (CONFIG_ZMK_SPLIT_BLE)
    add_subdirectory(bluetooth)
endif()

if (CONFIG_ZMK_SPLIT_DUAL_ROLE_BUILD)
    target_sources(app PRIVATE peripheral.c)
    zephyr_linker_sources(SECTIONS ../../include/linker/zmk-split-transport-peripheral.ld)
endif()
target_sources_ifdef(CONFIG_ZMK_SPLIT_RUNTIME_ROLE app PRIVATE runtime_role.c)
target_sources_ifdef(CONFIG_ZMK_KEYPOINT_TOPOLOGY_FALLBACK app PRIVATE topology_fallback.c)
target_sources_ifdef(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER app PRIVATE dongle_presence_server.c)
target_sources_ifdef(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT app PRIVATE dongle_presence_client.c)

if (CONFIG_ZMK_SPLIT_WIRED)
    add_subdirectory(wired)
endif()

if (CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE central.c)
    zephyr_linker_sources(SECTIONS ../../include/linker/zmk-split-transport-central.ld)
else()
    target_sources(app PRIVATE peripheral.c)
    zephyr_linker_sources(SECTIONS ../../include/linker/zmk-split-transport-peripheral.ld)
endif()

target_sources_ifdef(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE app PRIVATE ../../boards/zitaotech/zitaotech_sofle/custom_driver/keypoint_relay_source.c)
target_sources_ifdef(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER app PRIVATE ../../boards/zitaotech/Macintosch/custom_driver/keypoint_relay_receiver.c)
