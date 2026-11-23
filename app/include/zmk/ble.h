/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/keys.h>
#include <zmk/ble/profile.h>

#define ZMK_BLE_IS_CENTRAL                                                                         \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if ZMK_BLE_IS_CENTRAL
#if IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)
#define ZMK_BLE_PROFILE_COUNT 4
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_RUNTIME_ROLE) || IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
/* The hybrid left retains host bonds as well as right-hand and dongle bonds.
 * Extra bond capacity is not extra host profiles (Studio exposes five). */
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
#define ZMK_BLE_PROFILE_COUNT 4
#else
#define ZMK_BLE_PROFILE_COUNT 5
#endif
#if CONFIG_BT_MAX_PAIRED < (5 + CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS + 1)
#error "Hybrid left needs bond capacity for five hosts, its peripherals and the dongle"
#endif
#else
#define ZMK_BLE_PROFILE_COUNT (CONFIG_BT_MAX_PAIRED - CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS)
#endif
#define ZMK_SPLIT_BLE_PERIPHERAL_COUNT CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS
#else
#define ZMK_BLE_PROFILE_COUNT CONFIG_BT_MAX_PAIRED
#endif

void zmk_ble_clear_bonds(void);
bool zmk_ble_host_is_enabled(void);
int zmk_ble_host_set_enabled(bool enabled);
int zmk_ble_prof_next(void);
int zmk_ble_prof_prev(void);
int zmk_ble_prof_select(uint8_t index);
void zmk_ble_clear_all_bonds(void);
int zmk_ble_prof_disconnect(uint8_t index);

int zmk_ble_active_profile_index(void);
int zmk_ble_profile_index(const bt_addr_le_t *addr);
bt_addr_le_t *zmk_ble_profile_address(uint8_t index);

bt_addr_le_t *zmk_ble_active_profile_addr(void);
struct bt_conn *zmk_ble_active_profile_conn(void);

bool zmk_ble_profile_is_connected(uint8_t index);
bool zmk_ble_profile_is_open(uint8_t index);

bool zmk_ble_active_profile_is_open(void);
bool zmk_ble_active_profile_is_connected(void);
char *zmk_ble_active_profile_name(void);

int zmk_ble_unpair_all(void);

int zmk_ble_set_device_name(char *name);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int zmk_ble_put_peripheral_addr(const bt_addr_le_t *addr);
#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)
uint8_t zmk_ble_last_host_disconnect_reason(void);
#endif

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)
uint8_t zmk_ble_host_param_request_count(void);
#endif
