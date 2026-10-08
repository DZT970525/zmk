#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/os-detection/os_detection.pb.h>

#include "../os_detection_internal.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta os_detection_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-os-detection/"),
    // Unsecured is suggested by default to avoid unlocking in un-reliable
    // environments.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__os_detection, &os_detection_meta,
                         os_detection_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__os_detection, cormoran_os_detection_Response);

/* enum zmk_os starts at ZMK_OS_UNKNOWN=0; cormoran_os_detection_Os starts at
 * OS_UNSPECIFIED=0 with OS_UNKNOWN=1, so the proto value is always exactly
 * one more than the firmware value. */
static cormoran_os_detection_Os zmk_os_to_proto(enum zmk_os os) {
    return (cormoran_os_detection_Os)(os + 1);
}

/* Returns -1 for a proto value that doesn't map to a valid enum zmk_os. */
static int proto_to_zmk_os(cormoran_os_detection_Os os) {
    if (os == cormoran_os_detection_Os_OS_UNSPECIFIED) {
        return ZMK_OS_UNKNOWN;
    }
    if (os >= cormoran_os_detection_Os_OS_UNKNOWN && os <= cormoran_os_detection_Os_OS_ANDROID) {
        return (int)os - 1;
    }
    return -1;
}

static void fill_ble_profile_state(uint8_t index, cormoran_os_detection_BleProfileState *out) {
    out->index = index;
    out->bonded = zmk_os_detection_ble_profile_bonded(index);
    out->connected = zmk_os_detection_ble_profile_connected(index);
    out->detected = zmk_os_to_proto(zmk_os_detection_ble_profile_detected(index));
    out->override = zmk_os_to_proto(zmk_os_detection_ble_profile_override(index));
    out->effective = zmk_os_to_proto(zmk_os_detection_ble_profile_effective(index));
}

static void handle_get_state(cormoran_os_detection_StateResponse *out) {
    out->has_usb = true;
    out->usb.connected = zmk_os_detection_usb_connected();
    out->usb.detected = zmk_os_to_proto(zmk_os_detection_usb_detected());

    int profile_count = zmk_os_detection_ble_profile_count();
    int max_profiles = ARRAY_SIZE(out->ble_profiles);
    out->ble_profiles_count = 0;
    for (int i = 0; i < profile_count && i < max_profiles; i++) {
        fill_ble_profile_state(i, &out->ble_profiles[out->ble_profiles_count]);
        out->ble_profiles_count++;
    }

    out->active_profile_index = zmk_os_detection_selected_ble_profile_index();
    out->current_effective = zmk_os_to_proto(zmk_os_detection_current());
}

static int handle_set_ble_override(const cormoran_os_detection_SetBleOverrideRequest *req,
                                   cormoran_os_detection_Response *resp) {
    int profile_count = zmk_os_detection_ble_profile_count();
    if ((int)req->profile_index >= profile_count) {
        return -EINVAL;
    }

    int os = proto_to_zmk_os(req->os);
    if (os < 0) {
        return -EINVAL;
    }

    int rc = zmk_os_detection_set_ble_override(req->profile_index, (enum zmk_os)os);
    if (rc != 0) {
        return rc;
    }

    resp->which_response_type = cormoran_os_detection_Response_set_ble_override_tag;
    resp->response_type.set_ble_override.has_profile = true;
    fill_ble_profile_state(req->profile_index, &resp->response_type.set_ble_override.profile);
    return 0;
}

static bool os_detection_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                            pb_callback_t *encode_response) {
    cormoran_os_detection_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__os_detection, encode_response);

    cormoran_os_detection_Request req = cormoran_os_detection_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_os_detection_Request_fields, &req)) {
        LOG_WRN("Failed to decode os_detection request: %s", PB_GET_ERROR(&req_stream));
        cormoran_os_detection_ErrorResponse err = cormoran_os_detection_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = cormoran_os_detection_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case cormoran_os_detection_Request_get_state_tag:
        resp->which_response_type = cormoran_os_detection_Response_state_tag;
        handle_get_state(&resp->response_type.state);
        break;
    case cormoran_os_detection_Request_set_ble_override_tag:
        rc = handle_set_ble_override(&req.request_type.set_ble_override, resp);
        break;
    default:
        LOG_WRN("Unsupported os_detection request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        cormoran_os_detection_ErrorResponse err = cormoran_os_detection_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request");
        resp->which_response_type = cormoran_os_detection_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}

#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_TEST_INJECT)

/* Exercises the handler's actual business logic (handle_get_state /
 * handle_set_ble_override) directly, rather than round-tripping through the
 * generic nanopb bytes-field encode plumbing shared by every custom
 * subsystem in the template - that plumbing is unmodified template code,
 * not specific to this module. */
static int os_detection_handler_test_init(void) {
    cormoran_os_detection_StateResponse state = cormoran_os_detection_StateResponse_init_zero;
    handle_get_state(&state);
    LOG_INF("os detection: get_state usb_connected=%d usb_detected=%d ble_profiles_count=%d "
            "current_effective=%d",
            state.usb.connected, state.usb.detected, state.ble_profiles_count,
            state.current_effective);

    cormoran_os_detection_SetBleOverrideRequest override_req = {
        .profile_index = 0,
        .os = cormoran_os_detection_Os_OS_WINDOWS,
    };
    cormoran_os_detection_Response override_resp = cormoran_os_detection_Response_init_zero;
    int rc = handle_set_ble_override(&override_req, &override_resp);
    LOG_INF("os detection: set_ble_override(profile=0, WINDOWS) rc=%d", rc);

    return 0;
}

SYS_INIT(os_detection_handler_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_OS_DETECTION_TEST_INJECT */
