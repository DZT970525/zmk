/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>

#include <cormoran/runtime_combo/runtime_combo.h>
#include <cormoran/runtime_combo/runtime_combo.pb.h>
#include <cormoran/zmk/custom_settings.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static bool runtime_combo_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                             pb_callback_t *encode_response);

static struct zmk_rpc_custom_subsystem_meta runtime_combo_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-runtime-combo/"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__runtime_combo, &runtime_combo_meta,
                         runtime_combo_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__runtime_combo, cormoran_runtime_combo_Response);

static void set_error(cormoran_runtime_combo_Response *resp, const char *message) {
    cormoran_runtime_combo_ErrorResponse err = cormoran_runtime_combo_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = cormoran_runtime_combo_Response_error_tag;
    resp->response_type.error = err;
}

static void set_errno_error(cormoran_runtime_combo_Response *resp, const char *op, int err) {
    cormoran_runtime_combo_ErrorResponse error = cormoran_runtime_combo_ErrorResponse_init_zero;
    snprintf(error.message, sizeof(error.message), "%s failed: %d", op, err);
    resp->which_response_type = cormoran_runtime_combo_Response_error_tag;
    resp->response_type.error = error;
}

static cormoran_runtime_combo_SlowReleaseOverride
slow_release_override_to_proto(enum zmk_runtime_combo_slow_release_override value) {
    switch (value) {
    case ZMK_RUNTIME_COMBO_SLOW_RELEASE_ON:
        return cormoran_runtime_combo_SlowReleaseOverride_SLOW_RELEASE_OVERRIDE_ON;
    case ZMK_RUNTIME_COMBO_SLOW_RELEASE_OFF:
        return cormoran_runtime_combo_SlowReleaseOverride_SLOW_RELEASE_OVERRIDE_OFF;
    default:
        return cormoran_runtime_combo_SlowReleaseOverride_SLOW_RELEASE_OVERRIDE_INHERIT;
    }
}

static enum zmk_runtime_combo_slow_release_override
slow_release_override_from_proto(cormoran_runtime_combo_SlowReleaseOverride value) {
    switch (value) {
    case cormoran_runtime_combo_SlowReleaseOverride_SLOW_RELEASE_OVERRIDE_ON:
        return ZMK_RUNTIME_COMBO_SLOW_RELEASE_ON;
    case cormoran_runtime_combo_SlowReleaseOverride_SLOW_RELEASE_OVERRIDE_OFF:
        return ZMK_RUNTIME_COMBO_SLOW_RELEASE_OFF;
    default:
        return ZMK_RUNTIME_COMBO_SLOW_RELEASE_INHERIT;
    }
}

static cormoran_runtime_combo_ComboSource combo_source(uint32_t index) {
    bool has_default = zmk_runtime_combo_has_default(index);
    bool has_override = zmk_runtime_combo_has_override(index);
    if (has_default && has_override) {
        return cormoran_runtime_combo_ComboSource_COMBO_SOURCE_OVERRIDDEN;
    }
    if (has_default) {
        return cormoran_runtime_combo_ComboSource_COMBO_SOURCE_DEFAULT;
    }
    if (has_override) {
        return cormoran_runtime_combo_ComboSource_COMBO_SOURCE_RUNTIME;
    }
    return cormoran_runtime_combo_ComboSource_COMBO_SOURCE_EMPTY;
}

static int fill_combo_message(uint32_t index, cormoran_runtime_combo_Combo *message) {
    struct zmk_runtime_combo_config combo;
    int ret = zmk_runtime_combo_read(index, &combo);
    if (ret < 0) {
        return ret;
    }

    *message = (cormoran_runtime_combo_Combo)cormoran_runtime_combo_Combo_init_zero;
    message->index = index;
    message->enabled = combo.enabled;
    message->layer_mask = combo.layer_mask;
    message->key_positions_count = combo.key_position_len;
    for (uint8_t i = 0; i < combo.key_position_len; i++) {
        message->key_positions[i] = combo.key_positions[i];
    }
    message->timeout_ms = combo.timeout_ms;
    message->require_prior_idle_ms = combo.require_prior_idle_ms;
    message->slow_release_override = slow_release_override_to_proto(combo.slow_release_override);
    message->source = combo_source(index);

    zmk_runtime_combo_read_name(index, message->name, sizeof(message->name));

    message->has_behavior = true;
    if (combo.behavior.behavior_dev) {
        message->behavior.behavior_id = zmk_behavior_get_local_id(combo.behavior.behavior_dev);
        message->behavior.param1 = combo.behavior.param1;
        message->behavior.param2 = combo.behavior.param2;
    }

    return 0;
}

static int handle_list_combos(cormoran_runtime_combo_Response *resp) {
    cormoran_runtime_combo_ListCombosResponse result =
        cormoran_runtime_combo_ListCombosResponse_init_zero;
    uint32_t count = MIN(zmk_runtime_combo_count(), zmk_runtime_combo_max_count());

    for (uint32_t i = 0; i < count; i++) {
        int ret = fill_combo_message(i, &result.combos[result.combos_count]);
        if (ret == -ENOENT) {
            continue;
        }
        if (ret < 0) {
            return ret;
        }
        result.combos_count++;
    }

    resp->which_response_type = cormoran_runtime_combo_Response_list_combos_tag;
    resp->response_type.list_combos = result;
    return 0;
}

static int handle_get_combo(const cormoran_runtime_combo_GetComboRequest *req,
                            cormoran_runtime_combo_Response *resp) {
    cormoran_runtime_combo_GetComboResponse result =
        cormoran_runtime_combo_GetComboResponse_init_zero;
    int ret = fill_combo_message(req->index, &result.combo);
    if (ret < 0) {
        return ret;
    }

    result.has_combo = true;
    resp->which_response_type = cormoran_runtime_combo_Response_get_combo_tag;
    resp->response_type.get_combo = result;
    return 0;
}

static int request_to_combo(const cormoran_runtime_combo_SetComboRequest *req,
                            struct zmk_runtime_combo_config *combo) {
    if (req->index >= zmk_runtime_combo_max_count() || req->key_positions_count < 2 ||
        req->key_positions_count > CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO ||
        !req->has_behavior || req->behavior.behavior_id > UINT16_MAX ||
        req->timeout_ms > UINT16_MAX || req->require_prior_idle_ms > UINT16_MAX) {
        return -EINVAL;
    }

    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(req->behavior.behavior_id);
    if (!behavior_name) {
        return -ENODEV;
    }

    *combo = (struct zmk_runtime_combo_config){
        .enabled = req->enabled,
        .key_position_len = req->key_positions_count,
        .layer_mask = req->layer_mask,
        .behavior =
            {
                .behavior_dev = behavior_name,
                .param1 = req->behavior.param1,
                .param2 = req->behavior.param2,
            },
        .timeout_ms = req->timeout_ms,
        .require_prior_idle_ms = req->require_prior_idle_ms,
        .slow_release_override = slow_release_override_from_proto(req->slow_release_override),
    };

    for (uint8_t i = 0; i < req->key_positions_count; i++) {
        if (req->key_positions[i] > UINT16_MAX) {
            return -ERANGE;
        }
        combo->key_positions[i] = req->key_positions[i];
    }
    return 0;
}

static int handle_get_global_settings(cormoran_runtime_combo_Response *resp) {
    struct zmk_runtime_combo_global_settings settings;
    int ret = zmk_runtime_combo_read_global_settings(&settings);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_combo_GetGlobalSettingsResponse result =
        cormoran_runtime_combo_GetGlobalSettingsResponse_init_zero;
    result.has_settings = true;
    result.settings.timeout_ms = settings.timeout_ms;
    result.settings.slow_release = settings.slow_release;
    result.settings.max_combo = settings.max_combo;
    result.settings.require_prior_idle_ms = settings.require_prior_idle_ms;

    resp->which_response_type = cormoran_runtime_combo_Response_get_global_settings_tag;
    resp->response_type.get_global_settings = result;
    return 0;
}

static void set_status(cormoran_runtime_combo_Response *resp, const char *message,
                       uint32_t affected_count) {
    cormoran_runtime_combo_StatusResponse status = cormoran_runtime_combo_StatusResponse_init_zero;
    status.affected_count = affected_count;
    snprintf(status.message, sizeof(status.message), "%s", message);
    resp->which_response_type = cormoran_runtime_combo_Response_status_tag;
    resp->response_type.status = status;
}

static int handle_set_combo(const cormoran_runtime_combo_SetComboRequest *req,
                            cormoran_runtime_combo_Response *resp) {
    struct zmk_runtime_combo_config combo;
    int ret = request_to_combo(req, &combo);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_runtime_combo_write(req->index, &combo, req->persist);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Combo written", 1);
    return 0;
}

static int handle_set_combo_name(const cormoran_runtime_combo_SetComboNameRequest *req,
                                 cormoran_runtime_combo_Response *resp) {
    int ret = zmk_runtime_combo_write_name(req->index, req->name, req->persist);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Combo name written", 1);
    return 0;
}

static int handle_delete_combo(const cormoran_runtime_combo_DeleteComboRequest *req,
                               cormoran_runtime_combo_Response *resp) {
    int ret = zmk_runtime_combo_delete(req->index, req->persist);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Combo disabled", 1);
    return 0;
}

static int handle_set_timeout_ms(const cormoran_runtime_combo_SetTimeoutMsRequest *req,
                                 cormoran_runtime_combo_Response *resp) {
    if (req->timeout_ms == 0 || req->timeout_ms > UINT16_MAX) {
        return -EINVAL;
    }

    int ret = zmk_runtime_combo_write_timeout_ms((uint16_t)req->timeout_ms, req->persist);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Runtime combo timeout written", 1);
    return 0;
}

static int handle_set_slow_release(const cormoran_runtime_combo_SetSlowReleaseRequest *req,
                                   cormoran_runtime_combo_Response *resp) {
    int ret = zmk_runtime_combo_write_slow_release(req->slow_release, req->persist);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Runtime combo slow release written", 1);
    return 0;
}

static int
handle_set_require_prior_idle_ms(const cormoran_runtime_combo_SetRequirePriorIdleMsRequest *req,
                                 cormoran_runtime_combo_Response *resp) {
    if (req->require_prior_idle_ms > UINT16_MAX) {
        return -EINVAL;
    }

    int ret = zmk_runtime_combo_write_require_prior_idle_ms((uint16_t)req->require_prior_idle_ms,
                                                            req->persist);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Runtime combo require-prior-idle written", 1);
    return 0;
}

static int handle_reset_combo(const cormoran_runtime_combo_ResetComboRequest *req,
                              cormoran_runtime_combo_Response *resp) {
    int ret = zmk_runtime_combo_reset(req->index);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Combo reset to default", 1);
    return 0;
}

static int handle_save(cormoran_runtime_combo_Response *resp) {
    uint32_t affected_count = 0;
    int ret =
        zmk_custom_settings_save_scope(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, NULL, NULL, &affected_count);
    /* save_scope() doesn't itself change any in-memory value, but the
     * decoded-combo cache is rebuilt here too for the same reason as
     * discard: neither operation runs through
     * zmk_runtime_combo_write()/reset(), so the cache would otherwise never
     * learn about it. */
    zmk_runtime_combo_invalidate_cache();
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Runtime combo settings saved", affected_count);
    return 0;
}

static int handle_discard(cormoran_runtime_combo_Response *resp) {
    uint32_t affected_count = 0;
    int ret = zmk_custom_settings_discard_scope(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, NULL, NULL,
                                                &affected_count);
    zmk_runtime_combo_invalidate_cache();
    if (ret < 0) {
        return ret;
    }

    set_status(resp, "Runtime combo settings discarded", affected_count);
    return 0;
}

static bool runtime_combo_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                             pb_callback_t *encode_response) {
    cormoran_runtime_combo_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__runtime_combo, encode_response);

    cormoran_runtime_combo_Request req = cormoran_runtime_combo_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);

    if (!pb_decode(&req_stream, cormoran_runtime_combo_Request_fields, &req)) {
        LOG_WRN("Failed to decode runtime combo request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    int ret = 0;
    switch (req.which_request_type) {
    case cormoran_runtime_combo_Request_list_combos_tag:
        ret = handle_list_combos(resp);
        break;
    case cormoran_runtime_combo_Request_get_combo_tag:
        ret = handle_get_combo(&req.request_type.get_combo, resp);
        break;
    case cormoran_runtime_combo_Request_set_combo_tag:
        ret = handle_set_combo(&req.request_type.set_combo, resp);
        break;
    case cormoran_runtime_combo_Request_set_combo_name_tag:
        ret = handle_set_combo_name(&req.request_type.set_combo_name, resp);
        break;
    case cormoran_runtime_combo_Request_delete_combo_tag:
        ret = handle_delete_combo(&req.request_type.delete_combo, resp);
        break;
    case cormoran_runtime_combo_Request_get_global_settings_tag:
        ret = handle_get_global_settings(resp);
        break;
    case cormoran_runtime_combo_Request_set_timeout_ms_tag:
        ret = handle_set_timeout_ms(&req.request_type.set_timeout_ms, resp);
        break;
    case cormoran_runtime_combo_Request_set_slow_release_tag:
        ret = handle_set_slow_release(&req.request_type.set_slow_release, resp);
        break;
    case cormoran_runtime_combo_Request_save_tag:
        ret = handle_save(resp);
        break;
    case cormoran_runtime_combo_Request_discard_tag:
        ret = handle_discard(resp);
        break;
    case cormoran_runtime_combo_Request_set_require_prior_idle_ms_tag:
        ret = handle_set_require_prior_idle_ms(&req.request_type.set_require_prior_idle_ms, resp);
        break;
    case cormoran_runtime_combo_Request_reset_combo_tag:
        ret = handle_reset_combo(&req.request_type.reset_combo, resp);
        break;
    default:
        ret = -ENOTSUP;
        break;
    }

    if (ret < 0) {
        set_errno_error(resp, "Runtime combo request", ret);
    }
    return true;
}
