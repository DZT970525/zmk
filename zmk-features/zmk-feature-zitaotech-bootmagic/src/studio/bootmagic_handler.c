/*
 * Studio custom subsystem for ZitaoTech bootmagic.
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

#include <zmk/studio/custom.h>

#include <zitaotech/bootmagic/bootmagic.pb.h>
#include <zitaotech/zmk/bootmagic.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta zitaotech_bootmagic_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    // Unsecured: choosing a power-up key is harmless.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech__bootmagic, &zitaotech_bootmagic_meta,
                         zitaotech_bootmagic_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zitaotech__bootmagic, zitaotech_bootmagic_Response);

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const zitaotech_bootmagic_Notification *notification =
        (const zitaotech_bootmagic_Notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_submessage(stream, zitaotech_bootmagic_Notification_fields, notification);
}

static int bootmagic_subsystem_index(void) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, "zitaotech__bootmagic") == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

static void fill_state(zitaotech_bootmagic_State *out) {
    out->central_key = zitaotech_bootmagic_key_position();
    int32_t remote = zitaotech_bootmagic_remote_key();
    out->peripheral_key = remote == ZITAOTECH_BOOTMAGIC_KEY_UNKNOWN ? -1 : remote;
    out->peripheral_connected = zitaotech_bootmagic_peripheral_connected();
}

static void fill_error(zitaotech_bootmagic_Response *resp, const char *message) {
    resp->which_response_type = zitaotech_bootmagic_Response_error_tag;
    strncpy(resp->response_type.error.message, message,
            sizeof(resp->response_type.error.message) - 1);
}

static void notify_state_changed(void) {
    int subsystem_idx = bootmagic_subsystem_index();
    if (subsystem_idx < 0) {
        return;
    }

    static zitaotech_bootmagic_Notification notification;
    notification = (zitaotech_bootmagic_Notification)zitaotech_bootmagic_Notification_init_zero;
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

void zitaotech_bootmagic_state_changed(void) { notify_state_changed(); }

static bool zitaotech_bootmagic_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                                   pb_callback_t *encode_response) {
    zitaotech_bootmagic_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zitaotech__bootmagic, encode_response);

    zitaotech_bootmagic_Request req = zitaotech_bootmagic_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zitaotech_bootmagic_Request_fields, &req)) {
        LOG_WRN("Failed to decode bootmagic request: %s", PB_GET_ERROR(&req_stream));
        fill_error(resp, "Failed to decode request");
        return true;
    }

    bool is_write = req.which_request_type != zitaotech_bootmagic_Request_get_state_tag;
    if (is_write) {
        zitaotech_bootmagic_suppress_notifications(true);
    }

    switch (req.which_request_type) {
    case zitaotech_bootmagic_Request_get_state_tag:
        /*
         * Studio asks for the state whenever the page loads and on its poll.
         * If the other half is there but has never told us its key, ask it
         * again - that is what makes a peripheral powered up before the link
         * eventually show its key instead of "unassigned".
         */
        if (zitaotech_bootmagic_peripheral_connected() &&
            zitaotech_bootmagic_remote_key() == ZITAOTECH_BOOTMAGIC_KEY_UNKNOWN) {
            zitaotech_bootmagic_request_remote_key();
        }
        break;
    case zitaotech_bootmagic_Request_set_key_tag: {
        int rc = zitaotech_bootmagic_set_key(req.request_type.set_key.half ==
                                                 zitaotech_bootmagic_Half_HALF_PERIPHERAL,
                                             req.request_type.set_key.position);
        if (rc < 0) {
            fill_error(resp, rc == -ENOTCONN ? "The other half is not connected"
                                             : "Invalid key position");
            zitaotech_bootmagic_suppress_notifications(false);
            return true;
        }
        break;
    }
    default:
        LOG_WRN("Unsupported bootmagic request type: %d", req.which_request_type);
        fill_error(resp, "Unsupported request");
        if (is_write) {
            zitaotech_bootmagic_suppress_notifications(false);
        }
        return true;
    }

    if (is_write) {
        zitaotech_bootmagic_suppress_notifications(false);
    }

    resp->which_response_type = zitaotech_bootmagic_Response_state_tag;
    fill_state(&resp->response_type.state);
    return true;
}
