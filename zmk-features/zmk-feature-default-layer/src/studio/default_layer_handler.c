/*
 * Studio custom subsystem for the per-endpoint / per-OS default layer.
 *
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zmk/studio/custom.h>
#include <zmk/keymap.h>

#include <cormoran/default_layer/default_layer.pb.h>
#include <cormoran/zmk/default_layer.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta default_layer_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    /* Choosing a start-up layer is harmless, so no unlock dance. */
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__default_layer, &default_layer_meta,
                         default_layer_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__default_layer,
                                        cormoran_default_layer_Response);

/*
 * Both lists are repeated submessages, which nanopb encodes through a
 * callback (the generated struct only holds the callback).
 */
static bool encode_endpoints(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    uint32_t count = zmk_default_layer_endpoint_count();
    for (uint32_t index = 0; index < count; index++) {
        bool is_usb = false;
        uint32_t ble_profile = 0;

        /* Index 0 is the "nothing connected" slot; it has nothing to set. */
        if (!zmk_default_layer_endpoint_info(index, &is_usb, &ble_profile)) {
            continue;
        }

        cormoran_default_layer_EndpointState state =
            cormoran_default_layer_EndpointState_init_zero;
        state.index = index;
        state.is_usb = is_usb;
        state.ble_profile_index = ble_profile;
        state.value = zmk_default_layer_endpoint_value(index);

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, cormoran_default_layer_EndpointState_fields, &state)) {
            return false;
        }
    }

    return true;
}

static bool encode_os_layers(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    for (uint32_t os = 0; os < ZMK_DEFAULT_LAYER_OS_COUNT; os++) {
        cormoran_default_layer_OsLayerState state = cormoran_default_layer_OsLayerState_init_zero;
        state.os = os;
        state.value = zmk_default_layer_os_value(os);

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, cormoran_default_layer_OsLayerState_fields, &state)) {
            return false;
        }
    }

    return true;
}

static void fill_state(cormoran_default_layer_StateResponse *out) {
    out->endpoints.funcs.encode = encode_endpoints;
    out->os_layers.funcs.encode = encode_os_layers;
    out->active_endpoint_index = (uint32_t)MAX(0, zmk_default_layer_active_endpoint_index());
    out->current_os = zmk_default_layer_current_os();
    out->resolved_layer =
        zmk_default_layer_resolved(out->active_endpoint_index);
    out->layer_count = ZMK_KEYMAP_LAYERS_LEN;
    out->os_detection_available = IS_ENABLED(CONFIG_ZMK_OS_DETECTION);
}

static void fill_error(cormoran_default_layer_Response *resp, const char *message) {
    resp->which_response_type = cormoran_default_layer_Response_error_tag;
    strncpy(resp->response_type.error.message, message,
            sizeof(resp->response_type.error.message) - 1);
}

static bool default_layer_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                             pb_callback_t *encode_response) {
    cormoran_default_layer_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__default_layer, encode_response);

    cormoran_default_layer_Request req = cormoran_default_layer_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_default_layer_Request_fields, &req)) {
        LOG_WRN("Failed to decode default layer request: %s", PB_GET_ERROR(&req_stream));
        fill_error(resp, "Failed to decode request");
        return true;
    }

    switch (req.which_request_type) {
    case cormoran_default_layer_Request_get_state_tag:
        break;
    case cormoran_default_layer_Request_set_endpoint_layer_tag: {
        int rc = zmk_default_layer_set_endpoint_value(
            req.request_type.set_endpoint_layer.endpoint_index,
            req.request_type.set_endpoint_layer.value);
        if (rc < 0) {
            fill_error(resp, "Unknown endpoint or layer");
            return true;
        }
        break;
    }
    case cormoran_default_layer_Request_set_os_layer_tag: {
        int rc = zmk_default_layer_set_os_value(req.request_type.set_os_layer.os,
                                                req.request_type.set_os_layer.value);
        if (rc < 0) {
            fill_error(resp, "Unknown OS or layer");
            return true;
        }
        break;
    }
    default:
        LOG_WRN("Unsupported default layer request type: %d", req.which_request_type);
        fill_error(resp, "Unsupported request");
        return true;
    }

    resp->which_response_type = cormoran_default_layer_Response_state_tag;
    fill_state(&resp->response_type.state);
    return true;
}
