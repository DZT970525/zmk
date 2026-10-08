/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <proto/zmk/custom.pb.h>
#include <zmk/event_manager.h>
#include <zmk/studio/rpc.h>
#include <zephyr/logging/log.h>

struct zmk_studio_custom_notification {
    uint8_t subsystem_index;
    // callback to encode payload
    // Note that as for now, encode is executed within raise_zmk_studio_custom_notification(...)
    // caller can safely set pointer to local stack variable which remains until
    // raise_zmk_studio_custom_notification(...) returns.
    pb_callback_t encode_payload;
};

ZMK_EVENT_DECLARE(zmk_studio_custom_notification);

/**
 * Handler function which is called when a custom subsystem CallRequest is received.
 * @param request The CallRequest received from the studio.
 * @param encode_response pointer to CallResponse.payload's pb_callback_t.
 * The handler should set this to a function which encodes the response.
 *
 * WARNING: The encoding can be called multiple times to calculate the size
 * of the payload.
 *
 * WARNING: The encoding is executed after returning from this handler, so the pb_callback_t
 * **cannot use pointer to local stack variables**. Pointer to data segment (global/static variable)
 * or heap memory should be used. Instead of setting data pointer, the handler can also collect and
 * build response in encoder function if the operation is light-weight and does not have side
 * effects.
 *
 * NOTE: As for now, the handler is not called in parallel. Next RPC is always processed after
 * sending response. So the handler can share single global variable as data buffer to use in
 * encoder function.
 *
 * @return true if the request was handled successfully, false if failed.
 */
typedef bool(custom_subsystem_handler)(const zmk_custom_CallRequest *request,
                                       pb_callback_t *encode_response);

struct zmk_rpc_custom_subsystem_meta {
    char **ui_urls;
    size_t ui_urls_count;
    enum zmk_studio_rpc_handler_security security;
};

struct zmk_rpc_custom_subsystem {
    char *identifier;
    struct zmk_rpc_custom_subsystem_meta *meta;
    custom_subsystem_handler *handler;
};

/**
 * Registers a custom RPC subsystem.
 * @param _identifier Unique identifier string for the custom subsystem.
 * @param _meta Pointer to zmk_rpc_custom_subsystem_meta struct defining metadata.
 * @param _handler Function pointer to the handler function for CallRequests.
 */
#define ZMK_RPC_CUSTOM_SUBSYSTEM(_identifier, _meta, _handler)                                     \
    BUILD_ASSERT(sizeof(#_identifier) < CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_IDENTIFIER_MAX_LEN, \
                 "Identifier too long: " #_identifier);                                            \
    static bool _handler(const zmk_custom_CallRequest *req, pb_callback_t *res);                   \
    STRUCT_SECTION_ITERABLE(zmk_rpc_custom_subsystem, zmk_rpc_custom_subsystem_##_identifier) = {  \
        .identifier = #_identifier,                                                                \
        .meta = _meta,                                                                             \
        .handler = _handler,                                                                       \
    };

/**
 * Helper macro to define UI URLs array in zmk_rpc_custom_subsystem_meta.
 * @param ... List of string literals representing UI URLs.
 */
#define ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(...)                                                      \
    .ui_urls = (char *[]){__VA_ARGS__},                                                            \
    .ui_urls_count =                                                                               \
        COND_CODE_1(IS_EMPTY(__VA_ARGS__), (0), (UTIL_INC(NUM_VA_ARGS_LESS_1(__VA_ARGS__))))

/**
 * Function to encode response payload for custom subsystems.
 * Used in ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER macro.
 */
bool zmk_rpc_custom_subsystem_encode_response_payload(pb_ostream_t *stream,
                                                      const pb_field_t *payload_field,
                                                      const pb_msgdesc_t *custom_response_msgdesc,
                                                      void const *custom_response);

/**
 * Registers a custom RPC subsystem response buffer.
 * @param _subsystem_identifier Unique identifier string for the custom subsystem.
 * @param response_type The protobuf message type of the custom response.
 * @note The response buffer is static and shared.
 *       The encode function checks that the buffer is used only by one encoding at a time.
 */
#define ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(_subsystem_identifier, response_type)             \
    /** bufer */                                                                                   \
    static response_type zmk_rpc_custom_subsystem_response_buffer_##_subsystem_identifier;         \
    /** sequence counter for buffer usage tracking: every allocation gets a new
     * value and the encoder only accepts the one it was handed.  It has to be
     * an integer (a void* cannot be incremented). */                                              \
    static uintptr_t zmk_rpc_custom_subsystem_response_buffer_seq_##_subsystem_identifier;         \
                                                                                                   \
    static inline bool zmk_rpc_custom_subsystem_encode_response_##_subsystem_identifier(           \
        pb_ostream_t *stream, const pb_field_t *payload_field, void *const *arg) {                 \
        const void *seq = (void *)*arg;                                                            \
        if (seq !=                                                                                 \
            (void *)zmk_rpc_custom_subsystem_response_buffer_seq_##_subsystem_identifier) {        \
            LOG_DBG("response buffer is in use by other request: expected=%d current=%d",          \
                    (int)seq,                                                                      \
                    (int)zmk_rpc_custom_subsystem_response_buffer_seq_##_subsystem_identifier);    \
            return false; /* buffer was used by others before encoding */                          \
        }                                                                                          \
        return zmk_rpc_custom_subsystem_encode_response_payload(                                   \
            stream, payload_field, response_type##_fields,                                         \
            &zmk_rpc_custom_subsystem_response_buffer_##_subsystem_identifier);                    \
        /* NOTE: buffer cannot be released here because encode can be called multiple times */     \
    }                                                                                              \
    static inline response_type                                                                    \
        *zmk_rpc_custom_subsystem_response_buffer_allocate_##_subsystem_identifier(                \
            pb_callback_t *encode_response) {                                                      \
        zmk_rpc_custom_subsystem_response_buffer_##_subsystem_identifier =                         \
            (response_type)response_type##_init_zero;                                              \
        encode_response->funcs.encode =                                                            \
            zmk_rpc_custom_subsystem_encode_response_##_subsystem_identifier;                      \
        encode_response->arg = (void *)(uintptr_t)(++zmk_rpc_custom_subsystem_response_buffer_seq_##_subsystem_identifier); \
        return &zmk_rpc_custom_subsystem_response_buffer_##_subsystem_identifier;                  \
    }

/**
 * Helper macro to allocate response buffer for custom subsystems.
 * @param _subsystem_identifier Unique identifier string for the custom subsystem.
 * @param encode_response Pointer to pb_callback_t to be set for encoding response.
 *                        custom_subsystem_handler#encode_response should be passed here.
 * @return Pointer to allocated response buffer of appropriate type.
 * @note ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(_subsystem_identifier, response_type) is required
 * before using this macro.
 */
#define ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(_subsystem_identifier, encode_response)  \
    zmk_rpc_custom_subsystem_response_buffer_allocate_##_subsystem_identifier(encode_response)
