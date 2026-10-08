/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <string.h>
#include <zmk/studio/rpc.h>
#include <zmk/studio/custom.h>
#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(custom)

#define CUSTOM_RESPONSE(type, ...) ZMK_RPC_RESPONSE(custom, type, __VA_ARGS__)

ZMK_EVENT_IMPL(zmk_studio_custom_notification);

bool zmk_rpc_custom_subsystem_encode_response_payload(pb_ostream_t *stream,
                                                      const pb_field_t *payload_field,
                                                      const pb_msgdesc_t *custom_response_msgdesc,
                                                      void const *custom_response) {
    if (!pb_encode_tag_for_field(stream, payload_field)) {
        return false;
    }

    size_t size;
    if (!pb_get_encoded_size(&size, custom_response_msgdesc, custom_response)) {
        LOG_WRN("Failed to get encoded size for custom response");
        return false;
    }

    LOG_DBG("Encoding custom response of size %d", (int)size);

    if (!pb_encode_varint(stream, size)) {
        return false;
    }

    return pb_encode(stream, custom_response_msgdesc, custom_response);
}

static bool list_custom_subsystems_encode_ui_urls(pb_ostream_t *stream, const pb_field_t *field,
                                                  void *const *arg) {
    const struct zmk_rpc_custom_subsystem_meta *meta = (struct zmk_rpc_custom_subsystem_meta *)*arg;
    for (size_t i = 0; i < meta->ui_urls_count; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_string(stream, meta->ui_urls[i], strlen(meta->ui_urls[i]))) {
            return false;
        }
    }
    return true;
}

static bool list_custom_subsystems_encode_subsystems(pb_ostream_t *stream, const pb_field_t *field,
                                                     void *const *arg) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        zmk_custom_CustomSubsystemInfo subsystem_info = zmk_custom_CustomSubsystemInfo_init_zero;
        subsystem_info.index = i;
        strncpy(subsystem_info.identifier, custom_subsys->identifier,
                CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_IDENTIFIER_MAX_LEN);
        subsystem_info.ui_url.funcs.encode = list_custom_subsystems_encode_ui_urls;
        subsystem_info.ui_url.arg = (void *)custom_subsys->meta;
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, &zmk_custom_CustomSubsystemInfo_msg, &subsystem_info)) {
            return false;
        }
    }
    return true;
}

static zmk_studio_Response list_custom_subsystems(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_custom_ListCustomSubsystemResponse result =
        zmk_custom_ListCustomSubsystemResponse_init_zero;
    result.subsystems.funcs.encode = list_custom_subsystems_encode_subsystems;
    return CUSTOM_RESPONSE(list_custom_subsystems, result);
}

static zmk_studio_Response call(const zmk_studio_Request *req) {
    const zmk_custom_CallRequest *request = &req->subsystem.custom.request_type.call;
    { // validate service index
        size_t subsystem_count;
        STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);
        if (request->subsystem_index >= subsystem_count) {
            LOG_ERR("Invalid subsystem index: %d", request->subsystem_index);
            return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_RPC_NOT_FOUND);
        }
    }
    struct zmk_rpc_custom_subsystem *custom_subsys;
    STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, request->subsystem_index, &custom_subsys);
    if (custom_subsys->meta->security == ZMK_STUDIO_RPC_HANDLER_SECURED &&
        zmk_studio_core_get_lock_state() != ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED) {
        return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_UNLOCK_REQUIRED);
    }
    custom_subsystem_handler *handler = custom_subsys->handler;
    if (handler) {
        zmk_custom_CallResponse response = zmk_custom_CallResponse_init_zero;
        if (handler(request, &response.payload)) {
            response.subsystem_index = request->subsystem_index;
            return CUSTOM_RESPONSE(call, response);
        }
        return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_GENERIC);
    } else {
        LOG_DBG("No handler for custom subsystem %s", custom_subsys->identifier);
        return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_RPC_NOT_FOUND);
    }
}

ZMK_RPC_SUBSYSTEM_HANDLER(custom, list_custom_subsystems, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(custom, call, ZMK_STUDIO_RPC_HANDLER_UNSECURED);

static int custom_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    struct zmk_studio_custom_notification *custom_ev = as_zmk_studio_custom_notification(eh);
    if (!custom_ev) {
        return -ENOTSUP;
    }
    zmk_custom_CustomNotification data = zmk_custom_CustomNotification_init_zero;
    data.subsystem_index = custom_ev->subsystem_index;
    data.payload = custom_ev->encode_payload;
    *n = ZMK_RPC_NOTIFICATION(custom, custom_notification, data);
    return 0;
}

ZMK_RPC_EVENT_MAPPER(custom, custom_event_mapper, zmk_studio_custom_notification);

#ifdef CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_PRINT_LIST_ON_START
#include <zephyr/init.h>
static int custom_subsystem_init() {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);
    LOG_DBG("Registered Custom Subsystems (%d):", subsystem_count);
    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        LOG_DBG("  [%d] Identifier: %s, Security: %s", i, custom_subsys->identifier,
                custom_subsys->meta->security == ZMK_STUDIO_RPC_HANDLER_SECURED ? "Secured"
                                                                                : "Unsecured");
    }
    return 0;
}
SYS_INIT(custom_subsystem_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif
