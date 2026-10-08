/*
 * Studio custom subsystem for the ZitaoTech backlight (ZitaoTech Studio's
 * "Backlight" section on the Settings tab).
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

#include <zitaotech/backlight/backlight.pb.h>
#include <zitaotech/zmk/backlight.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta zitaotech_backlight_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    // Unsecured: brightness and the indicator effect are harmless, and the UI
    // is used while tuning, often before the keyboard is unlocked.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech__backlight, &zitaotech_backlight_meta,
                         zitaotech_backlight_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zitaotech__backlight, zitaotech_backlight_Response);

static void fill_state(zitaotech_backlight_State *out) {
    out->on = zitaotech_backlight_is_on();
    out->brightness = zitaotech_backlight_brightness();
    out->brightness_min = zitaotech_backlight_brightness_min();
    out->brightness_max = zitaotech_backlight_brightness_max();
    out->layer1_effect = zitaotech_backlight_layer_effect(1);
    out->layer2_effect = zitaotech_backlight_layer_effect(2);
    out->layer3_effect = zitaotech_backlight_layer_effect(3);
    out->layer4_effect = zitaotech_backlight_layer_effect(4);
}

/* ---------------------------------------------------------------------------
 * Live updates: the &bl keys and the layer 2 C / N brightness keys change the
 * backlight without Studio asking, so those changes are pushed as a
 * notification carrying the full state.
 * ------------------------------------------------------------------------ */

static zitaotech_backlight_State notification_state;

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const zitaotech_backlight_Notification *notification =
        (const zitaotech_backlight_Notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_submessage(stream, zitaotech_backlight_Notification_fields, notification);
}

static int backlight_subsystem_index(void) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, "zitaotech__backlight") == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

/* Overrides the weak hook in src/backlight.c. */
void zitaotech_backlight_state_changed(void) {
    int index = backlight_subsystem_index();
    if (index < 0) {
        return;
    }

    fill_state(&notification_state);
    zitaotech_backlight_Notification notification = zitaotech_backlight_Notification_init_zero;
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

static void fill_error(zitaotech_backlight_Response *resp, const char *message) {
    resp->which_response_type = zitaotech_backlight_Response_error_tag;
    strncpy(resp->response_type.error.message, message,
            sizeof(resp->response_type.error.message) - 1);
}

static bool zitaotech_backlight_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                                   pb_callback_t *encode_response) {
    zitaotech_backlight_Response *resp = ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(
        zitaotech__backlight, encode_response);

    zitaotech_backlight_Request req = zitaotech_backlight_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zitaotech_backlight_Request_fields, &req)) {
        LOG_WRN("Failed to decode backlight request: %s", PB_GET_ERROR(&req_stream));
        fill_error(resp, "Failed to decode request");
        return true;
    }

    /* Write requests answer with the new state themselves; only changes made on
     * the keyboard itself need a push notification. */
    bool is_write = req.which_request_type != zitaotech_backlight_Request_get_state_tag;
    if (is_write) {
        zitaotech_backlight_suppress_notifications(true);
    }

    switch (req.which_request_type) {
    case zitaotech_backlight_Request_get_state_tag:
        resp->which_response_type = zitaotech_backlight_Response_state_tag;
        fill_state(&resp->response_type.state);
        return true;
    case zitaotech_backlight_Request_set_on_tag: {
        int rc = zitaotech_backlight_set_on(req.request_type.set_on.on);
        if (rc < 0) {
            fill_error(resp, "Failed to switch the backlight");
            return true;
        }
        break;
    }
    case zitaotech_backlight_Request_set_brightness_tag: {
        int rc = zitaotech_backlight_set_brightness((uint8_t)req.request_type.set_brightness.brightness);
        if (rc < 0) {
            fill_error(resp, "Brightness out of range");
            return true;
        }
        break;
    }
    case zitaotech_backlight_Request_set_layer_effect_tag: {
        int rc = zitaotech_backlight_set_layer_effect(req.request_type.set_layer_effect.layer,
                                                      req.request_type.set_layer_effect.effect);
        if (rc < 0) {
            fill_error(resp, "Invalid layer or effect");
            return true;
        }
        break;
    }
    default:
        LOG_WRN("Unsupported backlight request type: %d", req.which_request_type);
        fill_error(resp, "Unsupported request");
        if (is_write) {
            zitaotech_backlight_suppress_notifications(false);
        }
        return true;
    }

    if (is_write) {
        zitaotech_backlight_suppress_notifications(false);
    }

    /* Write requests answer with the new state so the UI never has to guess. */
    resp->which_response_type = zitaotech_backlight_Response_state_tag;
    fill_state(&resp->response_type.state);
    return true;
}
