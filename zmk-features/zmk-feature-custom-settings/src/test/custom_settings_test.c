/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <cormoran/zmk/custom_settings.h>
#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Generous enough to also hold every "behavior/local_id/N" entry ZMK's own
 * behavior subsystem persists once test_behavior_value_type() loads the
 * "behavior" settings subtree (one entry per compiled-in behavior, ~30 for
 * the standard behaviors.dtsi set), on top of this file's own settings. */
#define TEST_SETTINGS_STORAGE_CAPACITY 64

struct test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE];
    size_t len;
};

static struct test_settings_record test_settings_storage[TEST_SETTINGS_STORAGE_CAPACITY];

static struct test_settings_record *test_settings_find_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        if (test_settings_storage[i].present &&
            strncmp(test_settings_storage[i].name, name, sizeof(test_settings_storage[i].name)) ==
                0) {
            return &test_settings_storage[i];
        }
    }

    return NULL;
}

static bool test_settings_has_record(const char *name) {
    return test_settings_find_record(name) != NULL;
}

static ssize_t test_settings_read_cb(void *cb_arg, void *data, size_t len) {
    const struct test_settings_record *record = cb_arg;
    size_t read_len = MIN(record->len, len);

    memcpy(data, record->data, read_len);
    return read_len;
}

static int test_settings_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        struct test_settings_record *record = &test_settings_storage[i];
        if (!record->present) {
            continue;
        }

        int ret = settings_call_set_handler(record->name, record->len, test_settings_read_cb,
                                            record, arg);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }

    return first_error;
}

static int test_settings_save(struct settings_store *cs, const char *name, const char *value,
                              size_t val_len) {
    ARG_UNUSED(cs);

    struct test_settings_record *record = test_settings_find_record(name);
    if (value == NULL) {
        if (record) {
            record->present = false;
        }
        return 0;
    }

    if (val_len > CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE) {
        return -EMSGSIZE;
    }

    if (strlen(name) >= SETTINGS_MAX_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    if (!record) {
        for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
            if (!test_settings_storage[i].present) {
                record = &test_settings_storage[i];
                break;
            }
        }
    }

    if (!record) {
        return -ENOMEM;
    }

    record->present = true;
    strcpy(record->name, name);
    memcpy(record->data, value, val_len);
    record->len = val_len;
    return 0;
}

static const struct settings_store_itf test_settings_itf = {
    .csi_load = test_settings_load,
    .csi_save = test_settings_save,
};

static struct settings_store test_settings_store = {
    .cs_itf = &test_settings_itf,
};

static int test_settings_backend_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }

    settings_src_register(&test_settings_store);
    settings_dst_register(&test_settings_store);
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
static struct zmk_rpc_custom_subsystem_meta test_custom_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool test_custom_settings_handle_request(const zmk_custom_CallRequest *req,
                                                pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);

    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(test, &test_custom_settings_meta, test_custom_settings_handle_request);
#endif

/* Count zmk_custom_settings_initialized firings to prove it is raised exactly
 * once (at the boot settings_load), and never again despite the many
 * settings_load_subtree reloads the discard-based tests below perform. */
static uint32_t initialized_event_count;

static int test_initialized_event_listener(const zmk_event_t *eh) {
    if (as_zmk_custom_settings_initialized(eh) != NULL) {
        initialized_event_count++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_settings_test_initialized, test_initialized_event_listener);
ZMK_SUBSCRIPTION(custom_settings_test_initialized, zmk_custom_settings_initialized);

int test_bytes_rpc_reverse(const struct zmk_custom_setting *setting, const uint8_t *src,
                           size_t src_size, uint8_t *dest, size_t *dest_size,
                           size_t dest_capacity) {
    ARG_UNUSED(setting);

    if (src_size > dest_capacity) {
        return -EMSGSIZE;
    }

    for (size_t i = 0; i < src_size; i++) {
        dest[i] = src[src_size - i - 1];
    }
    *dest_size = src_size;
    return 0;
}

ZMK_CUSTOM_SETTING_DEFINE(test_int_setting, "test", "int_value",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(10),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS(
    test_bytes_setting, "test", "bytes_value", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    ZMK_CUSTOM_SETTING_VALUE_BYTES(1, 2, 3), ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    test_bytes_rpc_reverse, test_bytes_rpc_reverse, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/* A NON-large STRING setting (no per-setting backing buffer): a value larger
 * than the fixed carrier must be rejected, not silently truncated (issue #16
 * follow-up). */
ZMK_CUSTOM_SETTING_DEFINE(test_string_setting, "test", "string_value",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                          ZMK_CUSTOM_SETTING_VALUE_STRING("hi"),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/* One registration for the whole 3-element array, replacing the pre-P3
 * ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(test_array_setting_0/1/2, ...)
 * trio (see the removal note in custom_settings.h). Defaults {1, 2, 3}
 * match the old per-element defaults exactly so existing assertions in
 * test_array_lifecycle/test_temporary_override_pool keep working unchanged. */
ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(test_array_setting_defaults, 1, 2, 3);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(test_array_setting, "test", "array_value",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, 3, 3,
                                test_array_setting_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

/* Bug #21 view-pool eviction test (test_view_pool_temp_slot_leak) needs more
 * distinct (array, index) element views than
 * CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE (default 16) so it can fill
 * the pool and force array_view_acquire() to recycle a slot. array_value's
 * max size of 3 is far too small, so this dedicated array provides plenty of
 * elements. All elements are active (default_size == max_size) so a
 * temporary override can be written to any of them. No constraint so any
 * int32 value is accepted. */
ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(test_view_pool_defaults, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(test_view_pool_array, "test", "view_pool",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, 20, 20,
                                test_view_pool_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/* Scalar filler used by the temp-slot leak/unregister tests to occupy
 * temporary-override pool slots without perturbing the settings exercised by
 * other tests. Unconstrained so any int32 value is accepted. */
ZMK_CUSTOM_SETTING_DEFINE(test_temp_filler_setting, "test", "temp_filler",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_DEFINE(test_hid_usage_setting, "test", "hid_usage",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(4),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_HID_USAGE(HID_USAGE_KEY, 4, 29));

ZMK_CUSTOM_SETTING_DEFINE(test_layer_id_setting, "test", "layer_id",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_LAYER_ID);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
ZMK_CUSTOM_SETTING_DEFINE(test_behavior_id_setting, "test", "behavior_id",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_BEHAVIOR_ID);

ZMK_CUSTOM_SETTING_DEFINE(test_behavior_setting, "test", "behavior_value",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
                          ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(0, 0, 0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#endif

struct test_profile {
    int32_t speed;
    bool invert;
    char label[8];
};

ZMK_CUSTOM_SETTING_RECORD_RANGE_INT32_DEFINE(test_profile_speed_range, 0, 100);

ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE(
    test_profile_schema, struct test_profile,
    ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32(struct test_profile, speed, 1, &test_profile_speed_range),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_BOOL(struct test_profile, invert, 2),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_STRING(struct test_profile, label, 3));

/* Default value is just the TLV version byte (no fields), decodes to "no
 * change" against caller-provided defaults. */
ZMK_CUSTOM_SETTING_DEFINE(test_record_setting, "test", "record_value",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/*
 * Simplification P4 layout self-checks (compile-time, via sizeof):
 *
 * 1. The per-setting RAM state block stays small: <= 24 bytes with 32-bit
 *    pointers (the design target; flags + temp_slot + a 12-byte value union
 *    + 2 pointers), scaling only through its two pointers/union on LP64
 *    hosts (native_sim on a 64-bit machine: <= 40).
 * 2. A BOOL (or any scalar) setting emits NO value buffer at all: its
 *    macro-emitted `_store` array is zero-length, so the whole per-setting
 *    RAM cost is exactly the state block.
 */
ZMK_CUSTOM_SETTING_DEFINE(test_bool_probe_setting, "test", "bool_probe",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, ZMK_CUSTOM_SETTING_VALUE_BOOL(false),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

BUILD_ASSERT(sizeof(struct zmk_custom_setting_state) <= (sizeof(void *) == 4 ? 24 : 40),
             "P4: per-setting RAM state block grew past the design budget");
BUILD_ASSERT(sizeof(test_bool_probe_setting_store) == 0,
             "P4: a BOOL setting must not emit a value store buffer");
BUILD_ASSERT(ZMK_CUSTOM_SETTING_VALUE_STORE_SIZE(ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                                                 CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) ==
                 CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1,
             "P4: a STRING setting's fixed store must reserve capacity + NUL");

/* Simplification P3: an RPC-creatable keyspace for user-created entries
 * under the "macro/" prefix. max_key_len (16) is a keyspace-local limit
 * bounded by (but need not equal) CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN (48,
 * see native_sim.conf) - "macro/" (6) plus a short suffix plus NUL
 * comfortably fits 16. Keyspaces no longer take a _default_value argument
 * (P3: a slot's blob always carries its own user key, so there is no shared
 * static default that could represent a not-yet-existing entry's key - see
 * the keyspace design comment in the header). */
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(test_macro_keyspace, "test", "macro/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                   /* max_size = */ sizeof(int32_t), /* max_key_len = */ 16,
                                   /* max_entries = */ 3,
                                   ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/*
 * Simplification P1 (Change 1, task 3): a keyspace whose max_size exceeds the
 * fixed carrier draws every slot's region from a per-keyspace
 * ZMK_CUSTOM_SETTING_LARGE_POOL instead of a dedicated value_bufs row -
 * exactly like ZMK_CUSTOM_SETTING_DEFINE_POOLED. Since simplification P3,
 * every slot's blob (key + payload combined) is pool-backed unconditionally,
 * sized max_entries * (max_key_len + max_size) = 2 * (16 + 100) = 232 bytes -
 * enough for two simultaneous full-size entries plus their (short) keys.
 */
#define TEST_KEYSPACE_LARGE_MAX_SIZE 100

ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(test_blob_keyspace, "test", "blob/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                   /* max_size = */ TEST_KEYSPACE_LARGE_MAX_SIZE,
                                   /* max_key_len = */ 16,
                                   /* max_entries = */ 2,
                                   ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/*
 * Enabler A (for zmk-feature-runtime-macro's keyspace migration): a keyspace
 * whose pool is deliberately smaller than the worst-case max_entries *
 * (max_key_len + max_size) budget - the over-commit promised by
 * docs/design/simplification-redesign.md §5.3 ("a pool smaller than
 * max_entries × max_size lets a keyspace hold many small entries or few
 * large ones"). max_entries=8 but the pool only holds 3 full-size entries
 * worth of bytes, so create() must start failing with -ENOSPC well before
 * the 8-entry slot-count ceiling is reached.
 */
#define TEST_OVERCOMMIT_MAX_SIZE 16
#define TEST_OVERCOMMIT_MAX_KEY_LEN 12
#define TEST_OVERCOMMIT_MAX_ENTRIES 8
#define TEST_OVERCOMMIT_POOL_SIZE (3 * (TEST_OVERCOMMIT_MAX_KEY_LEN + TEST_OVERCOMMIT_MAX_SIZE))

ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE(
    test_overcommit_keyspace, "test", "oc/", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    TEST_OVERCOMMIT_MAX_SIZE, TEST_OVERCOMMIT_MAX_KEY_LEN, TEST_OVERCOMMIT_MAX_ENTRIES,
    TEST_OVERCOMMIT_POOL_SIZE, ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/*
 * issue #16: a BYTES setting able to store a value much larger than the fixed
 * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE carrier, using its own right-sized
 * per-setting backing buffer (ZMK_CUSTOM_SETTING_DEFINE_SIZED). Large values
 * are reachable via zmk_custom_setting_write_bytes / read_into and the chunked
 * RPC only.
 */
#define TEST_LARGE_VALUE_SIZE 256

ZMK_CUSTOM_SETTING_DEFINE_SIZED(test_large_bytes_setting, TEST_LARGE_VALUE_SIZE, "test",
                                "large_bytes", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                ZMK_CUSTOM_SETTING_VALUE_BYTES(0),
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/* A record whose TLV encoding exceeds 64 bytes, stored in a large-sized BYTES
 * setting so it can round-trip past the old ceiling (issue #16, point 6). */
struct test_big_profile {
    int32_t id;
    char note[200];
};

ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE(
    test_big_profile_schema, struct test_big_profile,
    ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32(struct test_big_profile, id, 1, NULL),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_STRING(struct test_big_profile, note, 2));

ZMK_CUSTOM_SETTING_DEFINE_SIZED(test_big_record_setting, TEST_LARGE_VALUE_SIZE, "test",
                                "big_record", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/* The large-value backing buffer must live outside the descriptor (as a
 * pointer), so defining a 256-byte setting does not embed 256 bytes into
 * every struct zmk_custom_setting. If it did, this descriptor would be at
 * least as large as one buffer. */
BUILD_ASSERT(sizeof(struct zmk_custom_setting) < CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,
             "large value buffer must not be embedded in the setting descriptor");

/*
 * Shared large-value pool: several BYTES/STRING settings can be registered
 * against one static ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE budget instead of
 * each paying for its own worst-case max_size buffer
 * (ZMK_CUSTOM_SETTING_DEFINE_SIZED). Deliberately over-committed - two
 * 128-byte BYTES members plus a 32-byte STRING member add up to far more
 * than the 192-byte pool - so writes can legitimately fail with -ENOSPC and
 * exercise compaction (see test_large_value_pool).
 */
#define TEST_POOL_SIZE 192
#define TEST_POOLED_BYTES_MAX_SIZE 128
#define TEST_POOLED_STRING_MAX_SIZE 32

/* A genuinely empty BYTES value (size 0) - ZMK_CUSTOM_SETTING_VALUE_BYTES(0)
 * is NOT empty, it is a one-byte value {0}, so it cannot be used here to
 * exercise "an empty default must not allocate a pool region". */
#define TEST_POOLED_EMPTY_BYTES                                                                    \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, .size = 0})

ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(test_large_pool, TEST_POOL_SIZE);

ZMK_CUSTOM_SETTING_DEFINE_POOLED(test_pooled_a, TEST_POOLED_BYTES_MAX_SIZE, test_large_pool, "test",
                                 "pooled_a", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                 TEST_POOLED_EMPTY_BYTES,
                                 ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_DEFINE_POOLED(test_pooled_b, TEST_POOLED_BYTES_MAX_SIZE, test_large_pool, "test",
                                 "pooled_b", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                 TEST_POOLED_EMPTY_BYTES,
                                 ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_DEFINE_POOLED(test_pooled_c, TEST_POOLED_STRING_MAX_SIZE, test_large_pool,
                                 "test", "pooled_c", ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                                 ZMK_CUSTOM_SETTING_VALUE_STRING(""),
                                 ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                 ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

static int expect_int_value(const struct zmk_custom_setting *setting, int32_t expected) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read(setting, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != expected) {
        LOG_ERR("Unexpected custom setting value: expected %d, got %d", expected,
                value.int32_value);
        return -EINVAL;
    }

    return 0;
}

static int expect_int_value_by_key(const char *custom_subsystem_id, const char *key,
                                   int32_t expected) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_by_key(custom_subsystem_id, key, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != expected) {
        LOG_ERR("Unexpected custom setting value for %s/%s: expected %d, got %d",
                custom_subsystem_id, key, expected, value.int32_value);
        return -EINVAL;
    }

    return 0;
}

static int expect_array_int_value_by_key(const char *custom_subsystem_id, const char *key,
                                         uint32_t index, int32_t expected) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(custom_subsystem_id, key, index, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != expected) {
        LOG_ERR("Unexpected custom setting value for %s/%s/%u: expected %d, got %d",
                custom_subsystem_id, key, index, expected, value.int32_value);
        return -EINVAL;
    }

    return 0;
}

static int expect_bytes_value(const struct zmk_custom_setting_value *value, const uint8_t *expected,
                              size_t expected_size) {
    if (value->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES || value->size != expected_size ||
        memcmp(value->bytes_value, expected, expected_size) != 0) {
        LOG_ERR("Unexpected custom bytes setting value");
        return -EINVAL;
    }

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
static int expect_behavior_value(const struct zmk_custom_setting *setting,
                                 struct zmk_custom_setting_behavior_value expected) {
    struct zmk_custom_setting_behavior_value value;
    int ret = zmk_custom_setting_get_behavior(setting, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.behavior_id != expected.behavior_id || value.param1 != expected.param1 ||
        value.param2 != expected.param2) {
        LOG_ERR("Unexpected custom behavior setting value: expected id=%u p1=%u p2=%u, got id=%u "
                "p1=%u p2=%u",
                expected.behavior_id, expected.param1, expected.param2, value.behavior_id,
                value.param1, value.param2);
        return -EINVAL;
    }

    return 0;
}
#endif

static int test_constraint_validation(void) {
    const struct zmk_custom_setting *hid_usage = zmk_custom_setting_find("test", "hid_usage");
    if (!hid_usage) {
        LOG_ERR("Test HID usage setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_write(hid_usage, &ZMK_CUSTOM_SETTING_VALUE_INT32(29),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(hid_usage, &ZMK_CUSTOM_SETTING_VALUE_INT32(30),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected HID usage validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_validate_hid_usage");

    const struct zmk_custom_setting *layer_id = zmk_custom_setting_find("test", "layer_id");
    if (!layer_id) {
        LOG_ERR("Test layer ID setting not registered");
        return -ENOENT;
    }

    ret = zmk_custom_setting_write(layer_id, &ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(layer_id, &ZMK_CUSTOM_SETTING_VALUE_INT32(ZMK_KEYMAP_LAYERS_LEN),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected layer ID validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_validate_layer_id");

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    const struct zmk_custom_setting *behavior_id = zmk_custom_setting_find("test", "behavior_id");
    if (!behavior_id) {
        LOG_ERR("Test behavior ID setting not registered");
        return -ENOENT;
    }

    zmk_behavior_local_id_t key_press_id = zmk_behavior_get_local_id("key_press");
    if (key_press_id == UINT16_MAX) {
        LOG_ERR("Test key press behavior local ID not registered");
        return -ENOENT;
    }

    ret = zmk_custom_setting_write(behavior_id, &ZMK_CUSTOM_SETTING_VALUE_INT32(key_press_id),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(behavior_id, &ZMK_CUSTOM_SETTING_VALUE_INT32(UINT16_MAX),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected behavior ID validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_validate_behavior_id");
#endif

    return 0;
}

static int test_bytes_rpc_converters(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "bytes_value");
    if (!setting) {
        LOG_ERR("Test custom bytes setting not registered");
        return -ENOENT;
    }

    struct zmk_custom_setting_value internal_value;
    int ret = zmk_custom_setting_read(setting, &internal_value);
    if (ret < 0) {
        return ret;
    }
    const uint8_t expected_internal_default[] = {1, 2, 3};
    ret = expect_bytes_value(&internal_value, expected_internal_default,
                             sizeof(expected_internal_default));
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value rpc_value;
    ret = zmk_custom_setting_serialize_rpc_value(setting, &internal_value, &rpc_value);
    if (ret < 0) {
        return ret;
    }
    const uint8_t expected_rpc_default[] = {3, 2, 1};
    ret = expect_bytes_value(&rpc_value, expected_rpc_default, sizeof(expected_rpc_default));
    if (ret < 0) {
        return ret;
    }

    const struct zmk_custom_setting_value rpc_write = ZMK_CUSTOM_SETTING_VALUE_BYTES(9, 8, 7);
    ret = zmk_custom_setting_deserialize_rpc_value(setting, &rpc_write, &internal_value);
    if (ret < 0) {
        return ret;
    }
    const uint8_t expected_internal_write[] = {7, 8, 9};
    ret = expect_bytes_value(&internal_value, expected_internal_write,
                             sizeof(expected_internal_write));
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_bytes_rpc_convert rpc=030201 internal=070809");
    return 0;
}

static int test_scalar_lifecycle(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "int_value");
    if (!setting) {
        LOG_ERR("Test custom setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Reset custom setting unexpectedly has unsaved value");
        return -EINVAL;
    }

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(25),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value_by_key("test", "int_value", 25);
    if (ret < 0) {
        return ret;
    }

    if (!zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Memory-updated custom setting should have unsaved value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_update_read scalar=25");

    ret = zmk_custom_setting_discard(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Discarded custom setting unexpectedly has unsaved value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_discard scalar=10");

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(30),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_save(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 30);
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Saved custom setting unexpectedly has unsaved value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_save scalar=30");

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(40),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 30);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_load scalar=30");

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(41),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_discard(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 30);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_discard_persistent scalar=30");

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_reset scalar=10");

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(101),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected range validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_validate_range");

    return 0;
}

/* Persisting a value equal to the default must delete the stored record (so the
 * setting falls back to the possibly-updated default on reboot) rather than
 * keep a redundant copy. */
static int test_save_default_deletes_record(void) {
    const char *const record_name = "custom_settings/test/int_value";
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "int_value");
    if (!setting) {
        LOG_ERR("Test custom setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }
    if (test_settings_has_record(record_name)) {
        LOG_ERR("Reset custom setting should have no persisted record");
        return -EINVAL;
    }

    /* A non-default value persists a real record. */
    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(30),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    if (!test_settings_has_record(record_name)) {
        LOG_ERR("Non-default persisted value should have a stored record");
        return -EINVAL;
    }

    /* Persisting the default value (10) deletes that record instead of storing
     * a redundant copy. */
    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(10),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    if (test_settings_has_record(record_name)) {
        LOG_ERR("Persisting the default value should have deleted the record");
        return -EINVAL;
    }
    if (zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Setting saved back to default unexpectedly has unsaved value");
        return -EINVAL;
    }

    /* A reload with no record leaves the setting reading the default. */
    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }

    /* Persisting the default again with no existing record is a clean no-op. */
    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(10),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    if (test_settings_has_record(record_name)) {
        LOG_ERR("Persisting the default with no record should not create one");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_save_default_deletes_record");
    return 0;
}

static int test_array_lifecycle(void) {
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array_element("test", "array_value", 1);
    if (!array_setting || !zmk_custom_setting_is_array(array_setting)) {
        LOG_ERR("Test custom array setting not registered");
        return -ENOENT;
    }
    const struct zmk_custom_setting *array_tail_setting =
        zmk_custom_setting_find_array_element("test", "array_value", 2);
    if (!array_tail_setting || !zmk_custom_setting_is_array(array_tail_setting)) {
        LOG_ERR("Test custom array tail setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_array_size(array_setting) != 3 ||
        zmk_custom_setting_array_max_size(array_setting) != 3) {
        LOG_ERR("Unexpected default custom array size");
        return -EINVAL;
    }

    ret = expect_int_value(array_setting, 2);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(array_tail_setting, 3);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write_array_element(array_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(55),
                                                 2, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 1, 55);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_read_array_by_key("test", "array_value", 2,
                                               &(struct zmk_custom_setting_value){0});
    if (ret != -ENOENT) {
        LOG_ERR("Expected inactive custom array element read to fail, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_update_read array[1]=55 size=2");

    ret = zmk_custom_setting_save(array_setting);
    if (ret < 0) {
        return ret;
    }
    if (!test_settings_has_record("custom_settings/test/array_value/_size") ||
        test_settings_has_record("custom_settings/test/array_value/2")) {
        LOG_ERR("Unexpected persisted custom array records");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_save_required_array_items size=2");

    ret = zmk_custom_setting_write_array_element(array_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(66),
                                                 2, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_discard(array_setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 1, 55);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 2) {
        LOG_ERR("Discard did not restore persisted custom array size");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_save_discard array[1]=55 size=2");

    ret = zmk_custom_setting_write_array_element(array_tail_setting,
                                                 &ZMK_CUSTOM_SETTING_VALUE_INT32(77), 3,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 2, 77);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_grow_array array[2]=77 size=3");

    uint32_t affected_count = 0;
    ret = zmk_custom_settings_reset_scope("test", "array_value", NULL, &affected_count);
    if (ret < 0) {
        return ret;
    }
    if (affected_count != 3) {
        LOG_ERR("Unexpected custom array reset affected count: %u", affected_count);
        return -EINVAL;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 1, 2);
    if (ret < 0) {
        return ret;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 2, 3);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_reset array[1]=2 size=3");

    struct zmk_custom_setting_value popped_value;
    ret = zmk_custom_setting_array_pop_back(array_setting, &popped_value,
                                            ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    if (popped_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || popped_value.int32_value != 3 ||
        zmk_custom_setting_array_size(array_setting) != 2) {
        LOG_ERR("Unexpected custom array pop_back result");
        return -EINVAL;
    }
    ret = zmk_custom_setting_read_array_by_key("test", "array_value", 2,
                                               &(struct zmk_custom_setting_value){0});
    if (ret != -ENOENT) {
        LOG_ERR("Expected popped custom array element read to fail, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_pop_back array[2]=3 size=2");

    ret = zmk_custom_setting_array_push_back(array_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(88),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = expect_array_int_value_by_key("test", "array_value", 2, 88);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 3) {
        LOG_ERR("Custom array push_back did not grow active size");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_push_back array[2]=88 size=3");

    ret = zmk_custom_setting_save(array_setting);
    if (ret < 0) {
        return ret;
    }
    if (!test_settings_has_record("custom_settings/test/array_value/0") ||
        !test_settings_has_record("custom_settings/test/array_value/1") ||
        !test_settings_has_record("custom_settings/test/array_value/2")) {
        LOG_ERR("Custom array save did not persist all active values");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_save_all_active_array_items size=3");

    ret = zmk_custom_setting_array_push_back(array_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(99),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected full custom array push_back to fail, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_push_back_full size=3");
    return 0;
}

/* insert_at/remove_at: memmove-based shift over the array's single
 * contiguous buffer (see zmk_custom_setting_array_insert_at/remove_at in
 * custom_settings.c), instead of requiring one write per shifted element. */
static int test_array_insert_remove(void) {
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array("test", "array_value");
    if (!array_setting) {
        LOG_ERR("Test custom array setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }
    /* Defaults are {1, 2, 3} at size 3. */

    /* Insert in the middle: {1, 2, 3} -> {1, 99, 2, 3}. */
    ret = zmk_custom_setting_array_insert_at(array_setting, 1, &ZMK_CUSTOM_SETTING_VALUE_INT32(99),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        /* array_value's max size is 3, so inserting into a full array must
         * fail before anything else is asserted. */
        LOG_ERR("Expected full custom array insert_at to fail with -ERANGE, got %d", ret);
        return -EINVAL;
    }

    /* Make room by growing max usage down to 2 active elements first. */
    struct zmk_custom_setting_value popped;
    ret = zmk_custom_setting_array_pop_back(array_setting, &popped,
                                            ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    /* Active elements are now {1, 2}. */

    ret = zmk_custom_setting_array_insert_at(array_setting, 1, &ZMK_CUSTOM_SETTING_VALUE_INT32(99),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 3) {
        LOG_ERR("insert_at did not grow the active array size");
        return -EINVAL;
    }
    ret = expect_array_int_value_by_key("test", "array_value", 0, 1);
    if (ret == 0) {
        ret = expect_array_int_value_by_key("test", "array_value", 1, 99);
    }
    if (ret == 0) {
        ret = expect_array_int_value_by_key("test", "array_value", 2, 2);
    }
    if (ret < 0) {
        LOG_ERR("insert_at(1, 99) did not shift the tail correctly");
        return ret;
    }
    LOG_INF("PASS: custom_settings_insert_at_middle array={1,99,2}");

    /* insert_at(index == size) behaves like push_back. */
    ret = zmk_custom_setting_array_pop_back(array_setting, NULL,
                                            ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    /* Active elements are now {1, 99}. */
    ret = zmk_custom_setting_array_insert_at(array_setting, 2, &ZMK_CUSTOM_SETTING_VALUE_INT32(77),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = expect_array_int_value_by_key("test", "array_value", 2, 77);
    if (ret < 0) {
        LOG_ERR("insert_at(size, ...) did not behave like push_back");
        return ret;
    }
    LOG_INF("PASS: custom_settings_insert_at_end array={1,99,77}");

    /* insert_at past the (post-insert) end fails with -ERANGE and leaves the
     * array untouched: index 4 > size 3, and inserting would also exceed
     * max_size 3. */
    ret = zmk_custom_setting_array_insert_at(array_setting, 4, &ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected out-of-range insert_at to fail with -ERANGE, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_insert_at_out_of_range");

    /* remove_at in the middle: {1, 99, 77} -> {1, 77}, returns removed=99. */
    struct zmk_custom_setting_value removed;
    ret = zmk_custom_setting_array_remove_at(array_setting, 1, &removed,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    if (removed.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || removed.int32_value != 99) {
        LOG_ERR("remove_at did not return the removed value");
        return -EINVAL;
    }
    if (zmk_custom_setting_array_size(array_setting) != 2) {
        LOG_ERR("remove_at did not shrink the active array size");
        return -EINVAL;
    }
    ret = expect_array_int_value_by_key("test", "array_value", 0, 1);
    if (ret == 0) {
        ret = expect_array_int_value_by_key("test", "array_value", 1, 77);
    }
    if (ret < 0) {
        LOG_ERR("remove_at(1) did not shift the tail correctly");
        return ret;
    }
    LOG_INF("PASS: custom_settings_remove_at_middle array={1,77} removed=99");

    /* remove_at past the active length fails with -ENOENT. */
    ret = zmk_custom_setting_array_remove_at(array_setting, 5, NULL,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ENOENT) {
        LOG_ERR("Expected out-of-range remove_at to fail with -ENOENT, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_remove_at_out_of_range");

    ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/* Backward-compat check for the storage schema: writes raw per-index
 * records exactly as the pre-P3 one-registration-per-element design would
 * have written them (custom_settings/test/array_value/0, .../1, .../_size),
 * then verifies settings_load_subtree loads them into the correct in-RAM
 * slots of the new single-buffer implementation. Storage naming itself did
 * not change (see save_array_locked/custom_settings_handle_set), only how
 * the in-RAM side is organized. */
static int test_array_storage_backward_compat(void) {
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array("test", "array_value");
    if (!array_setting) {
        LOG_ERR("Test custom array setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }

    /* Simulate what old per-element registrations would have persisted:
     * size=2, index 0 = 11, index 1 = 22 (index 2 intentionally absent, as
     * it would be for an inactive slot). Values must satisfy array_value's
     * ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100) constraint. */
    const int32_t old_index_0 = 11;
    const int32_t old_index_1 = 22;
    const uint32_t old_size = 2;

    ret = test_settings_save(NULL, "custom_settings/test/array_value/0", (const char *)&old_index_0,
                             sizeof(old_index_0));
    if (ret < 0) {
        return ret;
    }
    ret = test_settings_save(NULL, "custom_settings/test/array_value/1", (const char *)&old_index_1,
                             sizeof(old_index_1));
    if (ret < 0) {
        return ret;
    }
    ret = test_settings_save(NULL, "custom_settings/test/array_value/_size",
                             (const char *)&old_size, sizeof(old_size));
    if (ret < 0) {
        return ret;
    }

    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_array_size(array_setting) != old_size) {
        LOG_ERR("Backward-compat load did not restore the old array size");
        return -EINVAL;
    }
    ret = expect_array_int_value_by_key("test", "array_value", 0, old_index_0);
    if (ret == 0) {
        ret = expect_array_int_value_by_key("test", "array_value", 1, old_index_1);
    }
    if (ret < 0) {
        LOG_ERR("Backward-compat load did not populate the correct in-RAM slots");
        return ret;
    }
    ret = zmk_custom_setting_read_array_by_key("test", "array_value", 2,
                                               &(struct zmk_custom_setting_value){0});
    if (ret != -ENOENT) {
        LOG_ERR("Expected index 2 to remain inactive after backward-compat load, got %d", ret);
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_array_storage_backward_compat size=2 values={11,22}");

    ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static void capture_int32_visitor(const struct zmk_custom_setting_value *value, void *user_data) {
    *(int32_t *)user_data = value->int32_value;
}

static int test_view_based_api(void) {
    const struct zmk_custom_setting *int_setting = zmk_custom_setting_find("test", "int_value");
    const struct zmk_custom_setting *bytes_setting = zmk_custom_setting_find("test", "bytes_value");
    if (!int_setting || !bytes_setting) {
        LOG_ERR("View API test settings not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(bytes_setting);
    if (ret < 0) {
        return ret;
    }

    /* zmk_custom_setting_with_value: zero-copy borrow under the lock. */
    int32_t seen = 0;
    ret = zmk_custom_setting_with_value(int_setting, capture_int32_visitor, &seen);
    if (ret < 0) {
        return ret;
    }
    if (seen != 10) {
        LOG_ERR("with_value visitor saw %d, expected default 10", seen);
        return -EINVAL;
    }

    /* zmk_custom_setting_get_int32 / set_int32. */
    int32_t int_value;
    ret = zmk_custom_setting_get_int32(int_setting, &int_value);
    if (ret < 0 || int_value != 10) {
        LOG_ERR("get_int32 returned ret=%d value=%d, expected 10", ret, int_value);
        return -EINVAL;
    }
    ret = zmk_custom_setting_set_int32(int_setting, 42, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_get_int32(int_setting, &int_value);
    if (ret < 0 || int_value != 42) {
        LOG_ERR("get_int32 after set_int32 returned ret=%d value=%d, expected 42", ret, int_value);
        return -EINVAL;
    }
    /* Type mismatch must be rejected rather than silently reinterpreted. */
    ret = zmk_custom_setting_get_int32(bytes_setting, &int_value);
    if (ret != -EINVAL) {
        LOG_ERR("Expected get_int32 on a bytes setting to fail, got %d", ret);
        return -EINVAL;
    }

    /* zmk_custom_setting_read_into with a right-sized buffer. */
    uint8_t small_buf[3];
    size_t out_size = 0;
    enum zmk_custom_setting_value_type out_type;
    ret = zmk_custom_setting_read_into(bytes_setting, small_buf, sizeof(small_buf), &out_size,
                                       &out_type);
    if (ret < 0 || out_size != 3 || out_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
        small_buf[0] != 1 || small_buf[1] != 2 || small_buf[2] != 3) {
        LOG_ERR("read_into returned ret=%d size=%u type=%d, expected default bytes {1,2,3}", ret,
                (unsigned)out_size, out_type);
        return -EINVAL;
    }
    uint8_t too_small[2];
    ret = zmk_custom_setting_read_into(bytes_setting, too_small, sizeof(too_small), NULL, NULL);
    if (ret != -EMSGSIZE) {
        LOG_ERR("Expected read_into with an undersized buffer to fail, got %d", ret);
        return -EINVAL;
    }

    /* zmk_custom_setting_write_bytes from a runtime buffer (not a
     * compile-time-literal ZMK_CUSTOM_SETTING_VALUE_BYTES argument list). */
    uint8_t runtime_bytes[] = {5, 6, 7, 8};
    ret = zmk_custom_setting_write_bytes(bytes_setting, runtime_bytes, sizeof(runtime_bytes),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    uint8_t readback[4];
    ret = zmk_custom_setting_read_into(bytes_setting, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != 4 || memcmp(readback, runtime_bytes, sizeof(runtime_bytes)) != 0) {
        LOG_ERR("read_into after write_bytes did not return the written value");
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(bytes_setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_view_api int=42 bytes=05060708");
    return 0;
}

/* Exercises the shared temporary-override pool (CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS /
 * CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE) that replaced a per-setting
 * temporary_value copy. Assumes the default pool size of 2 slots; none of
 * the test configs override it. */
static int test_temporary_override_pool(void) {
    const struct zmk_custom_setting *int_setting = zmk_custom_setting_find("test", "int_value");
    const struct zmk_custom_setting *bytes_setting = zmk_custom_setting_find("test", "bytes_value");
    const struct zmk_custom_setting *array_0 =
        zmk_custom_setting_find_array_element("test", "array_value", 0);
    const struct zmk_custom_setting *array_1 =
        zmk_custom_setting_find_array_element("test", "array_value", 1);
    if (!int_setting || !bytes_setting || !array_0 || !array_1) {
        LOG_ERR("Temporary override pool test settings not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }

    /* A temporary override is visible via read() but does not change
     * memory_value, and has_unsaved_value reports it. */
    ret = zmk_custom_setting_write(int_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(99),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 99);
    if (ret < 0) {
        return ret;
    }
    if (!zmk_custom_setting_has_unsaved_value(int_setting)) {
        LOG_ERR("Temporary override should report an unsaved value");
        return -EINVAL;
    }

    ret = zmk_custom_setting_rollback_temporary(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 10);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_has_unsaved_value(int_setting)) {
        LOG_ERR("Rolled-back custom setting unexpectedly has unsaved value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_temporary_rollback scalar=10");

    /* A value larger than one pool slot cannot use temporary mode. */
    uint8_t oversized[CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE + 1];
    memset(oversized, 0xAA, sizeof(oversized));
    struct zmk_custom_setting_value oversized_value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .size = sizeof(oversized),
    };
    memcpy(oversized_value.bytes_value, oversized, sizeof(oversized));
    ret = zmk_custom_setting_write(bytes_setting, &oversized_value,
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret != -EMSGSIZE) {
        LOG_ERR("Expected an oversized temporary write to fail with -EMSGSIZE, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_temporary_oversized size=%u", (unsigned)sizeof(oversized));

    /* Pool exhaustion: only CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS settings
     * may have an active temporary override at once. */
    ret = zmk_custom_setting_write(int_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(1),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_write(array_0, &ZMK_CUSTOM_SETTING_VALUE_INT32(2),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_write(array_1, &ZMK_CUSTOM_SETTING_VALUE_INT32(3),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret != -EBUSY) {
        LOG_ERR("Expected the temporary override pool to be exhausted, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_temporary_pool_exhausted");

    /* Freeing a slot lets a new override through. */
    ret = zmk_custom_setting_rollback_temporary(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_write(array_1, &ZMK_CUSTOM_SETTING_VALUE_INT32(3),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_temporary_pool_reclaimed");

    ret = zmk_custom_setting_rollback_temporary(array_0);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_rollback_temporary(array_1);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/* Bug #21, case 1: when the array "index view" pool is exhausted,
 * array_view_acquire() recycles pool slot 0 for a different (array, index).
 * If the element previously in slot 0 held an active temporary override,
 * that override's temp-pool slot must be released as part of the recycle.
 * Otherwise the temp slot leaks forever (its owner pointer aliases the reused
 * view address) and every later TEMPORARY write eventually fails with -EBUSY.
 *
 * The test deterministically parks a temporary-holding element in pool slot 0
 * (by acquiring one more distinct element than the pool holds, the last
 * acquire lands in slot 0), overrides it, then forces slot 0 to be recycled
 * by acquiring yet another distinct element. It then asserts the full
 * temporary-override pool is available again - proving the evicted element's
 * slot was freed and not left in_use/aliased. */
static int test_view_pool_temp_slot_leak(void) {
    const uint32_t pool_size = CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE;
    const uint32_t temp_slots = CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS;
    const struct zmk_custom_setting *int_setting = zmk_custom_setting_find("test", "int_value");
    const struct zmk_custom_setting *array = zmk_custom_setting_find_array("test", "view_pool");
    if (!int_setting || !array) {
        return -ENOENT;
    }
    if (pool_size + 2 > zmk_custom_setting_array_max_size(array)) {
        LOG_ERR("view_pool array (max %u) too small for a view pool of %u",
                zmk_custom_setting_array_max_size(array), pool_size);
        return -EINVAL;
    }

    /* Fill the view pool and then acquire one extra element, so pool slot 0
     * deterministically ends up holding the view for index `pool_size`. */
    const struct zmk_custom_setting *evicted = NULL;
    for (uint32_t i = 0; i <= pool_size; i++) {
        evicted = zmk_custom_setting_find_array_element("test", "view_pool", i);
        if (!evicted) {
            return -ENOENT;
        }
    }

    /* Put a temporary override on the element parked in slot 0, consuming one
     * temporary-override pool slot. */
    int ret = zmk_custom_setting_write(evicted, &ZMK_CUSTOM_SETTING_VALUE_INT32(4242),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(evicted, 4242);
    if (ret < 0) {
        return ret;
    }

    /* Acquire a fresh distinct element: the pool is full, so this recycles
     * slot 0, evicting the temporary-override holder. The fix must release
     * that element's temp slot here. */
    const struct zmk_custom_setting *replacement =
        zmk_custom_setting_find_array_element("test", "view_pool", pool_size + 1);
    if (!replacement) {
        return -ENOENT;
    }

    /* The whole temporary-override pool must be usable again. If the evicted
     * element's slot leaked, one slot stays in_use and the last of these
     * writes fails with -EBUSY. Fillers are scalars (int_value + temp_filler)
     * so acquiring them cannot itself recycle the view pool. */
    const struct zmk_custom_setting *fillers[] = {int_setting, &test_temp_filler_setting};
    if (temp_slots > ARRAY_SIZE(fillers)) {
        LOG_ERR("test assumes at most %u temp slots, configured %u", (unsigned)ARRAY_SIZE(fillers),
                temp_slots);
        return -EINVAL;
    }
    for (uint32_t i = 0; i < temp_slots; i++) {
        ret = zmk_custom_setting_write(fillers[i], &ZMK_CUSTOM_SETTING_VALUE_INT32(50 + i),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
        if (ret < 0) {
            LOG_ERR("Temp pool leaked after view-pool eviction: filler %u failed (%d)", i, ret);
            return ret;
        }
    }

    /* Pool is now full; a further temporary write must fail cleanly. This
     * confirms the fillers really occupied every slot (i.e. none was still
     * held by the leaked/aliased evicted element). */
    ret = zmk_custom_setting_write(replacement, &ZMK_CUSTOM_SETTING_VALUE_INT32(9999),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret != -EBUSY) {
        LOG_ERR("Expected temp pool to be exactly full after eviction, got %d", ret);
        return -EINVAL;
    }

    /* Clean up: release every filler override we placed. */
    for (uint32_t i = 0; i < temp_slots; i++) {
        ret = zmk_custom_setting_rollback_temporary(fillers[i]);
        if (ret < 0) {
            return ret;
        }
    }

    LOG_INF("PASS: custom_settings_view_pool_temp_slot_leak pool=%u", pool_size);
    return 0;
}

/* Simulates what a boot-time devicetree default installer does: replace a
 * setting's compile-time default before any user value has been set, then
 * confirm reset() re-applies the new default. */
static int test_boot_default_override(void) {
    const struct zmk_custom_setting *int_setting = zmk_custom_setting_find("test", "int_value");
    if (!int_setting) {
        return -ENODEV;
    }

    int ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }

    /* An out-of-range default is rejected and must not disturb the existing
     * default. */
    ret = zmk_custom_setting_set_default(int_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(500));
    if (ret != -ERANGE) {
        LOG_ERR("Expected out-of-range default to fail with -ERANGE, got %d", ret);
        return -EINVAL;
    }
    ret = expect_int_value(int_setting, 10);
    if (ret < 0) {
        return ret;
    }

    static const struct zmk_custom_setting_value boot_default = ZMK_CUSTOM_SETTING_VALUE_INT32(77);
    ret = zmk_custom_setting_set_default(int_setting, &boot_default);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 77);
    if (ret < 0) {
        return ret;
    }

    /* A user write still shadows the new default, and reset() restores it. */
    ret = zmk_custom_setting_write(int_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(42),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 42);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 77);
    if (ret < 0) {
        return ret;
    }

    /* Restore the compile-time default so this test is order-independent. */
    ret = zmk_custom_setting_set_default(int_setting, &test_int_setting_default);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_boot_default_override");
    return 0;
}

/* Build a keyspace slot's `[user_key\0][payload]` blob the way a real
 * CreateSetting+save would, for tests that seed a persisted record directly
 * in the fake settings backend (simulating a value saved by a previous boot
 * session) - see test_keyspace_lifecycle's settings-load re-binding case.
 * Returns the blob length, or 0 if it does not fit `out_capacity`. */
static size_t test_keyspace_encode_blob(uint8_t *out, size_t out_capacity, const char *key,
                                        const void *payload, size_t payload_size) {
    size_t key_len = strlen(key);
    if (key_len + 1 + payload_size > out_capacity) {
        return 0;
    }

    memcpy(out, key, key_len);
    out[key_len] = '\0';
    memcpy(out + key_len + 1, payload, payload_size);
    return key_len + 1 + payload_size;
}

/*
 * Simplification P3: opaque-blob keyspaces. Exercises the whole lifecycle:
 * create entries (quota enforcement, prefix mismatch), confirm they are
 * find-able/listable/covered by scope operations via the explicit
 * keyspace-slot walks apply_scope/for_each_list_item now do (Goal A audit -
 * keyspace slots are no longer reachable through ZMK_CUSTOM_SETTING_FOREACH),
 * delete one and confirm its slot is reusable, and - the trickiest and most
 * important part - confirm persisted entries seeded directly in the fake
 * settings backend under their ordinal storage name (as if created by a
 * CreateSetting RPC in a previous boot session and saved, with no
 * application code creating them again this boot) come back as live,
 * listable, deletable settings after a settings_load pass, with no
 * CreateSetting call ever happening for them this boot. A keyspace needs no
 * runtime registration step any more (ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE is
 * a compile-time STRUCT_SECTION_ITERABLE), so this test starts straight from
 * create().
 */
static int test_keyspace_lifecycle(void) {
    /* A key that does not start with the keyspace's prefix is rejected. */
    const struct zmk_custom_setting *created = NULL;
    int ret = zmk_custom_setting_keyspace_create(&test_macro_keyspace, "not_macro/foo",
                                                 &ZMK_CUSTOM_SETTING_VALUE_INT32(1),
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &created);
    if (ret != -EINVAL) {
        LOG_ERR("Expected a prefix-mismatched key to fail with -EINVAL, got %d", ret);
        return -EINVAL;
    }

    /* A key that does not fit max_key_len (16, including NUL) is rejected. */
    ret = zmk_custom_setting_keyspace_create(&test_macro_keyspace, "macro/this-name-is-too-long",
                                             &ZMK_CUSTOM_SETTING_VALUE_INT32(1),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &created);
    if (ret != -ENAMETOOLONG) {
        LOG_ERR("Expected an oversized key to fail with -ENAMETOOLONG, got %d", ret);
        return -EINVAL;
    }

    /* Create up to max_entries (3) entries. */
    ret = zmk_custom_setting_keyspace_create(&test_macro_keyspace, "macro/one",
                                             &ZMK_CUSTOM_SETTING_VALUE_INT32(1),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &created);
    if (ret < 0 || !created) {
        LOG_ERR("Failed to create macro/one: %d", ret);
        return ret < 0 ? ret : -EINVAL;
    }
    ret = zmk_custom_setting_keyspace_create(&test_macro_keyspace, "macro/two",
                                             &ZMK_CUSTOM_SETTING_VALUE_INT32(2),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &created);
    if (ret < 0) {
        LOG_ERR("Failed to create macro/two: %d", ret);
        return ret;
    }
    ret = zmk_custom_setting_keyspace_create(&test_macro_keyspace, "macro/three",
                                             &ZMK_CUSTOM_SETTING_VALUE_INT32(3),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &created);
    if (ret < 0) {
        LOG_ERR("Failed to create macro/three: %d", ret);
        return ret;
    }

    /* Creating the same key twice fails. */
    ret = zmk_custom_setting_keyspace_create(&test_macro_keyspace, "macro/one",
                                             &ZMK_CUSTOM_SETTING_VALUE_INT32(9),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret != -EEXIST) {
        LOG_ERR("Expected re-creating macro/one to fail with -EEXIST, got %d", ret);
        return -EINVAL;
    }

    /* The pool is full (3/3): a 4th create fails cleanly with -ENOSPC. */
    ret = zmk_custom_setting_keyspace_create(&test_macro_keyspace, "macro/four",
                                             &ZMK_CUSTOM_SETTING_VALUE_INT32(4),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret != -ENOSPC) {
        LOG_ERR("Expected create on a full keyspace to fail with -ENOSPC, got %d", ret);
        return -EINVAL;
    }

    /* Created entries are find-able like any other setting. */
    ret = expect_int_value_by_key("test", "macro/one", 1);
    if (ret == 0) {
        ret = expect_int_value_by_key("test", "macro/two", 2);
    }
    if (ret == 0) {
        ret = expect_int_value_by_key("test", "macro/three", 3);
    }
    if (ret < 0) {
        LOG_ERR("Created keyspace entries are not readable by key");
        return ret;
    }

    /* zmk_custom_setting_keyspace_find resolves the same literal key
     * without going through the generic subsystem+key registry walk. */
    if (zmk_custom_setting_keyspace_find(&test_macro_keyspace, "macro/one") == NULL) {
        LOG_ERR("zmk_custom_setting_keyspace_find did not find macro/one");
        return -ENOENT;
    }
    if (zmk_custom_setting_keyspace_find(&test_macro_keyspace, "macro/nonexistent") != NULL) {
        LOG_ERR("zmk_custom_setting_keyspace_find found a nonexistent key");
        return -EINVAL;
    }

    /* Discovery: the existing key_prefix scope filter (used by list/save/
     * discard/reset) sees exactly the 3 live entries under "macro/" - via
     * apply_scope's explicit keyspace-slot pass (simplification P3: slots
     * are no longer reachable through ZMK_CUSTOM_SETTING_FOREACH). */
    uint32_t affected_count = 0;
    ret = zmk_custom_settings_save_scope("test", NULL, "macro/", &affected_count);
    if (ret < 0) {
        return ret;
    }
    if (affected_count != 3) {
        LOG_ERR("Expected key_prefix scope to match exactly 3 macro/ entries, got %u",
                affected_count);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_keyspace_create_and_scope_discovery count=3");

    /* Delete one entry: its slot is released and reusable, and its
     * persisted record (macro/two was saved above, occupying slot 1 - the
     * second create - so it persists under the ordinal record name
     * "custom_settings/test/macro/#1", simplification P3) is erased. */
    if (!test_settings_has_record("custom_settings/test/macro/#1")) {
        LOG_ERR("Persisted keyspace entry's ordinal record was not written");
        return -EINVAL;
    }
    ret = zmk_custom_setting_keyspace_delete(&test_macro_keyspace, "macro/two");
    if (ret < 0) {
        LOG_ERR("zmk_custom_setting_keyspace_delete failed: %d", ret);
        return ret;
    }
    if (zmk_custom_setting_find("test", "macro/two") != NULL) {
        LOG_ERR("Deleted keyspace entry is still find-able");
        return -EINVAL;
    }
    if (test_settings_has_record("custom_settings/test/macro/#1")) {
        LOG_ERR("Deleted keyspace entry's persisted record was not erased");
        return -EINVAL;
    }
    ret = zmk_custom_setting_keyspace_delete(&test_macro_keyspace, "macro/two");
    if (ret != -ENOENT) {
        LOG_ERR("Expected deleting an already-deleted key to fail with -ENOENT, got %d", ret);
        return -EINVAL;
    }

    /* The freed slot is reusable by a new create. */
    ret = zmk_custom_setting_keyspace_create(&test_macro_keyspace, "macro/four",
                                             &ZMK_CUSTOM_SETTING_VALUE_INT32(4),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret < 0) {
        LOG_ERR("Failed to reuse a freed keyspace slot: %d", ret);
        return ret;
    }
    LOG_INF("PASS: custom_settings_keyspace_delete_frees_slot");

    /* Clean up in-RAM state so the settings-load re-binding test below
     * starts from an empty pool. */
    zmk_custom_setting_keyspace_delete(&test_macro_keyspace, "macro/one");
    zmk_custom_setting_keyspace_delete(&test_macro_keyspace, "macro/three");
    zmk_custom_setting_keyspace_delete(&test_macro_keyspace, "macro/four");

    /*
     * Settings-load re-binding: seed 3 persisted records directly in the
     * fake settings backend under their ORDINAL storage name
     * ("<key_prefix>#<index>", simplification P3 - a slot's storage
     * identity, not its user key), each holding the opaque
     * `[user_key\0][payload]` blob a real CreateSetting+save would have
     * written, as if that had happened in a previous boot session -
     * crucially, with NO zmk_custom_setting_keyspace_create call happening
     * this boot at all. settings_load_subtree then drives the exact same
     * SETTINGS_STATIC_HANDLER_DEFINE callback path as the real boot-time
     * settings_load() in main() (this test itself runs from a SYS_INIT
     * before that call, so it must trigger the load explicitly the same way
     * test_array_storage_backward_compat does). custom_settings_handle_set
     * must recognize each record name matches test_macro_keyspace's "macro/"
     * prefix + "#<index>" pattern, bind slot `index` directly (no key
     * needed yet - it comes from decoding the blob about to be loaded), and
     * apply the loaded blob - all before this call returns. All 3 slots were
     * just freed above, so indices 0/1/2 are all available regardless of
     * which physical slots "macro/one" etc. happened to occupy earlier.
     */
    const int32_t reloaded_a = 111;
    const int32_t reloaded_b = 222;
    const int32_t reloaded_c = 333;
    uint8_t blob_a[32];
    uint8_t blob_b[32];
    uint8_t blob_c[32];
    size_t blob_a_len = test_keyspace_encode_blob(blob_a, sizeof(blob_a), "macro/reload-a",
                                                  &reloaded_a, sizeof(reloaded_a));
    size_t blob_b_len = test_keyspace_encode_blob(blob_b, sizeof(blob_b), "macro/reload-b",
                                                  &reloaded_b, sizeof(reloaded_b));
    size_t blob_c_len = test_keyspace_encode_blob(blob_c, sizeof(blob_c), "macro/reload-c",
                                                  &reloaded_c, sizeof(reloaded_c));
    ret =
        test_settings_save(NULL, "custom_settings/test/macro/#0", (const char *)blob_a, blob_a_len);
    if (ret == 0) {
        ret = test_settings_save(NULL, "custom_settings/test/macro/#1", (const char *)blob_b,
                                 blob_b_len);
    }
    if (ret == 0) {
        ret = test_settings_save(NULL, "custom_settings/test/macro/#2", (const char *)blob_c,
                                 blob_c_len);
    }
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_find("test", "macro/reload-a") != NULL) {
        LOG_ERR("Reload-test keyspace entry was already bound before settings_load_subtree ran");
        return -EINVAL;
    }

    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value_by_key("test", "macro/reload-a", reloaded_a);
    if (ret == 0) {
        ret = expect_int_value_by_key("test", "macro/reload-b", reloaded_b);
    }
    if (ret == 0) {
        ret = expect_int_value_by_key("test", "macro/reload-c", reloaded_c);
    }
    if (ret < 0) {
        LOG_ERR("Persisted keyspace entries were not re-bound by settings_load_subtree");
        return ret;
    }

    /* Re-bound entries are full citizens: listable via key_prefix, and
     * deletable, exactly like ones created via CreateSetting this boot. */
    affected_count = 0;
    ret = zmk_custom_settings_save_scope("test", NULL, "macro/", &affected_count);
    if (ret < 0) {
        return ret;
    }
    if (affected_count != 3) {
        LOG_ERR("Expected key_prefix scope to match exactly 3 re-bound macro/ entries, got %u",
                affected_count);
        return -EINVAL;
    }

    ret = zmk_custom_setting_keyspace_delete(&test_macro_keyspace, "macro/reload-a");
    if (ret == 0) {
        ret = zmk_custom_setting_keyspace_delete(&test_macro_keyspace, "macro/reload-b");
    }
    if (ret == 0) {
        ret = zmk_custom_setting_keyspace_delete(&test_macro_keyspace, "macro/reload-c");
    }
    if (ret < 0) {
        LOG_ERR("Failed to delete a re-bound keyspace entry: %d", ret);
        return ret;
    }

    LOG_INF("PASS: custom_settings_keyspace_settings_load_rebind a=111 b=222 c=333");

    return 0;
}

/*
 * Regression for "No keyspace registered for this key" on CreateSetting.
 *
 * The Studio CreateSetting/DeleteSetting handlers resolve their target
 * keyspace with zmk_custom_settings_keyspace_find_for_key(subsystem, key).
 * A client that addresses an entry by key alone - a SettingRef with no
 * custom_subsystem_index, which is exactly how the runtime-macro web UI
 * issues "macro/<name>" create/delete/rename - arrives with a NULL subsystem
 * id. zmk_custom_setting_find() already treats a NULL/empty subsystem id as
 * "match any"; find_for_key used to bail on NULL instead, so every such
 * create failed. This asserts a keyspace still resolves by its key prefix
 * with a NULL (and empty) subsystem id, even for a not-yet-existing key
 * (the create path matches by prefix, before any entry exists).
 */
static int test_keyspace_find_null_subsystem(void) {
    struct zmk_custom_setting_keyspace *ks =
        zmk_custom_settings_keyspace_find_for_key(NULL, "macro/not-created-yet");
    if (ks != &test_macro_keyspace) {
        LOG_ERR("find_for_key(NULL, \"macro/...\") = %p, expected the macro keyspace", (void *)ks);
        return -ENOENT;
    }

    ks = zmk_custom_settings_keyspace_find_for_key("", "macro/not-created-yet");
    if (ks != &test_macro_keyspace) {
        LOG_ERR("find_for_key(\"\", \"macro/...\") = %p, expected the macro keyspace", (void *)ks);
        return -ENOENT;
    }

    /* A key whose prefix matches no keyspace still resolves to NULL. */
    if (zmk_custom_settings_keyspace_find_for_key(NULL, "unknown/x") != NULL) {
        LOG_ERR("find_for_key(NULL, \"unknown/...\") matched a keyspace unexpectedly");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_keyspace_find_null_subsystem");
    return 0;
}

static int test_record_settings(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "record_value");
    if (!setting) {
        LOG_ERR("Record test setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    /* Nothing stored yet (default is just the TLV version byte), so
     * caller-provided defaults must survive record_get untouched. */
    struct test_profile profile = {.speed = 5, .invert = false, .label = "def"};
    ret = zmk_custom_setting_record_get(setting, &test_profile_schema, &profile);
    if (ret < 0) {
        return ret;
    }
    if (profile.speed != 5 || profile.invert != false || strcmp(profile.label, "def") != 0) {
        LOG_ERR("record_get on the default value changed caller-provided defaults");
        return -EINVAL;
    }

    struct test_profile write_profile = {.speed = 42, .invert = true, .label = "abcdefg"};
    ret = zmk_custom_setting_record_set(setting, &test_profile_schema, &write_profile,
                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    struct test_profile read_profile = {0};
    ret = zmk_custom_setting_record_get(setting, &test_profile_schema, &read_profile);
    if (ret < 0) {
        return ret;
    }
    if (read_profile.speed != 42 || read_profile.invert != true ||
        strcmp(read_profile.label, "abcdefg") != 0) {
        LOG_ERR("Record round-trip mismatch: speed=%d invert=%d label=%s", read_profile.speed,
                read_profile.invert, read_profile.label);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_record speed=42 invert=1 label=abcdefg");

    /* A field constraint violation is rejected before anything is written. */
    struct test_profile invalid_profile = {.speed = 999, .invert = false, .label = "x"};
    ret = zmk_custom_setting_record_set(setting, &test_profile_schema, &invalid_profile,
                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected an out-of-range record field to fail with -ERANGE, got %d", ret);
        return -EINVAL;
    }
    read_profile = (struct test_profile){0};
    ret = zmk_custom_setting_record_get(setting, &test_profile_schema, &read_profile);
    if (ret < 0 || read_profile.speed != 42) {
        LOG_ERR("Rejected record write unexpectedly changed the stored value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_record_constraint_rejected");

    /* Schema evolution: decoding with a schema missing a field the data
     * has (label) skips it and leaves the destination struct's own value
     * for that field untouched. */
    uint8_t encoded[32];
    size_t encoded_size;
    ret = zmk_custom_setting_record_encode(&test_profile_schema, &write_profile, encoded,
                                           sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }
    static const struct zmk_custom_setting_record_field speed_only_fields[] = {
        {.field_id = 1,
         .offset = offsetof(struct test_profile, speed),
         .size = sizeof(int32_t),
         .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
         .constraint = NULL},
    };
    static const struct zmk_custom_setting_record_schema speed_only_schema = {
        .fields = speed_only_fields,
        .fields_count = ARRAY_SIZE(speed_only_fields),
        .record_size = sizeof(struct test_profile),
    };
    struct test_profile speed_only = {.speed = -1, .invert = false, .label = "kept"};
    ret = zmk_custom_setting_record_decode(&speed_only_schema, encoded, encoded_size, &speed_only);
    if (ret < 0 || speed_only.speed != 42 || strcmp(speed_only.label, "kept") != 0) {
        LOG_ERR("Decoding with a narrower schema did not skip the unknown field correctly");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_record_schema_evolution speed=42 label=kept");

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
static int test_behavior_value_type(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "behavior_value");
    if (!setting) {
        LOG_ERR("Test behavior value setting not registered");
        return -ENOENT;
    }

    /* ZMK's own "behavior" settings subtree assigns real local IDs from its
     * commit handler (behavior_local_id_init for the CRC16 id type, or
     * behavior_handle_commit for the settings-table id type); until that
     * subtree is loaded/committed once, every behavior's local_id is stuck
     * at its zero-initialized default, making zmk_behavior_get_local_id and
     * zmk_behavior_find_behavior_name_from_local_id both resolve to id 0 and
     * therefore ambiguous between behaviors. Load it explicitly here since
     * this test file's fake settings backend is otherwise only driven by
     * this module's own "custom_settings" subtree. */
    int ret = settings_load_subtree("behavior");
    if (ret < 0) {
        return ret;
    }

    zmk_behavior_local_id_t key_press_id = zmk_behavior_get_local_id("key_press");
    if (key_press_id == UINT16_MAX) {
        LOG_ERR("Test key press behavior local ID not registered");
        return -ENOENT;
    }

    /* &kp's metadata only accepts an HID_USAGE-shaped param1 and requires
     * param2 == 0, so this exercises real ZMK behavior metadata validation
     * (zmk_behavior_validate_binding), not just a local id lookup. */
    struct zmk_custom_setting_behavior_value key_a = {
        .behavior_id = key_press_id,
        .param1 = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_A),
        .param2 = 0,
    };

    ret = zmk_custom_setting_set_behavior(setting, key_a, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }

    ret = expect_behavior_value(setting, key_a);
    if (ret < 0) {
        return ret;
    }

    /* Force a reload from persistent storage to prove the varint-packed
     * storage encoding (see encode_behavior_value in custom_settings.c)
     * round-trips through the settings backend, not just the in-RAM cache. */
    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        return ret;
    }

    ret = expect_behavior_value(setting, key_a);
    if (ret < 0) {
        return ret;
    }
    /* behavior_id is a runtime-assigned local id (registration-order
     * dependent), so it is deliberately omitted from this log line to keep
     * it deterministic across builds -- see custom_settings_validate_behavior_id
     * above for the same reasoning. */
    LOG_INF("PASS: custom_settings_behavior_value param1=0x%05x", key_a.param1);

    struct zmk_custom_setting_behavior_value bad_param1 = {
        .behavior_id = key_press_id,
        .param1 = 0xffff,
        .param2 = 0,
    };
    ret =
        zmk_custom_setting_set_behavior(setting, bad_param1, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -EINVAL) {
        LOG_ERR("Expected behavior param metadata validation failure, got %d", ret);
        return -EINVAL;
    }

    struct zmk_custom_setting_behavior_value bad_id = {
        .behavior_id = UINT16_MAX,
        .param1 = 0,
        .param2 = 0,
    };
    ret = zmk_custom_setting_set_behavior(setting, bad_id, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected behavior id validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_behavior_value_rejects_invalid");

    return zmk_custom_setting_reset(setting);
}
#endif

static int test_large_value(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "large_bytes");
    if (!setting) {
        LOG_ERR("Large value test setting not registered");
        return -ENOENT;
    }

    /* Sanity: a normal (int32) setting keeps inline state-union storage (no
     * blob buffer or pool of any kind - P4), while the sized setting draws
     * from its private pool. This is what keeps unrelated settings' RAM
     * footprint from growing (issue #16). */
    if (test_int_setting.blob.max_size != 0 || test_int_setting.blob.pool != NULL) {
        LOG_ERR("Normal setting unexpectedly has blob-store state");
        return -EINVAL;
    }
    if (setting->state->blob.data == NULL) {
        LOG_ERR("Sized setting is missing its pool-backed region (non-empty default)");
        return -EINVAL;
    }

    uint8_t payload[TEST_LARGE_VALUE_SIZE];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7 + 3);
    }

    int ret = zmk_custom_setting_write_bytes(setting, payload, sizeof(payload),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Large write_bytes failed: %d", ret);
        return ret;
    }

    uint8_t readback[TEST_LARGE_VALUE_SIZE];
    size_t out_size = 0;
    enum zmk_custom_setting_value_type out_type;
    ret = zmk_custom_setting_read_into(setting, readback, sizeof(readback), &out_size, &out_type);
    if (ret < 0 || out_size != sizeof(payload) || out_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
        memcmp(readback, payload, sizeof(payload)) != 0) {
        LOG_ERR("Large read_into round-trip mismatch: ret=%d size=%u", ret, (unsigned)out_size);
        return -EINVAL;
    }

    /* The fixed-carrier read must clearly reject the oversized value instead
     * of silently truncating it. */
    struct zmk_custom_setting_value carrier;
    ret = zmk_custom_setting_read(setting, &carrier);
    if (ret != -EMSGSIZE) {
        LOG_ERR("Fixed-carrier read of large value should return -EMSGSIZE, got %d", ret);
        return -EINVAL;
    }

    /* Persist, clobber in RAM, then discard: the persisted large value must be
     * restored from flash exactly. */
    ret = zmk_custom_setting_write_bytes(setting, payload, sizeof(payload),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        LOG_ERR("Large persist failed: %d", ret);
        return ret;
    }

    uint8_t other[100];
    memset(other, 0xAB, sizeof(other));
    ret = zmk_custom_setting_write_bytes(setting, other, sizeof(other),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_discard(setting);
    if (ret < 0) {
        LOG_ERR("Large discard failed: %d", ret);
        return ret;
    }

    out_size = 0;
    ret = zmk_custom_setting_read_into(setting, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload) || memcmp(readback, payload, sizeof(payload)) != 0) {
        LOG_ERR("Large discard did not restore persisted value: ret=%d size=%u", ret,
                (unsigned)out_size);
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_large_value size=%u", (unsigned)sizeof(payload));
    return 0;
}

static int test_large_record(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "big_record");
    if (!setting) {
        LOG_ERR("Large record test setting not registered");
        return -ENOENT;
    }

    struct test_big_profile in = {.id = 1234};
    memset(in.note, 'x', 180);
    in.note[180] = '\0';

    /* Confirm the encoding really is larger than the old 64-byte ceiling. */
    uint8_t encoded[TEST_LARGE_VALUE_SIZE];
    size_t encoded_size = 0;
    int ret = zmk_custom_setting_record_encode(&test_big_profile_schema, &in, encoded,
                                               sizeof(encoded), &encoded_size);
    if (ret < 0 || encoded_size <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        LOG_ERR("Large record encoding not larger than carrier: ret=%d size=%u", ret,
                (unsigned)encoded_size);
        return -EINVAL;
    }

    ret = zmk_custom_setting_record_set(setting, &test_big_profile_schema, &in,
                                        ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        LOG_ERR("Large record_set failed: %d", ret);
        return ret;
    }

    struct test_big_profile out = {0};
    ret = zmk_custom_setting_record_get(setting, &test_big_profile_schema, &out);
    if (ret < 0) {
        LOG_ERR("Large record_get failed: %d", ret);
        return ret;
    }

    if (out.id != in.id || strlen(out.note) != 180 || memcmp(out.note, in.note, 180) != 0) {
        LOG_ERR("Large record round-trip mismatch: id=%d note_len=%u", out.id,
                (unsigned)strlen(out.note));
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_large_record id=%d note_len=%u encoded=%u", out.id,
            (unsigned)strlen(out.note), (unsigned)encoded_size);
    return 0;
}

/* A >64-byte value written to a NON-large STRING setting must be rejected with
 * -EMSGSIZE (mirroring the BYTES guard) instead of being silently truncated to
 * the carrier size and reported as success. This path is reachable via the
 * firmware API and the chunked-write RPC large branch (issue #16 follow-up). */
static int test_string_no_truncation(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "string_value");
    if (!setting) {
        LOG_ERR("String test setting not registered");
        return -ENOENT;
    }
    /* P4: every STRING setting keeps its value behind a blob buffer now;
     * "non-large" means a carrier-capacity fixed store, not a pool. */
    if (setting->blob.pool != NULL ||
        setting->blob.max_size != CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        LOG_ERR("String test setting unexpectedly has large/pooled blob store");
        return -EINVAL;
    }

    /* An oversized STRING value must be rejected, not truncated. */
    char big[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 16];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    int ret = zmk_custom_setting_write_bytes(setting, big, sizeof(big) - 1,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -EMSGSIZE) {
        LOG_ERR("Oversized STRING write should return -EMSGSIZE, got %d", ret);
        return -EINVAL;
    }

    /* The rejected write must not have altered the stored value. */
    char readback[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
    size_t out_size = 0;
    ret = zmk_custom_setting_read_into(setting, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != strlen("hi") || memcmp(readback, "hi", out_size) != 0) {
        LOG_ERR("Rejected STRING write clobbered stored value: ret=%d size=%u", ret,
                (unsigned)out_size);
        return -EINVAL;
    }

    /* A value that fits the carrier must still succeed (no regression). */
    char fit[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
    memset(fit, 'b', CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    fit[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE] = '\0';
    ret = zmk_custom_setting_write_bytes(setting, fit, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Carrier-sized STRING write failed: %d", ret);
        return ret;
    }

    out_size = 0;
    ret = zmk_custom_setting_read_into(setting, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ||
        memcmp(readback, fit, out_size) != 0) {
        LOG_ERR("Carrier-sized STRING round-trip mismatch: ret=%d size=%u", ret,
                (unsigned)out_size);
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_string_no_truncation");
    return 0;
}

static void fill_pool_test_pattern(uint8_t *buf, size_t size, uint8_t seed) {
    for (size_t i = 0; i < size; i++) {
        buf[i] = (uint8_t)(i * 7 + seed);
    }
}

/* Shared large-value pool (ZMK_CUSTOM_SETTING_DEFINE_POOLED): several
 * settings draw regions from one static budget instead of each reserving its
 * own worst-case max_size buffer. test_pooled_a/b/c are deliberately
 * over-committed against the 192-byte test_large_pool, so writes here
 * legitimately hit -ENOSPC and exercise compaction (a setting's region can
 * move within the pool at any write that needs more room than it currently
 * has - see pool_ensure_region in custom_settings.c). */
static int test_large_value_pool(void) {
    if (test_pooled_a.blob.pool != &test_large_pool || test_pooled_a.state->blob.data != NULL ||
        test_pooled_a.state->blob.size != 0) {
        LOG_ERR("Pooled setting A is not in its expected pre-write state");
        return -EINVAL;
    }
    if (test_pooled_b.state->blob.data != NULL || test_pooled_b.state->blob.size != 0) {
        LOG_ERR("Pooled setting B is not in its expected pre-write state");
        return -EINVAL;
    }
    /* test_pooled_c's default is an empty STRING, but a STRING's stored NUL
     * still costs one pool byte even at size 0 (see pool_member_extent) - so
     * unlike A/B (BYTES, genuinely zero-cost when empty) it already holds a
     * one-byte region here. Confirm that, then use the actual pool usage as
     * this test's baseline instead of assuming the pool starts at 0. */
    if (test_pooled_c.state->blob.data == NULL || test_pooled_c.state->blob.size != 0) {
        LOG_ERR("Pooled STRING setting C is not in its expected pre-write state");
        return -EINVAL;
    }
    size_t baseline_used = zmk_custom_setting_large_pool_used(&test_large_pool);
    if (baseline_used != 1) {
        LOG_ERR("Unexpected pool baseline usage: %u", (unsigned)baseline_used);
        return -EINVAL;
    }

    uint8_t readback[TEST_POOLED_BYTES_MAX_SIZE];
    size_t out_size;
    int ret;

    /* 1. Write 100 bytes to A, read back identical. */
    uint8_t payload_a[100];
    fill_pool_test_pattern(payload_a, sizeof(payload_a), 3);
    ret = zmk_custom_setting_write_bytes(&test_pooled_a, payload_a, sizeof(payload_a),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Pool write A failed: %d", ret);
        return ret;
    }

    out_size = 0;
    ret = zmk_custom_setting_read_into(&test_pooled_a, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload_a) ||
        memcmp(readback, payload_a, sizeof(payload_a)) != 0) {
        LOG_ERR("Pool A round-trip mismatch: ret=%d size=%u", ret, (unsigned)out_size);
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_pool_write_and_read size=%u", (unsigned)sizeof(payload_a));

    /* 2. A already holds 100 of the pool's 192 bytes, so a 100-byte write to
     * B (100 + 100 > 192) must fail with -ENOSPC and leave A untouched. */
    uint8_t payload_b[100];
    fill_pool_test_pattern(payload_b, sizeof(payload_b), 11);
    ret = zmk_custom_setting_write_bytes(&test_pooled_b, payload_b, sizeof(payload_b),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ENOSPC) {
        LOG_ERR("Pool write B should fail with -ENOSPC, got %d", ret);
        return -EINVAL;
    }

    out_size = 0;
    ret = zmk_custom_setting_read_into(&test_pooled_a, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload_a) ||
        memcmp(readback, payload_a, sizeof(payload_a)) != 0) {
        LOG_ERR("Pool A clobbered by B's failed allocation");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_pool_enospc");

    /* 3./4. Shrinking A to 20 bytes frees enough of the pool's *logical*
     * budget (a shrink writes in place - A's region does not move) that B's
     * 100-byte write now succeeds. Placing B's new region requires
     * compaction: every other pool member is repacked from the start of the
     * pool, so this also validates A's content survives a compaction it was
     * not directly involved in. */
    uint8_t payload_a2[20];
    fill_pool_test_pattern(payload_a2, sizeof(payload_a2), 41);
    ret = zmk_custom_setting_write_bytes(&test_pooled_a, payload_a2, sizeof(payload_a2),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Shrinking pool A failed: %d", ret);
        return ret;
    }

    ret = zmk_custom_setting_write_bytes(&test_pooled_b, payload_b, sizeof(payload_b),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Pool write B should succeed once A has shrunk: %d", ret);
        return ret;
    }

    out_size = 0;
    ret = zmk_custom_setting_read_into(&test_pooled_a, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload_a2) ||
        memcmp(readback, payload_a2, sizeof(payload_a2)) != 0) {
        LOG_ERR("Pool A content lost across B's compacting allocation");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_pool_compaction size_a=%u size_b=%u",
            (unsigned)sizeof(payload_a2), (unsigned)sizeof(payload_b));

    /* 4. Grow A again within the remaining budget (192 - 100(B) = 92); this
     * forces A's region to move (its old spot is not big enough), which in
     * turn requires B - the other live member - to be compacted down first.
     * Both values must come out intact afterwards. */
    uint8_t payload_a3[70];
    fill_pool_test_pattern(payload_a3, sizeof(payload_a3), 77);
    ret = zmk_custom_setting_write_bytes(&test_pooled_a, payload_a3, sizeof(payload_a3),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Growing pool A within budget failed: %d", ret);
        return ret;
    }

    out_size = 0;
    ret = zmk_custom_setting_read_into(&test_pooled_a, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload_a3) ||
        memcmp(readback, payload_a3, sizeof(payload_a3)) != 0) {
        LOG_ERR("Pool A mismatch after regrowing");
        return -EINVAL;
    }
    out_size = 0;
    ret = zmk_custom_setting_read_into(&test_pooled_b, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload_b) ||
        memcmp(readback, payload_b, sizeof(payload_b)) != 0) {
        LOG_ERR("Pool B disturbed by A's regrowth");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_pool_grow size_a=%u size_b=%u", (unsigned)sizeof(payload_a3),
            (unsigned)sizeof(payload_b));

    /* 5. An empty write releases A's region entirely; pool_used must drop by
     * exactly A's freed extent. */
    size_t used_before = zmk_custom_setting_large_pool_used(&test_large_pool);
    ret = zmk_custom_setting_write_bytes(&test_pooled_a, NULL, 0,
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Releasing pool A failed: %d", ret);
        return ret;
    }
    size_t used_after = zmk_custom_setting_large_pool_used(&test_large_pool);
    if (used_after != used_before - sizeof(payload_a3) || test_pooled_a.state->blob.data != NULL) {
        LOG_ERR("Pool usage did not drop as expected after releasing A: before=%u after=%u",
                (unsigned)used_before, (unsigned)used_after);
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_pool_release used_before=%u used_after=%u",
            (unsigned)used_before, (unsigned)used_after);

    /* 6. Persist B, clobber it in RAM, then discard: the persisted pool
     * value must be restored - discard's settings_load_subtree() replay
     * routes back through pool_ensure_region exactly like any other pool
     * write. */
    ret = zmk_custom_setting_write_bytes(&test_pooled_b, payload_b, sizeof(payload_b),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        LOG_ERR("Persisting pool B failed: %d", ret);
        return ret;
    }

    uint8_t clobber[40];
    memset(clobber, 0xCD, sizeof(clobber));
    ret = zmk_custom_setting_write_bytes(&test_pooled_b, clobber, sizeof(clobber),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Clobbering pool B failed: %d", ret);
        return ret;
    }

    ret = zmk_custom_setting_discard(&test_pooled_b);
    if (ret < 0) {
        LOG_ERR("Discarding pool B failed: %d", ret);
        return ret;
    }

    out_size = 0;
    ret = zmk_custom_setting_read_into(&test_pooled_b, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload_b) ||
        memcmp(readback, payload_b, sizeof(payload_b)) != 0) {
        LOG_ERR("Pool B discard did not restore the persisted value: ret=%d size=%u", ret,
                (unsigned)out_size);
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_pool_persist_discard size=%u", (unsigned)sizeof(payload_b));

    /* 7. A STRING pooled member: write it, then force a real compaction move
     * (release B, then regrow it to a different size so the compactor has to
     * reposition C alongside it) and confirm the NUL terminator survives. */
    ret = zmk_custom_setting_write_bytes(&test_pooled_c, "hello", 5,
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Writing pool C failed: %d", ret);
        return ret;
    }

    ret = zmk_custom_setting_write_bytes(&test_pooled_b, NULL, 0,
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Releasing pool B failed: %d", ret);
        return ret;
    }

    uint8_t payload_b2[90];
    fill_pool_test_pattern(payload_b2, sizeof(payload_b2), 5);
    ret = zmk_custom_setting_write_bytes(&test_pooled_b, payload_b2, sizeof(payload_b2),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Regrowing pool B for the compaction check failed: %d", ret);
        return ret;
    }

    char string_readback[TEST_POOLED_STRING_MAX_SIZE];
    out_size = 0;
    ret = zmk_custom_setting_read_into(&test_pooled_c, string_readback, sizeof(string_readback),
                                       &out_size, NULL);
    if (ret < 0 || out_size != 5 || memcmp(string_readback, "hello", 5) != 0 ||
        string_readback[5] != '\0') {
        LOG_ERR("Pool STRING member C corrupted across compaction: ret=%d size=%u", ret,
                (unsigned)out_size);
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_pool_string_member value=%s", string_readback);

    /* Leave a clean slate (every pooled test member released) for anything
     * that runs after this test. */
    zmk_custom_setting_write_bytes(&test_pooled_a, NULL, 0, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    zmk_custom_setting_write_bytes(&test_pooled_b, NULL, 0, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    zmk_custom_setting_write_bytes(&test_pooled_c, NULL, 0, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

    return 0;
}

/*
 * Simplification P1 (Change 1, task 3): a keyspace entry whose value exceeds
 * the fixed carrier round-trips through the keyspace's own
 * ZMK_CUSTOM_SETTING_LARGE_POOL - the same mechanism ZMK_CUSTOM_SETTING_
 * DEFINE_POOLED uses, just scoped to one keyspace's slots instead of a
 * caller-shared pool. Also exercises that a deleted entry's pool region is
 * released (via zmk_custom_setting_keyspace_delete -> pool_release_locked),
 * not just its slot. Since simplification P3 every entry's pool region holds
 * the WHOLE blob (key + NUL + payload, not just the payload), so the
 * expected pool usage below includes each entry's (short) key length.
 */
static int test_keyspace_large_value(void) {
    if (zmk_custom_setting_large_pool_used(test_blob_keyspace.large_pool) != 0) {
        LOG_ERR("Keyspace pool should start empty");
        return -EINVAL;
    }

    const struct zmk_custom_setting *created = NULL;
    int ret = zmk_custom_setting_keyspace_create(&test_blob_keyspace, "blob/one",
                                                 &ZMK_CUSTOM_SETTING_VALUE_BYTES(1, 2, 3),
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &created);
    if (ret < 0 || !created) {
        LOG_ERR("Failed to create blob/one: %d", ret);
        return ret < 0 ? ret : -EINVAL;
    }

    /* Grow the entry past the fixed carrier via write_bytes, exactly like a
     * DEFINE_POOLED/DEFINE_SIZED setting - the slot's blob region starts NULL
     * (no region allocated by create() for a value that fits the carrier)
     * and pool_ensure_region() carves one on this write. */
    uint8_t payload[TEST_KEYSPACE_LARGE_MAX_SIZE];
    fill_pool_test_pattern(payload, sizeof(payload), 5);
    ret = zmk_custom_setting_write_bytes(created, payload, sizeof(payload),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        LOG_ERR("Writing max-size blob/one failed: %d", ret);
        return ret;
    }

    /* A second full-size entry must also fit - the pool's worst-case budget
     * (max_entries * (max_size + 1)) matches what two dedicated per-slot
     * buffers would have reserved, so this is not a capacity regression. */
    const struct zmk_custom_setting *created2 = NULL;
    ret = zmk_custom_setting_keyspace_create(&test_blob_keyspace, "blob/two",
                                             &ZMK_CUSTOM_SETTING_VALUE_BYTES(9),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &created2);
    if (ret < 0 || !created2) {
        LOG_ERR("Failed to create blob/two: %d", ret);
        return ret < 0 ? ret : -EINVAL;
    }
    uint8_t payload2[TEST_KEYSPACE_LARGE_MAX_SIZE];
    fill_pool_test_pattern(payload2, sizeof(payload2), 19);
    ret = zmk_custom_setting_write_bytes(created2, payload2, sizeof(payload2),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Writing max-size blob/two failed (pool budget regression?): %d", ret);
        return ret;
    }

    uint8_t readback[TEST_KEYSPACE_LARGE_MAX_SIZE];
    size_t out_size = 0;
    ret = zmk_custom_setting_read_into(created, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload) || memcmp(readback, payload, sizeof(payload)) != 0) {
        LOG_ERR("Keyspace pooled read-back mismatch for blob/one: ret=%d size=%u", ret,
                (unsigned)out_size);
        return -EINVAL;
    }

    /* Clobber blob/one in RAM, then discard: the persisted large value must
     * be restored from the keyspace pool exactly like any other pooled
     * setting's discard. */
    uint8_t clobber[10];
    memset(clobber, 0xEE, sizeof(clobber));
    ret = zmk_custom_setting_write_bytes(created, clobber, sizeof(clobber),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_discard(created);
    if (ret < 0) {
        LOG_ERR("Discarding blob/one failed: %d", ret);
        return ret;
    }
    out_size = 0;
    ret = zmk_custom_setting_read_into(created, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != sizeof(payload) || memcmp(readback, payload, sizeof(payload)) != 0) {
        LOG_ERR("Keyspace pooled discard did not restore persisted value: ret=%d size=%u", ret,
                (unsigned)out_size);
        return -EINVAL;
    }

    /* Each entry's pool region is the whole blob: strlen("blob/one") + NUL +
     * payload (and likewise for "blob/two") - both keys are the same length
     * here, so the two entries' overhead is identical. */
    size_t blob_one_size = strlen("blob/one") + 1 + sizeof(payload);
    size_t blob_two_size = strlen("blob/two") + 1 + sizeof(payload2);
    size_t used_before_delete = zmk_custom_setting_large_pool_used(test_blob_keyspace.large_pool);
    if (used_before_delete != blob_one_size + blob_two_size) {
        LOG_ERR("Unexpected keyspace pool usage before delete: %u", (unsigned)used_before_delete);
        return -EINVAL;
    }

    /* Deleting an entry must release its pool region, not just its slot -
     * zmk_custom_setting_keyspace_delete -> pool_release_locked. */
    ret = zmk_custom_setting_keyspace_delete(&test_blob_keyspace, "blob/one");
    if (ret < 0) {
        LOG_ERR("Deleting blob/one failed: %d", ret);
        return ret;
    }
    size_t used_after_delete = zmk_custom_setting_large_pool_used(test_blob_keyspace.large_pool);
    if (used_after_delete != used_before_delete - blob_one_size) {
        LOG_ERR("Keyspace pool usage did not drop after delete: before=%u after=%u",
                (unsigned)used_before_delete, (unsigned)used_after_delete);
        return -EINVAL;
    }

    ret = zmk_custom_setting_keyspace_delete(&test_blob_keyspace, "blob/two");
    if (ret < 0) {
        LOG_ERR("Deleting blob/two failed: %d", ret);
        return ret;
    }
    if (zmk_custom_setting_large_pool_used(test_blob_keyspace.large_pool) != 0) {
        LOG_ERR("Keyspace pool should be empty after deleting every entry");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_keyspace_large_value size=%u", (unsigned)sizeof(payload));
    return 0;
}

/*
 * Enabler A: prove the over-commit budget actually bites before max_entries
 * does. TEST_OVERCOMMIT_POOL_SIZE fits exactly 3 full-size entries; a 4th
 * full-size entry must fail with -ENOSPC even though 5 of the keyspace's 8
 * slots are still free.
 */
static int test_keyspace_pool_overcommit(void) {
    if (zmk_custom_setting_large_pool_used(test_overcommit_keyspace.large_pool) != 0) {
        LOG_ERR("Overcommit keyspace pool should start empty");
        return -EINVAL;
    }

    struct zmk_custom_setting_value value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .size = TEST_OVERCOMMIT_MAX_SIZE,
    };
    fill_pool_test_pattern(value.bytes_value, TEST_OVERCOMMIT_MAX_SIZE, 3);

    char key[16];
    int created_count = 0;
    for (int i = 0; i < TEST_OVERCOMMIT_MAX_ENTRIES; i++) {
        snprintf(key, sizeof(key), "oc/%d", i);
        int ret = zmk_custom_setting_keyspace_create(&test_overcommit_keyspace, key, &value,
                                                     ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
        if (ret == -ENOSPC) {
            break;
        }
        if (ret < 0) {
            LOG_ERR("Unexpected error creating overcommit entry %d: %d", i, ret);
            return ret;
        }
        created_count++;
    }

    /* The pool (3 * (12 + 16) = 84 bytes) fits at most 3 entries whose blob
     * is "oc/<i>\0" (5-6 bytes) + 16-byte payload (~21-22 bytes each) - so
     * capacity runs out well before all 8 slots are used. */
    if (created_count >= TEST_OVERCOMMIT_MAX_ENTRIES) {
        LOG_ERR("Overcommit pool should have run out before filling every slot (created %d/%d)",
                created_count, TEST_OVERCOMMIT_MAX_ENTRIES);
        return -EINVAL;
    }
    if (created_count == 0) {
        LOG_ERR("Overcommit pool should fit at least one full-size entry");
        return -EINVAL;
    }

    for (int i = 0; i < created_count; i++) {
        snprintf(key, sizeof(key), "oc/%d", i);
        zmk_custom_setting_keyspace_delete(&test_overcommit_keyspace, key);
    }
    if (zmk_custom_setting_large_pool_used(test_overcommit_keyspace.large_pool) != 0) {
        LOG_ERR("Overcommit keyspace pool should be empty after deleting every entry");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_keyspace_pool_overcommit created=%d max_entries=%d",
            created_count, TEST_OVERCOMMIT_MAX_ENTRIES);
    return 0;
}

/* Runs after every settings_load / discard-driven settings_load_subtree above:
 * zmk_custom_settings_initialized must have fired exactly once, at the very
 * first (boot) load, and not again on any of the later targeted reloads. */
static int test_initialized_event(void) {
    if (initialized_event_count != 1) {
        LOG_ERR("Expected zmk_custom_settings_initialized to fire exactly once, got %u",
                initialized_event_count);
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_initialized_event count=%u", initialized_event_count);
    return 0;
}

static int custom_settings_test_init(void) {
    int ret = test_settings_backend_init();
    if (ret < 0) {
        return ret;
    }

    ret = test_constraint_validation();
    if (ret < 0) {
        return ret;
    }

    ret = test_bytes_rpc_converters();
    if (ret < 0) {
        return ret;
    }

    ret = test_scalar_lifecycle();
    if (ret < 0) {
        return ret;
    }

    ret = test_save_default_deletes_record();
    if (ret < 0) {
        return ret;
    }

    ret = test_array_lifecycle();
    if (ret < 0) {
        return ret;
    }

    ret = test_array_insert_remove();
    if (ret < 0) {
        return ret;
    }

    ret = test_array_storage_backward_compat();
    if (ret < 0) {
        return ret;
    }

    ret = test_view_based_api();
    if (ret < 0) {
        return ret;
    }

    ret = test_temporary_override_pool();
    if (ret < 0) {
        return ret;
    }

    ret = test_view_pool_temp_slot_leak();
    if (ret < 0) {
        return ret;
    }

    ret = test_boot_default_override();
    if (ret < 0) {
        return ret;
    }

    ret = test_keyspace_lifecycle();
    if (ret < 0) {
        return ret;
    }

    ret = test_keyspace_find_null_subsystem();
    if (ret < 0) {
        return ret;
    }

    ret = test_record_settings();
    if (ret < 0) {
        return ret;
    }

    ret = test_large_value();
    if (ret < 0) {
        return ret;
    }

    ret = test_large_record();
    if (ret < 0) {
        return ret;
    }

    ret = test_string_no_truncation();
    if (ret < 0) {
        return ret;
    }

    ret = test_large_value_pool();
    if (ret < 0) {
        return ret;
    }

    ret = test_keyspace_large_value();
    if (ret < 0) {
        return ret;
    }

    ret = test_keyspace_pool_overcommit();
    if (ret < 0) {
        return ret;
    }

    ret = test_initialized_event();
    if (ret < 0) {
        return ret;
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    return test_behavior_value_type();
#else
    return 0;
#endif
}

SYS_INIT(custom_settings_test_init, APPLICATION, 99);
