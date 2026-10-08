/*
 * Studio custom subsystem for the ZitaoTech trackpad (ZitaoTech Studio's
 * "Trackpad" tab).
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#include <zmk/studio/custom.h>

#include <zitaotech/trackpad/trackpad.pb.h>
#include <zitaotech/zmk/trackpad.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta zitaotech_trackpad_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    // Unsecured: adjusting pointer feel is harmless and the UI is used while
    // tuning, often before the keyboard is unlocked.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech__trackpad, &zitaotech_trackpad_meta,
                         zitaotech_trackpad_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zitaotech__trackpad, zitaotech_trackpad_Response);

/* ---------------------------------------------------------------------------
 * Live updates: anything that changes on the device itself (the &DPI keys, and
 * later any key-driven mode) pushes a notification carrying the full state, so
 * the web UI never has to wait for its next poll.
 * ------------------------------------------------------------------------ */

static zitaotech_trackpad_State notification_state;

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const zitaotech_trackpad_Notification *notification =
        (const zitaotech_trackpad_Notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_submessage(stream, zitaotech_trackpad_Notification_fields, notification);
}

static int trackpad_subsystem_index(void) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, "zitaotech__trackpad") == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

static void fill_state(zitaotech_trackpad_State *out) {
    out->supported = true;
    strncpy(out->driver_name, CONFIG_ZITAOTECH_TRACKPAD_DRIVER_NAME, sizeof(out->driver_name) - 1);
    out->dpi_index = zitaotech_trackpad_dpi_index();
    out->dpi_value = zitaotech_trackpad_dpi_value();
    out->dpi_min = CONFIG_ZITAOTECH_TRACKPAD_DPI_MIN;
    out->dpi_max = CONFIG_ZITAOTECH_TRACKPAD_DPI_MAX;
    out->dpi_step = CONFIG_ZITAOTECH_TRACKPAD_DPI_STEP;
    out->dpi_levels = ZITAOTECH_TRACKPAD_DPI_LEVELS;
    out->scroll_speed_percent = zitaotech_trackpad_scroll_speed_percent();
    out->scroll_speed_min = CONFIG_ZITAOTECH_TRACKPAD_SCROLL_SPEED_MIN;
    out->scroll_speed_max = CONFIG_ZITAOTECH_TRACKPAD_SCROLL_SPEED_MAX;
    out->scroll_invert_x = zitaotech_trackpad_scroll_invert_x();
    out->scroll_invert_y = zitaotech_trackpad_scroll_invert_y();
    out->burst_enabled = zitaotech_trackpad_burst_enabled();
    out->burst_multiplier = zitaotech_trackpad_burst_multiplier();
    out->burst_multiplier_max = CONFIG_ZITAOTECH_TRACKPAD_BURST_MAX;
    /* mode_keys is a submessage: without has_mode_keys nanopb omits it from the
     * wire format entirely, which made every mode key look unassigned. */
    out->has_mode_keys = true;
    out->mode_keys.arrow_key = zitaotech_trackpad_arrow_key_position();
    out->mode_keys.fast_key = zitaotech_trackpad_fast_key_position();
    out->mode_keys.slow_key = zitaotech_trackpad_slow_key_position();
    out->mode_keys.scroll_key = zitaotech_trackpad_scroll_key_position();
    out->smooth_scroll_enabled = zitaotech_trackpad_smooth_scroll_enabled();
    out->arrow_swipe_threshold = zitaotech_trackpad_arrow_swipe_threshold();
    out->arrow_swipe_threshold_min = CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_MIN;
    out->arrow_swipe_threshold_max = CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_MAX;
    out->scroll_burst_enabled = zitaotech_trackpad_scroll_burst_enabled();
    out->scroll_burst_multiplier = zitaotech_trackpad_scroll_burst_multiplier();
    out->scroll_burst_multiplier_max = CONFIG_ZITAOTECH_TRACKPAD_SCROLL_BURST_MAX;
    out->caps_lock_scroll_enabled = zitaotech_trackpad_caps_lock_scroll_enabled();
    out->mouse_layer_enabled = zitaotech_trackpad_mouse_layer_enabled();
    out->mouse_layer_ms = zitaotech_trackpad_mouse_layer_ms();
    out->mouse_layer_ms_min = CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MIN;
    out->mouse_layer_ms_max = CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MAX;
    out->trackpoint_mouse_layer_enabled =
        zitaotech_trackpad_trackpoint_mouse_layer_enabled();
    out->trackpoint_mouse_layer_ms = zitaotech_trackpad_trackpoint_mouse_layer_ms();
    out->mouse_layer = CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER;
    out->touch_on_ms = zitaotech_trackpad_touch_on_ms();
    out->touch_on_ms_min = CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MIN;
    out->touch_on_ms_max = CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MAX;
    out->mode = (zitaotech_trackpad_TrackpadMode)zitaotech_trackpad_mode();
    out->mode_switch_enabled = zitaotech_trackpad_mode_switch_enabled();
}

/* Overrides the weak hook in src/trackpad.c. */
void zitaotech_trackpad_state_changed(void) {
    int index = trackpad_subsystem_index();
    if (index < 0) {
        return;
    }

    fill_state(&notification_state);
    zitaotech_trackpad_Notification notification =
        zitaotech_trackpad_Notification_init_zero;
    notification.has_state = true;
    notification.state = notification_state;

    pb_callback_t payload = {
        .funcs.encode = encode_notification_payload,
        .arg = (void *)&notification,
    };

    (void)raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
}

static void fill_error(zitaotech_trackpad_Response *resp, const char *message) {
    resp->which_response_type = zitaotech_trackpad_Response_error_tag;
    strncpy(resp->response_type.error.message, message,
            sizeof(resp->response_type.error.message) - 1);
}

static bool zitaotech_trackpad_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                                  pb_callback_t *encode_response) {
    zitaotech_trackpad_Response *resp = ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(
        zitaotech__trackpad, encode_response);

    zitaotech_trackpad_Request req = zitaotech_trackpad_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zitaotech_trackpad_Request_fields, &req)) {
        LOG_WRN("Failed to decode trackpad request: %s", PB_GET_ERROR(&req_stream));
        fill_error(resp, "Failed to decode request");
        return true;
    }

    /* Write requests answer with the new state themselves; only changes made on
     * the keyboard (the &DPI keys) need a push notification. */
    bool is_write = req.which_request_type != zitaotech_trackpad_Request_get_state_tag;
    if (is_write) {
        zitaotech_trackpad_suppress_notifications(true);
    }

    switch (req.which_request_type) {
    case zitaotech_trackpad_Request_get_state_tag:
        resp->which_response_type = zitaotech_trackpad_Response_state_tag;
        fill_state(&resp->response_type.state);
        return true;
    case zitaotech_trackpad_Request_set_dpi_tag: {
        int rc = zitaotech_trackpad_set_dpi_index(req.request_type.set_dpi.dpi_index);
        if (rc < 0) {
            fill_error(resp, rc == -EINVAL ? "DPI step out of range" : "Failed to set DPI");
            return true;
        }
        break;
    }
    case zitaotech_trackpad_Request_set_scroll_speed_tag: {
        int rc = zitaotech_trackpad_set_scroll_speed_percent(
            req.request_type.set_scroll_speed.speed_percent);
        if (rc < 0) {
            fill_error(resp, rc == -EINVAL ? "Scroll speed out of range" : "Failed to set speed");
            return true;
        }
        break;
    }
    case zitaotech_trackpad_Request_set_scroll_direction_tag:
        zitaotech_trackpad_set_scroll_invert(req.request_type.set_scroll_direction.invert_x,
                                             req.request_type.set_scroll_direction.invert_y);
        break;
    case zitaotech_trackpad_Request_set_burst_tag: {
        int rc = zitaotech_trackpad_set_burst(req.request_type.set_burst.enabled,
                                              req.request_type.set_burst.multiplier);
        if (rc < 0) {
            fill_error(resp, rc == -EINVAL ? "Burst multiplier out of range"
                                           : "Failed to set burst mode");
            return true;
        }
        break;
    }
    case zitaotech_trackpad_Request_set_mode_keys_tag: {
        const zitaotech_trackpad_ModeKeys *keys = &req.request_type.set_mode_keys.keys;
        const int32_t positions[ZITAOTECH_TRACKPAD_KEY_COUNT] = {
            keys->arrow_key,
            keys->fast_key,
            keys->slow_key,
            keys->scroll_key,
        };
        int rc = zitaotech_trackpad_set_mode_keys(positions);
        if (rc < 0) {
            fill_error(resp, "Key positions must be unique");
            return true;
        }
        break;
    }
    case zitaotech_trackpad_Request_set_smooth_scroll_tag:
        zitaotech_trackpad_set_smooth_scroll(req.request_type.set_smooth_scroll.enabled);
        break;
    case zitaotech_trackpad_Request_set_caps_lock_scroll_tag:
        zitaotech_trackpad_set_caps_lock_scroll(req.request_type.set_caps_lock_scroll.enabled);
        break;
    case zitaotech_trackpad_Request_set_mouse_layer_tag:
        zitaotech_trackpad_set_mouse_layer(req.request_type.set_mouse_layer.enabled);
        break;
    case zitaotech_trackpad_Request_set_mouse_layer_ms_tag:
        zitaotech_trackpad_set_mouse_layer_ms(req.request_type.set_mouse_layer_ms.ms);
        break;
    case zitaotech_trackpad_Request_set_trackpoint_mouse_layer_tag:
        zitaotech_trackpad_set_trackpoint_mouse_layer(
            req.request_type.set_trackpoint_mouse_layer.enabled);
        break;
    case zitaotech_trackpad_Request_set_trackpoint_mouse_layer_ms_tag:
        zitaotech_trackpad_set_trackpoint_mouse_layer_ms(
            req.request_type.set_trackpoint_mouse_layer_ms.ms);
        break;
    case zitaotech_trackpad_Request_set_touch_on_ms_tag: {
        int rc = zitaotech_trackpad_set_touch_on_ms(req.request_type.set_touch_on_ms.ms);
        if (rc < 0) {
            fill_error(resp, "Touch time out of range");
            return true;
        }
        break;
    }
    case zitaotech_trackpad_Request_set_mode_tag: {
        int rc = zitaotech_trackpad_set_mode((uint32_t)req.request_type.set_mode.mode);
        if (rc < 0) {
            fill_error(resp, "Unknown trackpad mode");
            return true;
        }
        break;
    }
    case zitaotech_trackpad_Request_set_mode_switch_enabled_tag: {
        zitaotech_trackpad_set_mode_switch_enabled(
            req.request_type.set_mode_switch_enabled.enabled);
        break;
    }
    case zitaotech_trackpad_Request_set_arrow_swipe_threshold_tag: {
        int rc = zitaotech_trackpad_set_arrow_swipe_threshold(
            req.request_type.set_arrow_swipe_threshold.threshold);
        if (rc < 0) {
            fill_error(resp, "Swipe threshold out of range");
            return true;
        }
        break;
    }
    case zitaotech_trackpad_Request_set_scroll_burst_tag: {
        int rc = zitaotech_trackpad_set_scroll_burst(req.request_type.set_scroll_burst.enabled,
                                                     req.request_type.set_scroll_burst.multiplier);
        if (rc < 0) {
            fill_error(resp, "Scroll burst multiplier out of range");
            return true;
        }
        break;
    }
    case zitaotech_trackpad_Request_reset_tag:
        zitaotech_trackpad_reset();
        break;
    default:
        LOG_WRN("Unsupported trackpad request type: %d", req.which_request_type);
        fill_error(resp, "Unsupported request");
        if (is_write) {
            zitaotech_trackpad_suppress_notifications(false);
        }
        return true;
    }

    if (is_write) {
        zitaotech_trackpad_suppress_notifications(false);
    }

    /* Write requests answer with the new state so the UI never has to guess. */
    resp->which_response_type = zitaotech_trackpad_Response_state_tag;
    fill_state(&resp->response_type.state);
    return true;
}
