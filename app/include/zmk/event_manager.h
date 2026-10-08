/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>

struct zmk_event_type {
    const char *name;
};

typedef struct {
    const struct zmk_event_type *event;
    uint8_t last_listener_index;
} zmk_event_t;

#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_EV_EVENT_HANDLED 1
#define ZMK_EV_EVENT_CAPTURED 2

typedef int (*zmk_listener_callback_t)(const zmk_event_t *eh);
struct zmk_listener {
    zmk_listener_callback_t callback;
};

struct zmk_event_subscription {
    const struct zmk_event_type *event_type;
    const struct zmk_listener *listener;
};

#define ZMK_EVENT_DECLARE(event_type)                                                              \
    struct event_type##_event {                                                                    \
        zmk_event_t header;                                                                        \
        struct event_type data;                                                                    \
    };                                                                                             \
    struct event_type##_event copy_raised_##event_type(const struct event_type *ev);               \
    int raise_##event_type(struct event_type);                                                     \
    struct event_type *as_##event_type(const zmk_event_t *eh);                                     \
    extern const struct zmk_event_type zmk_event_##event_type;

#define ZMK_EVENT_IMPL(event_type)                                                                 \
    const struct zmk_event_type zmk_event_##event_type = {.name = STRINGIFY(event_type)};          \
    const struct zmk_event_type *zmk_event_ref_##event_type __used                                 \
        __attribute__((__section__(".event_type"))) = &zmk_event_##event_type;                     \
    struct event_type##_event copy_raised_##event_type(const struct event_type *ev) {              \
        struct event_type##_event *outer = CONTAINER_OF(ev, struct event_type##_event, data);      \
        return *outer;                                                                             \
    };                                                                                             \
    int raise_##event_type(struct event_type data) {                                               \
        struct event_type##_event ev = {.data = data,                                              \
                                        .header = {.event = &zmk_event_##event_type}};             \
        return ZMK_EVENT_RAISE(ev);                                                                \
    };                                                                                             \
    struct event_type *as_##event_type(const zmk_event_t *eh) {                                    \
        return (eh->event == &zmk_event_##event_type) ? &((struct event_type##_event *)eh)->data   \
                                                      : NULL;                                      \
    };

#define ZMK_LISTENER(mod, cb) const struct zmk_listener zmk_listener_##mod = {.callback = cb};

#define ZMK_SUBSCRIPTION(mod, ev_type)                                                             \
    extern const struct zmk_listener zmk_listener_##mod;                                           \
    const Z_DECL_ALIGN(struct zmk_event_subscription)                                              \
        _CONCAT(_CONCAT(zmk_event_sub_, mod), ev_type) __used                                      \
        __attribute__((__section__(".event_subscription"))) = {                                    \
            .event_type = &zmk_event_##ev_type,                                                    \
            .listener = &zmk_listener_##mod,                                                       \
    };

#define ZMK_EVENT_RAISE(ev) zmk_event_manager_raise(&(ev).header)

#define ZMK_EVENT_RAISE_AFTER(ev, mod)                                                             \
    zmk_event_manager_raise_after(&(ev).header, &zmk_listener_##mod)

#define ZMK_EVENT_RAISE_AT(ev, mod) zmk_event_manager_raise_at(&(ev).header, &zmk_listener_##mod)

#define ZMK_EVENT_RELEASE(ev) zmk_event_manager_release(&(ev).header)

int zmk_event_manager_raise(zmk_event_t *event);
int zmk_event_manager_raise_after(zmk_event_t *event, const struct zmk_listener *listener);
int zmk_event_manager_raise_at(zmk_event_t *event, const struct zmk_listener *listener);
int zmk_event_manager_release(zmk_event_t *event);

/*
 * Split event relay.
 *
 * Generic plumbing that lets an event raised on one half of a split keyboard be
 * delivered to the other half, so features like the settings RPC can keep the
 * peripheral in sync with the central.  The macros mirror the ones from
 * cormoran's ZMK fork so the upstream modules build unmodified; the transport
 * underneath is the small, single-write relay characteristic of the split
 * service (see zmk/split/transport/types.h) instead of the fork's chunked one.
 */

// indicates that the relay event originated from self
#define ZMK_RELAY_EVENT_SOURCE_SELF 0xFF

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

#include <string.h>

#include <zephyr/sys/util_macro.h>

#include <zmk/split/transport/types.h>

struct zmk_relay_event_received {
    uint8_t source;
    const char *event_name;
    const uint8_t *event_data;
    size_t event_data_size;
};

ZMK_EVENT_DECLARE(zmk_relay_event_received);

/**
 * @brief Send a relay event to the other half.
 *
 * On the central this fans the event out to every connected peripheral, on the
 * peripheral it goes to its central.  Returns 0 when the event was handed to
 * the transport, a negative errno otherwise.
 */
int zmk_split_relay_event_send(const struct zmk_split_relay_event_payload *payload);

#define __ZMK_RELAY_ASSERT_NAME(_identifier)                                                       \
    BUILD_ASSERT(sizeof(#_identifier) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN,               \
                 "Name " STRINGIFY(_identifier) " too large for relay event");

#define __ZMK_RELAY_ASSERT_SIZE(_event_type, _identifier)                                          \
    BUILD_ASSERT(sizeof(struct _event_type) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,              \
                 "Payload of " STRINGIFY(_event_type) " too large for relay event");               \
    __ZMK_RELAY_ASSERT_NAME(_identifier)

/*
 * True when the event was raised locally (rather than restored from a relayed
 * one).  The extra parentheses survive COND_CODE_1's bracket stripping so the
 * comparison cannot be broken up by a surrounding `!`.
 */
#define __ZMK_RELAY_IS_LOCAL(_ev, _source_field_name)                                              \
    COND_CODE_1(IS_EMPTY(_source_field_name), (true),                                              \
                (((_ev)->_source_field_name == ZMK_RELAY_EVENT_SOURCE_SELF)))

/**
 * @brief Define the listener that turns a relayed event back into a local one.
 * @param _event_type name of event struct type
 * @param _identifier short unique identifier for this event type in the relay
 * @param _source_field_name optional field in the event struct carrying the
 *                           source device; filled in with the sending half's id.
 */
#define ZMK_RELAY_EVENT_HANDLE(_event_type, _identifier, _source_field_name)                       \
    __ZMK_RELAY_ASSERT_SIZE(_event_type, _identifier)                                              \
                                                                                                   \
    static const char *_event_type##_relay_id = STRINGIFY(_identifier);                            \
    static int zmk_split_relay_event_listener_##_event_type##_cb(const zmk_event_t *eh) {          \
        struct zmk_relay_event_received *ev = as_zmk_relay_event_received(eh);                     \
        if (ev && strcmp(_event_type##_relay_id, ev->event_name) == 0) {                           \
            struct _event_type original_ev;                                                        \
            if (sizeof(struct _event_type) != ev->event_data_size) {                               \
                LOG_WRN("Relay event data size mismatch for event type %s: expected %d, got %d",   \
                        _event_type##_relay_id, sizeof(struct _event_type), ev->event_data_size);  \
                return ZMK_EV_EVENT_BUBBLE;                                                        \
            }                                                                                      \
            memcpy(&original_ev, ev->event_data, sizeof(struct _event_type));                      \
            COND_CODE_1(IS_EMPTY(_source_field_name), (),                                          \
                        (original_ev._source_field_name = ev->source + 1; /* 0 is central */))     \
            raise_##_event_type(original_ev);                                                      \
            return ZMK_EV_EVENT_HANDLED;                                                           \
        }                                                                                          \
        return ZMK_EV_EVENT_BUBBLE;                                                                \
    }                                                                                              \
    ZMK_LISTENER(_event_type##_relay_handle, zmk_split_relay_event_listener_##_event_type##_cb);   \
    ZMK_SUBSCRIPTION(_event_type##_relay_handle, zmk_relay_event_received);

/**
 * @brief Define the listener that forwards a local event to the other half.
 *
 * Only events raised locally (source field == self) are forwarded, so relaying
 * cannot loop.
 */
#define ZMK_RELAY_EVENT_FORWARD(_event_type, _identifier, _source_field_name)                      \
    __ZMK_RELAY_ASSERT_SIZE(_event_type, _identifier)                                              \
                                                                                                   \
    static const char *_event_type##_relay_forward_id = STRINGIFY(_identifier);                    \
    static int zmk_split_relay_forward_##_event_type##_cb(const zmk_event_t *eh) {                 \
        struct _event_type *ev = as_##_event_type(eh);                                             \
        if (!ev) {                                                                                 \
            return ZMK_EV_EVENT_BUBBLE;                                                            \
        }                                                                                          \
        if (!__ZMK_RELAY_IS_LOCAL(ev, _source_field_name)) {                                       \
            return ZMK_EV_EVENT_BUBBLE;                                                            \
        }                                                                                          \
        struct zmk_split_relay_event_payload payload = {0};                                        \
        payload.event_type_size = strlen(_event_type##_relay_forward_id);                          \
        payload.event_data_size = sizeof(struct _event_type);                                      \
        memcpy(payload.event_type, _event_type##_relay_forward_id, payload.event_type_size);       \
        memcpy(payload.event_data, ev, sizeof(struct _event_type));                                \
        zmk_split_relay_event_send(&payload);                                                      \
        return ZMK_EV_EVENT_BUBBLE;                                                                \
    }                                                                                              \
    ZMK_LISTENER(_event_type##_relay_forward, zmk_split_relay_forward_##_event_type##_cb);         \
    ZMK_SUBSCRIPTION(_event_type##_relay_forward, _event_type);

/**
 * @brief Relay an event to the peripherals (central half only).
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(_event_type, _identifier, _source_field_name)        \
    ZMK_RELAY_EVENT_FORWARD(_event_type, _identifier, _source_field_name)
#else
#define ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(_event_type, _identifier, _source_field_name)
#endif

/**
 * @brief Relay an event to the central (peripheral half only).
 */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(_event_type, _identifier, _source_field_name)        \
    ZMK_RELAY_EVENT_FORWARD(_event_type, _identifier, _source_field_name)
#else
#define ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(_event_type, _identifier, _source_field_name)
#endif

#else // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

#define ZMK_RELAY_EVENT_HANDLE(_event_type, _identifier, _source_field_name)
#define ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(_event_type, _identifier, _source_field_name)
#define ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(_event_type, _identifier, _source_field_name)

#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
