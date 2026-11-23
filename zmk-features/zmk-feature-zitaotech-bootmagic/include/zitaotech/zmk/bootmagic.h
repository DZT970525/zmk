/*
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/event_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Keymap position checked at power-up on this half (-1 = disabled). */
int32_t zitaotech_bootmagic_key_position(void);

/**
 * Set it.  On the central this stores the value locally; a peripheral target
 * is relayed to the other half, which stores it in its own flash - each half
 * has to read its own setting before its keyboard starts.
 */
int zitaotech_bootmagic_set_key(bool peripheral, int32_t position);

/** Hook called after any change; the Studio handler overrides it to notify. */
void zitaotech_bootmagic_state_changed(void);
void zitaotech_bootmagic_suppress_notifications(bool suppress);

/** Relayed copy of the other half's setting, so the UI can show both. */
#define ZITAOTECH_BOOTMAGIC_KEY_UNKNOWN (-2)

/** The other half's key as last relayed to us, or UNKNOWN. */
int32_t zitaotech_bootmagic_remote_key(void);
int32_t zitaotech_bootmagic_studio_left_key(void);

/**
 * Whether the other half is reachable right now.  Nothing heartbeats: the
 * split transport reports the link coming up and dropping, and this is just a
 * read of that state, which is also what lets Studio grey out its row instead
 * of pretending the setting could be written while it is away.
 */
bool zitaotech_bootmagic_peripheral_connected(void);

/**
 * Ask the other half what key it is configured for (central half only, no-op
 * on the peripheral).  Call it whenever its value matters and is still
 * unknown: the report a peripheral sends on its own is easily lost, because at
 * its power-up - and even at connect time - the central may not have
 * subscribed to the relay notification yet.
 */
void zitaotech_bootmagic_request_remote_key(void);

/** Raised on this half when its key setting changes. */
struct zitaotech_bootmagic_key_changed {
    int32_t position;
};

ZMK_EVENT_DECLARE(zitaotech_bootmagic_key_changed);

/** Relayed between the halves to keep both settings visible from Studio. */
struct zitaotech_bootmagic_mirror {
    int32_t position;
    uint8_t source;
#if (defined(CONFIG_BOARD_KEYPOINT_DONGLE) || defined(CONFIG_BOARD_SOFLE_DONGLE)) || (defined(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT)) || (defined(CONFIG_BOARD_KEYPOINT_DONGLE_RIGHT) || defined(CONFIG_BOARD_SOFLE_DONGLE_RIGHT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_RIGHT))
    uint8_t side;
#endif
};

ZMK_EVENT_DECLARE(zitaotech_bootmagic_mirror);

/** Relayed question "which key are you set to?" (central -> peripheral). */
struct zitaotech_bootmagic_query {
    uint8_t reserved;
};

ZMK_EVENT_DECLARE(zitaotech_bootmagic_query);

#ifdef __cplusplus
}
#endif
