/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <cormoran/zmk/custom_settings.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>

static struct zmk_rpc_custom_subsystem_meta zmk_config_sample_custom_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool zmk_config_sample_handle_request(const zmk_custom_CallRequest *req,
                                             pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);

    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(zmk_config_sample, &zmk_config_sample_custom_settings_meta,
                         zmk_config_sample_handle_request);
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS)
static int zmk_config_sample_reverse_bytes(const struct zmk_custom_setting *setting,
                                           const uint8_t *src, size_t src_size, uint8_t *dest,
                                           size_t *dest_size, size_t dest_capacity) {
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
#endif /* CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS */

static const struct zmk_custom_setting_constraint zmk_config_sample_no_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_NONE},
};

static const struct zmk_custom_setting_constraint zmk_config_sample_range_0_100[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 100}}},
};

static const struct zmk_custom_setting_constraint zmk_config_sample_range_0_10[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 10}}},
};

static const struct zmk_custom_setting_constraint zmk_config_sample_hid_usage_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE,
     .hid_usage = {.usage_page = HID_USAGE_KEY,
                   .usage_min = HID_USAGE_KEY_KEYBOARD_A,
                   .usage_max = HID_USAGE_KEY_KEYBOARD_Z}},
};

static const struct zmk_custom_setting_constraint zmk_config_sample_layer_id_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID},
};

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
static const struct zmk_custom_setting_constraint zmk_config_sample_behavior_id_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID},
};
#endif

/* Registered fields directly build struct zmk_custom_setting instead of
 * using the ZMK_CUSTOM_SETTING_DEFINE macros, to exercise that path too.
 * Simplification P4: hand-built descriptors must be declared `const` (the
 * iterable section is ROM now), point `state` at a hand-declared RAM
 * struct zmk_custom_setting_state (temp_slot = -1, like the macros emit),
 * and - for BYTES/STRING - provide their own exact-size store buffer with
 * state.blob.data pointing at it plus a matching descriptor
 * blob.max_size. */
static const struct zmk_custom_setting_value zmk_config_sample_int32_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 42};

static struct zmk_custom_setting_state zmk_config_sample_int32_state = {.temp_slot = -1};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_int32) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "int32_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_range_0_100,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_range_0_100),
    .default_value = &zmk_config_sample_int32_default,
    .state = &zmk_config_sample_int32_state,
};

static const struct zmk_custom_setting_value zmk_config_sample_bool_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, .bool_value = true};

static struct zmk_custom_setting_state zmk_config_sample_bool_state = {.temp_slot = -1};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_bool) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "bool_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = &zmk_config_sample_bool_default,
    .state = &zmk_config_sample_bool_state,
};

static const struct zmk_custom_setting_value zmk_config_sample_string_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    .size = sizeof("hello zmk") - 1,
    .string_value = "hello zmk"};

static uint8_t zmk_config_sample_string_store[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
static struct zmk_custom_setting_state zmk_config_sample_string_state = {
    .temp_slot = -1,
    .blob.data = zmk_config_sample_string_store,
};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_string) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "string_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = &zmk_config_sample_string_default,
    .blob = {.max_size = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE, .pool = NULL},
    .state = &zmk_config_sample_string_state,
};

static const struct zmk_custom_setting_value zmk_config_sample_bytes_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    .size = 4,
    .bytes_value = {0x01, 0x02, 0x03, 0x04}};

static uint8_t zmk_config_sample_bytes_store[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
static struct zmk_custom_setting_state zmk_config_sample_bytes_state = {
    .temp_slot = -1,
    .blob.data = zmk_config_sample_bytes_store,
};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_bytes) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "bytes_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = &zmk_config_sample_bytes_default,
    .blob = {.max_size = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE, .pool = NULL},
    .state = &zmk_config_sample_bytes_state,
};

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS)
static const struct zmk_custom_setting_value zmk_config_sample_bytes_rpc_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    .size = 4,
    .bytes_value = {0x11, 0x22, 0x33, 0x44}};

static uint8_t zmk_config_sample_bytes_rpc_store[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
static struct zmk_custom_setting_state zmk_config_sample_bytes_rpc_state = {
    .temp_slot = -1,
    .blob.data = zmk_config_sample_bytes_rpc_store,
};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_bytes_rpc) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "bytes_rpc_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = &zmk_config_sample_bytes_rpc_default,
    .rpc_serializer = zmk_config_sample_reverse_bytes,
    .rpc_deserializer = zmk_config_sample_reverse_bytes,
    .blob = {.max_size = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE, .pool = NULL},
    .state = &zmk_config_sample_bytes_rpc_state,
};
#endif /* CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS */

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES)
/*
 * issue #16: a BYTES setting able to store a value larger than the fixed
 * carrier, using the ZMK_CUSTOM_SETTING_DEFINE_SIZED macro. When
 * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE is left at the default
 * (equal to the carrier size) this is just a normal setting; the sample
 * builds raise it so this stores up to 256 bytes, reachable over the chunked
 * RPC / read_into / write_bytes API.
 */
ZMK_CUSTOM_SETTING_DEFINE_SIZED(
    zmk_config_sample_large_bytes, CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,
    "zmk_config_sample", "large_bytes_value", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    ZMK_CUSTOM_SETTING_VALUE_BYTES(0), ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/*
 * A large BYTES setting sharing a static ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE
 * budget instead of reserving its own dedicated buffer
 * (ZMK_CUSTOM_SETTING_DEFINE_SIZED above) - the pool sizing to use when
 * several large settings should not each cost their own worst-case max_size
 * while empty.
 */
ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(zmk_config_sample_pool,
                                     CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE);

ZMK_CUSTOM_SETTING_DEFINE_POOLED(
    zmk_config_sample_pooled_bytes, CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,
    zmk_config_sample_pool, "zmk_config_sample", "pooled_bytes_value",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, ZMK_CUSTOM_SETTING_VALUE_BYTES(0),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#endif /* CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES */

static const struct zmk_custom_setting_value zmk_config_sample_private_string_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    .size = sizeof("device only") - 1,
    .string_value = "device only"};

static uint8_t
    zmk_config_sample_private_string_store[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
static struct zmk_custom_setting_state zmk_config_sample_private_string_state = {
    .temp_slot = -1,
    .blob.data = zmk_config_sample_private_string_store,
};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_private_string) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "device_private_string",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = &zmk_config_sample_private_string_default,
    .blob = {.max_size = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE, .pool = NULL},
    .state = &zmk_config_sample_private_string_state,
};

static const struct zmk_custom_setting_value zmk_config_sample_secure_int32_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 7};

static struct zmk_custom_setting_state zmk_config_sample_secure_int32_state = {.temp_slot = -1};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_secure_int32) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "secure_int32",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    .constraints = zmk_config_sample_range_0_10,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_range_0_10),
    .default_value = &zmk_config_sample_secure_int32_default,
    .state = &zmk_config_sample_secure_int32_state,
};

static const struct zmk_custom_setting_value zmk_config_sample_hid_usage_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .int32_value = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_A)};

static struct zmk_custom_setting_state zmk_config_sample_hid_usage_state = {.temp_slot = -1};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_hid_usage) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "hid_usage",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_hid_usage_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_hid_usage_constraints),
    .default_value = &zmk_config_sample_hid_usage_default,
    .state = &zmk_config_sample_hid_usage_state,
};

static const struct zmk_custom_setting_value zmk_config_sample_layer_id_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0};

static struct zmk_custom_setting_state zmk_config_sample_layer_id_state = {.temp_slot = -1};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_layer_id) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "layer_id",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_layer_id_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_layer_id_constraints),
    .default_value = &zmk_config_sample_layer_id_default,
    .state = &zmk_config_sample_layer_id_state,
};

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
static const struct zmk_custom_setting_value zmk_config_sample_behavior_id_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0};

static struct zmk_custom_setting_state zmk_config_sample_behavior_id_state = {.temp_slot = -1};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_behavior_id) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "behavior_id",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_behavior_id_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_behavior_id_constraints),
    .default_value = &zmk_config_sample_behavior_id_default,
    .state = &zmk_config_sample_behavior_id_state,
};

static const struct zmk_custom_setting_value zmk_config_sample_behavior_value_default = {
    .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
    .behavior_value = {.behavior_id = 0, .param1 = 0, .param2 = 0}};

static struct zmk_custom_setting_state zmk_config_sample_behavior_value_state = {.temp_slot = -1};

const STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_behavior_value) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "behavior_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = &zmk_config_sample_behavior_value_default,
    .state = &zmk_config_sample_behavior_value_state,
};
#endif

/*
 * P3 rework: array settings now own a single contiguous backing buffer plus
 * per-slot bookkeeping (struct zmk_custom_setting_array_state) instead of
 * plain scalar-sized fields duplicated onto one STRUCT_SECTION_ITERABLE
 * instance per element - hand-building that state here would just
 * reimplement what ZMK_CUSTOM_SETTING_ARRAY_DEFINE already does, without
 * adding meaningful extra test coverage over the hand-built scalar
 * STRUCT_SECTION_ITERABLE instances above. Use the macro for the array case
 * instead; the scalar settings above still exercise the "hand-built
 * STRUCT_SECTION_ITERABLE instead of macros" path.
 */
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(zmk_config_sample_array_defaults, 10, 20, 30, 40);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(zmk_config_sample_array, "zmk_config_sample", "array_value",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, 4, 4,
                                zmk_config_sample_array_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));
#endif /* CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY */

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
/*
 * Simplification P3: an RPC-creatable keyspace sample. Studio clients can
 * create/delete named STRING entries under the "profile/" prefix at runtime
 * (CreateSetting/DeleteSetting); each entry persists under an ordinal
 * storage record ("zmk_config_sample/profile/#<slot>") whose value is the
 * [user_key\0][payload] blob, so the user-chosen key survives reboot without
 * a separate key table. Included in the sample builds so the keyspace path
 * is exercised on real hardware, not only under native_sim.
 */
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(zmk_config_sample_profiles, "zmk_config_sample", "profile/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                                   /* max_size = */ 32, /* max_key_len = */ 24,
                                   /* max_entries = */ 4,
                                   ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                   ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#endif /* CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE */
