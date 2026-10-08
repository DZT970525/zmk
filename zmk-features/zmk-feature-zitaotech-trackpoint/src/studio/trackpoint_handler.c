/*
 * Studio custom subsystem for the ZitaoTech TrackPoint (ZitaoTech Studio's
 * "TrackPoint" tab).
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

#include <zitaotech/trackpoint/trackpoint.pb.h>
#include <zitaotech/zmk/trackpoint.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta zitaotech_trackpoint_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    // Unsecured: adjusting pointer feel is harmless and the UI is used while
    // tuning, often before the keyboard is unlocked.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech__trackpoint, &zitaotech_trackpoint_meta,
                         zitaotech_trackpoint_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zitaotech__trackpoint, zitaotech_trackpoint_Response);

/* ---------------------------------------------------------------------------
 * Live updates: anything that changes on the device itself (the &DPI keys, or
 * the other half through the split relay) pushes a notification carrying the
 * full state, so the web UI never has to wait for its next poll.
 * ------------------------------------------------------------------------ */

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const zitaotech_trackpoint_Notification *notification =
        (const zitaotech_trackpoint_Notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_submessage(stream, zitaotech_trackpoint_Notification_fields, notification);
}

static int trackpoint_subsystem_index(void) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, "zitaotech__trackpoint") == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

static void fill_state(zitaotech_trackpoint_State *out) {
    out->supported = true;
    strncpy(out->driver_name, CONFIG_ZITAOTECH_TRACKPOINT_DRIVER_NAME,
            sizeof(out->driver_name) - 1);
    out->dpi_index = zitaotech_trackpoint_dpi_index();
    out->dpi_value = zitaotech_trackpoint_dpi_value();
    out->dpi_min = CONFIG_ZITAOTECH_TRACKPOINT_DPI_MIN;
    out->dpi_max = CONFIG_ZITAOTECH_TRACKPOINT_DPI_MAX;
    out->dpi_step = CONFIG_ZITAOTECH_TRACKPOINT_DPI_STEP;
    out->dpi_levels = ZITAOTECH_TRACKPOINT_DPI_LEVELS;
    out->burst_enabled = zitaotech_trackpoint_burst_enabled();
    out->burst_multiplier = zitaotech_trackpoint_burst_multiplier();
    out->burst_multiplier_max = CONFIG_ZITAOTECH_TRACKPOINT_BURST_MAX;
    out->axis_x_positive = zitaotech_trackpoint_axis_scale(
        ZITAOTECH_TRACKPOINT_AXIS_X_POSITIVE);
    out->axis_x_negative = zitaotech_trackpoint_axis_scale(
        ZITAOTECH_TRACKPOINT_AXIS_X_NEGATIVE);
    out->axis_y_positive = zitaotech_trackpoint_axis_scale(
        ZITAOTECH_TRACKPOINT_AXIS_Y_POSITIVE);
    out->axis_y_negative = zitaotech_trackpoint_axis_scale(
        ZITAOTECH_TRACKPOINT_AXIS_Y_NEGATIVE);
    out->axis_scale_min = CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MIN;
    out->axis_scale_max = CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MAX;
    out->accel = zitaotech_trackpoint_accel();
    out->accel_min = CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MIN;
    out->accel_max = CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MAX;
    out->scroll_speed_percent = zitaotech_trackpoint_scroll_speed_percent();
    out->scroll_speed_min = CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_MIN;
    out->scroll_speed_max = CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_MAX;
    out->scroll_burst_enabled = zitaotech_trackpoint_scroll_burst_enabled();
    out->scroll_burst_multiplier = zitaotech_trackpoint_scroll_burst_multiplier();
    out->scroll_burst_multiplier_max = CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_BURST_MAX;
    out->scroll_accel = zitaotech_trackpoint_scroll_accel();
    out->scroll_accel_min = CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MIN;
    out->scroll_accel_max = CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MAX;
    out->scroll_key = zitaotech_trackpoint_scroll_key_position();
    out->smooth_scroll_enabled = zitaotech_trackpoint_smooth_scroll_enabled();
    out->smooth_scroll_supported = true;
    out->scroll_capslock_enabled = zitaotech_trackpoint_scroll_capslock_enabled();
    out->scroll_invert_x = zitaotech_trackpoint_scroll_invert_x();
    out->scroll_invert_y = zitaotech_trackpoint_scroll_invert_y();
    out->arrow_key = zitaotech_trackpoint_mode_key_position(ZITAOTECH_TRACKPOINT_MODE_ARROW);
    out->fast_key = zitaotech_trackpoint_mode_key_position(ZITAOTECH_TRACKPOINT_MODE_FAST);
    out->slow_key = zitaotech_trackpoint_mode_key_position(ZITAOTECH_TRACKPOINT_MODE_SLOW);
    out->arrow_swipe_threshold = zitaotech_trackpoint_arrow_swipe_threshold();
    out->arrow_swipe_min = CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MIN;
    out->arrow_swipe_max = CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MAX;
}

static void fill_error(zitaotech_trackpoint_Response *resp, const char *message) {
    resp->which_response_type = zitaotech_trackpoint_Response_error_tag;
    strncpy(resp->response_type.error.message, message,
            sizeof(resp->response_type.error.message) - 1);
}

static void notify_state_changed(void) {
    int subsystem_idx = trackpoint_subsystem_index();
    if (subsystem_idx < 0) {
        return;
    }

    static zitaotech_trackpoint_Notification notification;
    notification = (zitaotech_trackpoint_Notification)zitaotech_trackpoint_Notification_init_zero;
    fill_state(&notification.state);

    raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = subsystem_idx,
        .encode_payload =
            {
                .funcs = {.encode = encode_notification_payload},
                .arg = &notification,
            },
    });
}

/*
 * The module calls this after every change; it is compiled in only here, so
 * builds without the Studio RPC keep the no-op from trackpoint.c.
 */
void zitaotech_trackpoint_state_changed(void) { notify_state_changed(); }

static bool zitaotech_trackpoint_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                                    pb_callback_t *encode_response) {
    zitaotech_trackpoint_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zitaotech__trackpoint, encode_response);

    zitaotech_trackpoint_Request req = zitaotech_trackpoint_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zitaotech_trackpoint_Request_fields, &req)) {
        LOG_WRN("Failed to decode trackpoint request: %s", PB_GET_ERROR(&req_stream));
        fill_error(resp, "Failed to decode request");
        return true;
    }

    /* Write requests answer with the new state themselves; suppress the extra
     * notification so it cannot interleave with the response frame. */
    bool is_write = req.which_request_type != zitaotech_trackpoint_Request_get_state_tag;
    if (is_write) {
        zitaotech_trackpoint_suppress_notifications(true);
    }

    switch (req.which_request_type) {
    case zitaotech_trackpoint_Request_get_state_tag:
        break;
    case zitaotech_trackpoint_Request_set_dpi_tag: {
        int rc = zitaotech_trackpoint_set_dpi_index(req.request_type.set_dpi.dpi_index);
        if (rc < 0) {
            fill_error(resp, "DPI step out of range");
            zitaotech_trackpoint_suppress_notifications(false);
            return true;
        }
        break;
    }
    case zitaotech_trackpoint_Request_set_burst_tag: {
        int rc = zitaotech_trackpoint_set_burst(req.request_type.set_burst.enabled,
                                                req.request_type.set_burst.multiplier);
        if (rc < 0) {
            fill_error(resp, "Burst multiplier out of range");
            zitaotech_trackpoint_suppress_notifications(false);
            return true;
        }
        break;
    }
    case zitaotech_trackpoint_Request_set_axis_scales_tag: {
        const zitaotech_trackpoint_SetAxisScalesRequest *req_scales =
            &req.request_type.set_axis_scales;
        const uint32_t scales[ZITAOTECH_TRACKPOINT_AXIS_COUNT] = {
            req_scales->x_positive,
            req_scales->x_negative,
            req_scales->y_positive,
            req_scales->y_negative,
        };
        int rc = zitaotech_trackpoint_set_axis_scales(scales);
        if (rc < 0) {
            fill_error(resp, rc == -ERANGE ? "Fine tuning out of range"
                                           : "Failed to set fine tuning");
            zitaotech_trackpoint_suppress_notifications(false);
            return true;
        }
        break;
    }
    case zitaotech_trackpoint_Request_set_accel_tag: {
        int rc = zitaotech_trackpoint_set_accel(req.request_type.set_accel.accel);
        if (rc < 0) {
            fill_error(resp, "Acceleration out of range");
            zitaotech_trackpoint_suppress_notifications(false);
            return true;
        }
        break;
    }
    case zitaotech_trackpoint_Request_set_scroll_tag: {
        const zitaotech_trackpoint_SetScrollRequest *scroll = &req.request_type.set_scroll;
        int rc = zitaotech_trackpoint_set_scroll(scroll->speed_percent, scroll->burst_enabled,
                                                 scroll->burst_multiplier, scroll->accel);
        if (rc < 0) {
            fill_error(resp, "Scroll settings out of range");
            zitaotech_trackpoint_suppress_notifications(false);
            return true;
        }
        break;
    }
    case zitaotech_trackpoint_Request_set_scroll_key_tag: {
        int rc = zitaotech_trackpoint_set_scroll_key_position(
            req.request_type.set_scroll_key.position);
        if (rc < 0) {
            fill_error(resp, "Key position out of range");
            zitaotech_trackpoint_suppress_notifications(false);
            return true;
        }
        break;
    }
    case zitaotech_trackpoint_Request_set_smooth_scroll_tag:
        zitaotech_trackpoint_set_smooth_scroll(req.request_type.set_smooth_scroll.enabled);
        break;
    case zitaotech_trackpoint_Request_set_scroll_options_tag: {
        const zitaotech_trackpoint_SetScrollOptionsRequest *opts =
            &req.request_type.set_scroll_options;
        zitaotech_trackpoint_set_scroll_options(opts->capslock_scroll, opts->invert_x,
                                                opts->invert_y);
        break;
    }
    case zitaotech_trackpoint_Request_set_mode_key_tag: {
        int rc = zitaotech_trackpoint_set_mode_key(
            (enum zitaotech_trackpoint_mode)req.request_type.set_mode_key.mode,
            req.request_type.set_mode_key.position);
        if (rc < 0) {
            fill_error(resp, "Invalid mode key");
            zitaotech_trackpoint_suppress_notifications(false);
            return true;
        }
        break;
    }
    case zitaotech_trackpoint_Request_set_arrow_threshold_tag: {
        int rc = zitaotech_trackpoint_set_arrow_swipe_threshold(
            req.request_type.set_arrow_threshold.threshold);
        if (rc < 0) {
            fill_error(resp, "Arrow threshold out of range");
            zitaotech_trackpoint_suppress_notifications(false);
            return true;
        }
        break;
    }
    case zitaotech_trackpoint_Request_reset_tag:
        zitaotech_trackpoint_reset();
        break;
    default:
        LOG_WRN("Unsupported trackpoint request type: %d", req.which_request_type);
        fill_error(resp, "Unsupported request");
        if (is_write) {
            zitaotech_trackpoint_suppress_notifications(false);
        }
        return true;
    }

    if (is_write) {
        zitaotech_trackpoint_suppress_notifications(false);
    }

    /* Write requests answer with the new state so the UI never has to guess. */
    resp->which_response_type = zitaotech_trackpoint_Response_state_tag;
    fill_state(&resp->response_type.state);
    return true;
}
