# Copyright (c) 2022 The ZMK Contributors
# SPDX-License-Identifier: MIT

if (NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL OR CONFIG_ZMK_SPLIT_DUAL_ROLE_BUILD)
  target_sources(app PRIVATE service.c)
  target_sources(app PRIVATE peripheral.c)
endif()
if (CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE central.c)
endif()

if (CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY)
  target_sources(app PRIVATE central_bas_proxy.c)
endif()
target_sources_ifdef(CONFIG_ZMK_KEYPOINT_PEER_SELECTION app PRIVATE peer_selection.c)
