/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <cormoran/runtime_combo/runtime_combo.h>
#include <cormoran/zmk/custom_settings.h>

#include <dt-bindings/zmk/keys.h>
#include <drivers/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* The behavior binding is stored separately, as a parallel BEHAVIOR-typed
 * custom-settings array (see ZMK_RUNTIME_COMBO_BEHAVIORS_KEY), so the packed
 * body below only carries the rest of a combo's configuration. */
#define RUNTIME_COMBO_STORAGE_VERSION 4
#define RUNTIME_COMBO_FLAG_ENABLED BIT(0)
#define RUNTIME_COMBO_FLAG_SLOW_RELEASE_OVERRIDE BIT(1)
#define RUNTIME_COMBO_FLAG_SLOW_RELEASE_VALUE BIT(2)
#define RUNTIME_COMBO_PACKED_HEADER_SIZE 12
#define RUNTIME_COMBO_PACKED_MAX_SIZE                                                              \
    (RUNTIME_COMBO_PACKED_HEADER_SIZE +                                                            \
     (CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO * sizeof(uint16_t)))

BUILD_ASSERT(RUNTIME_COMBO_PACKED_MAX_SIZE <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
             "Runtime combo packed value must fit in one custom setting bytes value");

#define RUNTIME_COMBO_EMPTY_BYTES                                                                  \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, .size = 0})

/* ZMK_CUSTOM_SETTING_ARRAY_DEFINE seeds every element slot (all max_size, not
 * just default_size) from defaults[index] at init, so each array needs a
 * full-length defaults table. Bare aggregate initializers (not the
 * ZMK_CUSTOM_SETTING_VALUE_* compound-literal macros) keep these valid
 * constant initializers under strict -std=c11, matching the reason
 * ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE avoids the macros. Every
 * default is "empty"; combined with default_size 0 the arrays start empty and
 * grow on write, so an un-grown element reads back -ENOENT and the read paths
 * fall back to any compile-time default. */
#define RUNTIME_COMBO_COMBOS_DEFAULT(i, _) {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, .size = 0}
#define RUNTIME_COMBO_NAMES_DEFAULT(i, _)                                                          \
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING, .size = 0, .string_value = ""}
#define RUNTIME_COMBO_BEHAVIORS_DEFAULT(i, _)                                                      \
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,                                               \
     .behavior_value = {.behavior_id = 0, .param1 = 0, .param2 = 0}}

static const struct zmk_custom_setting_value
    runtime_combo_combos_defaults[CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS] = {
        LISTIFY(CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, RUNTIME_COMBO_COMBOS_DEFAULT, (, ))};
static const struct zmk_custom_setting_value
    runtime_combo_names_defaults[CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS] = {
        LISTIFY(CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, RUNTIME_COMBO_NAMES_DEFAULT, (, ))};
static const struct zmk_custom_setting_value
    runtime_combo_behaviors_defaults[CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS] = {
        LISTIFY(CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, RUNTIME_COMBO_BEHAVIORS_DEFAULT, (, ))};

/* One registration per array (custom-settings P3 API): each array owns a
 * single contiguous backing buffer sized by max_count, instead of N separate
 * per-element registrations. Element access still happens by base key + index
 * via zmk_custom_setting_*_array_* below. The combo body and name arrays are
 * joined by a parallel BEHAVIOR-typed array so each combo's behavior binding is
 * stored and validated as a first-class binding rather than opaque bytes. */
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(runtime_combo_combos, ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                ZMK_RUNTIME_COMBO_COMBOS_KEY, ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, 0,
                                runtime_combo_combos_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(runtime_combo_names, ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                ZMK_RUNTIME_COMBO_NAMES_KEY, ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                                CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, 0,
                                runtime_combo_names_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(
    runtime_combo_behaviors, ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_BEHAVIORS_KEY,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR, CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, 0,
    runtime_combo_behaviors_defaults, ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/*
 * Global (non per-combo) tuning settings. Registered through the public
 * ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS macro: since the custom-settings
 * simplification (P4) the descriptor is a const, flash-resident object with a
 * separate RAM state block, so it can no longer be hand-built as a
 * STRUCT_SECTION_ITERABLE with an inline `.temp_slot`/`.default_value`.
 */
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    runtime_combo_timeout_ms, ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_TIMEOUT_MS_KEY,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_RUNTIME_COMBO_DEFAULT_TIMEOUT_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(1, 65535));

ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    runtime_combo_slow_release, ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_SLOW_RELEASE_KEY,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, ZMK_CUSTOM_SETTING_VALUE_BOOL(false),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    runtime_combo_require_prior_idle_ms, ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
    ZMK_RUNTIME_COMBO_REQUIRE_PRIOR_IDLE_MS_KEY, ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_RUNTIME_COMBO_DEFAULT_REQUIRE_PRIOR_IDLE_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(0, 65535));

/* Compile-time defaults from an optional `cormoran,runtime-combo-defaults` node. */
struct zmk_runtime_combo_default {
    uint32_t slot;
    const char *name;
    struct zmk_runtime_combo_config config;
};

#define DT_DRV_COMPAT cormoran_runtime_combo_defaults

/* ARRAY_SIZE()-based RUNTIME_COMBO_DEFAULT_COUNT below uses sizeof, so it
 * cannot be used in `#if`; this preprocessor-evaluable flag can. */
#define RUNTIME_COMBO_HAS_DEFAULTS DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define RUNTIME_COMBO_DEFAULT_PROP_BIT_AT_IDX(n, prop, idx) BIT(DT_PROP_BY_IDX(n, prop, idx))
#define RUNTIME_COMBO_DEFAULT_LAYER_MASK(n)                                                        \
    COND_CODE_1(DT_NODE_HAS_PROP(n, layers),                                                       \
                (DT_FOREACH_PROP_ELEM_SEP(n, layers, RUNTIME_COMBO_DEFAULT_PROP_BIT_AT_IDX, (|))), \
                (0))

#define RUNTIME_COMBO_DEFAULT_SLOW_RELEASE(n)                                                      \
    (DT_PROP(n, slow_release) ? ZMK_RUNTIME_COMBO_SLOW_RELEASE_ON                                  \
                              : ZMK_RUNTIME_COMBO_SLOW_RELEASE_INHERIT)

#define RUNTIME_COMBO_DEFAULT_VALIDATE(n)                                                          \
    BUILD_ASSERT(DT_PROP_LEN(n, key_positions) >= 2,                                               \
                 "runtime combo default must have at least two key positions");                    \
    BUILD_ASSERT(DT_PROP_LEN(n, key_positions) <=                                                  \
                     CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO,                             \
                 "runtime combo default has too many key positions");                              \
    BUILD_ASSERT(DT_PROP_OR(n, slot, DT_NODE_CHILD_IDX(n)) < CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS,  \
                 "runtime combo default slot is out of range");

DT_INST_FOREACH_CHILD(0, RUNTIME_COMBO_DEFAULT_VALIDATE)

#define RUNTIME_COMBO_DEFAULT_INST(n)                                                              \
    {                                                                                              \
        .slot = DT_PROP_OR(n, slot, DT_NODE_CHILD_IDX(n)),                                         \
        .name = DT_PROP_OR(n, display_name, DT_NODE_FULL_NAME(n)),                                 \
        .config =                                                                                  \
            {                                                                                      \
                .enabled = true,                                                                   \
                .key_position_len = DT_PROP_LEN(n, key_positions),                                 \
                .layer_mask = RUNTIME_COMBO_DEFAULT_LAYER_MASK(n),                                 \
                .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                                      \
                .timeout_ms = DT_PROP_OR(n, timeout_ms, 0),                                        \
                .require_prior_idle_ms = DT_PROP_OR(n, require_prior_idle_ms, 0),                  \
                .slow_release_override = RUNTIME_COMBO_DEFAULT_SLOW_RELEASE(n),                    \
                .key_positions = DT_PROP(n, key_positions),                                        \
            },                                                                                     \
    },

static const struct zmk_runtime_combo_default runtime_combo_defaults[] = {
    DT_INST_FOREACH_CHILD(0, RUNTIME_COMBO_DEFAULT_INST)};

#define RUNTIME_COMBO_DEFAULT_COUNT ARRAY_SIZE(runtime_combo_defaults)

#else

static const struct zmk_runtime_combo_default runtime_combo_defaults[1];

#define RUNTIME_COMBO_DEFAULT_COUNT 0

#endif

static const struct zmk_runtime_combo_default *runtime_combo_find_default(uint32_t index) {
    for (size_t i = 0; i < RUNTIME_COMBO_DEFAULT_COUNT; i++) {
        if (runtime_combo_defaults[i].slot == index) {
            return &runtime_combo_defaults[i];
        }
    }
    return NULL;
}

/* RAM cache of decoded, effective combos (after default fallback), so the
 * key-event hot path never touches the custom-settings registry or the
 * packed-byte decoder. `key_positions` is uint16_t here (not the oversized
 * int32_t used by zmk_runtime_combo_config) since values are already
 * range-checked on write. */
struct runtime_combo_cache_entry {
    bool valid;
    bool enabled;
    uint8_t key_position_len;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    uint16_t timeout_ms;
    uint16_t require_prior_idle_ms;
    enum zmk_runtime_combo_slow_release_override slow_release_override;
    uint16_t key_positions[CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO];
};

static struct runtime_combo_cache_entry runtime_combo_cache[CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS];
/* SYS_INIT runs before main() calls settings_load(), so an eager cache build
 * at init would only ever see compile-time defaults. Instead the cache is
 * built lazily on the first real position event, which is guaranteed to
 * happen after settings_load() completes. */
static bool runtime_combo_cache_ready;

static struct zmk_runtime_combo_global_settings runtime_combo_global_cache;
static bool runtime_combo_global_cache_valid;

struct runtime_combo_active {
    uint16_t combo_idx;
    uint8_t key_positions_pressed_count;
    struct zmk_position_state_changed_event
        key_positions_pressed[CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO];
};

static uint8_t pending_count;
static struct zmk_position_state_changed_event
    pending_keys[CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO];
static struct runtime_combo_active active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS];
static uint8_t active_combo_count;
static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;
static int64_t last_tapped_timestamp = INT32_MIN;
static int64_t last_combo_timestamp = INT32_MIN;
/* Index of a combo whose positions are all pressed but which is being held back
 * because a strict superset combo is still viable. -1 means none. */
static int32_t fully_pressed_combo_idx = -1;

static void finish_pending_and_activate(uint32_t combo_idx,
                                        const struct zmk_runtime_combo_config *combo);

static void put_u16(uint8_t *dest, uint16_t value) { sys_put_le16(value, dest); }
static void put_u32(uint8_t *dest, uint32_t value) { sys_put_le32(value, dest); }
static uint16_t get_u16(const uint8_t *src) { return sys_get_le16(src); }
static uint32_t get_u32(const uint8_t *src) { return sys_get_le32(src); }

static const struct zmk_custom_setting *combo_setting(uint32_t index) {
    return zmk_custom_setting_find_array_element(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                 ZMK_RUNTIME_COMBO_COMBOS_KEY, index);
}

static const struct zmk_custom_setting *name_setting(uint32_t index) {
    return zmk_custom_setting_find_array_element(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                 ZMK_RUNTIME_COMBO_NAMES_KEY, index);
}

static const struct zmk_custom_setting *behavior_setting(uint32_t index) {
    return zmk_custom_setting_find_array_element(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                 ZMK_RUNTIME_COMBO_BEHAVIORS_KEY, index);
}

/* Read a combo's behavior binding from its BEHAVIOR-typed array element and
 * resolve the stored local ID back to a behavior device name. */
static int read_combo_behavior(uint32_t index, struct zmk_behavior_binding *behavior) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                   ZMK_RUNTIME_COMBO_BEHAVIORS_KEY, index, &value);
    if (ret < 0) {
        return ret;
    }
    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR) {
        return -EINVAL;
    }
    zmk_behavior_local_id_t behavior_id = value.behavior_value.behavior_id;
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    if (!behavior_name) {
        return -ENODEV;
    }
    *behavior = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .param1 = value.behavior_value.param1,
        .param2 = value.behavior_value.param2,
    };
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    behavior->local_id = behavior_id;
#endif
    return 0;
}

/* Persist a combo's behavior binding into its BEHAVIOR-typed array element,
 * growing the array to cover `index` if needed. The custom-settings write path
 * validates the binding against the target behavior's parameter metadata. */
static int write_combo_behavior(uint32_t index, const struct zmk_behavior_binding *behavior,
                                bool persist) {
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(behavior->behavior_dev);
    if (behavior_id == 0 || behavior_id == UINT16_MAX) {
        return -ENODEV;
    }
    const struct zmk_custom_setting *setting = behavior_setting(index);
    if (!setting) {
        return -ENOENT;
    }
    struct zmk_custom_setting_value value =
        ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(behavior_id, behavior->param1, behavior->param2);
    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    return zmk_custom_setting_write_array_element(setting, &value, array_size,
                                                  persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                          : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

static uint32_t runtime_combo_default_upper_bound(void) {
    uint32_t upper_bound = 0;
    for (size_t i = 0; i < RUNTIME_COMBO_DEFAULT_COUNT; i++) {
        upper_bound = MAX(upper_bound, runtime_combo_defaults[i].slot + 1);
    }
    return upper_bound;
}

uint32_t zmk_runtime_combo_count(void) {
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_COMBOS_KEY);
    uint32_t array_size = setting ? zmk_custom_setting_array_size(setting) : 0;
    /* Slots covered only by a compile-time default may never have grown the
     * underlying custom-settings array, but must still be matched/listed. */
    return MAX(array_size, runtime_combo_default_upper_bound());
}

uint32_t zmk_runtime_combo_max_count(void) { return CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS; }

uint32_t zmk_runtime_combo_default_count(void) { return RUNTIME_COMBO_DEFAULT_COUNT; }

bool zmk_runtime_combo_has_default(uint32_t index) {
    return runtime_combo_find_default(index) != NULL;
}

int zmk_runtime_combo_read_default(uint32_t index, struct zmk_runtime_combo_config *combo) {
    if (!combo) {
        return -EINVAL;
    }
    const struct zmk_runtime_combo_default *def = runtime_combo_find_default(index);
    if (!def) {
        return -ENOENT;
    }
    *combo = def->config;
    return 0;
}

static int packed_to_combo(const struct zmk_custom_setting_value *value,
                           struct zmk_runtime_combo_config *combo) {
    if (value->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES) {
        return -EINVAL;
    }
    if (value->size == 0) {
        *combo = (struct zmk_runtime_combo_config){0};
        return 0;
    }

    uint8_t version = value->bytes_value[0];
    if (version != RUNTIME_COMBO_STORAGE_VERSION) {
        return -EINVAL;
    }
    if (value->size < RUNTIME_COMBO_PACKED_HEADER_SIZE) {
        return -EINVAL;
    }

    uint8_t key_position_len = value->bytes_value[2];
    if (key_position_len < 2 ||
        key_position_len > CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO ||
        value->size != RUNTIME_COMBO_PACKED_HEADER_SIZE + key_position_len * sizeof(uint16_t)) {
        return -EINVAL;
    }

    uint8_t flags = value->bytes_value[1];
    enum zmk_runtime_combo_slow_release_override slow_release_override =
        ZMK_RUNTIME_COMBO_SLOW_RELEASE_INHERIT;
    if (flags & RUNTIME_COMBO_FLAG_SLOW_RELEASE_OVERRIDE) {
        slow_release_override = (flags & RUNTIME_COMBO_FLAG_SLOW_RELEASE_VALUE)
                                    ? ZMK_RUNTIME_COMBO_SLOW_RELEASE_ON
                                    : ZMK_RUNTIME_COMBO_SLOW_RELEASE_OFF;
    }

    /* The behavior binding lives in a parallel BEHAVIOR-typed array and is
     * filled in by the caller (see read_combo_behavior). */
    *combo = (struct zmk_runtime_combo_config){
        .enabled = (flags & RUNTIME_COMBO_FLAG_ENABLED) != 0,
        .key_position_len = key_position_len,
        .layer_mask = get_u32(&value->bytes_value[4]),
        .timeout_ms = get_u16(&value->bytes_value[8]),
        .require_prior_idle_ms = get_u16(&value->bytes_value[10]),
        .slow_release_override = slow_release_override,
    };

    for (uint8_t i = 0; i < key_position_len; i++) {
        combo->key_positions[i] =
            get_u16(&value->bytes_value[RUNTIME_COMBO_PACKED_HEADER_SIZE + i * sizeof(uint16_t)]);
    }
    return 0;
}

static int combo_to_packed(const struct zmk_runtime_combo_config *combo,
                           struct zmk_custom_setting_value *value) {
    if (combo->key_position_len < 2 ||
        combo->key_position_len > CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        if (combo->key_positions[i] < 0 || combo->key_positions[i] > UINT16_MAX) {
            return -ERANGE;
        }
    }

    *value = (struct zmk_custom_setting_value){
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .size = RUNTIME_COMBO_PACKED_HEADER_SIZE + combo->key_position_len * sizeof(uint16_t),
    };
    uint8_t flags = combo->enabled ? RUNTIME_COMBO_FLAG_ENABLED : 0;
    if (combo->slow_release_override != ZMK_RUNTIME_COMBO_SLOW_RELEASE_INHERIT) {
        flags |= RUNTIME_COMBO_FLAG_SLOW_RELEASE_OVERRIDE;
        if (combo->slow_release_override == ZMK_RUNTIME_COMBO_SLOW_RELEASE_ON) {
            flags |= RUNTIME_COMBO_FLAG_SLOW_RELEASE_VALUE;
        }
    }
    value->bytes_value[0] = RUNTIME_COMBO_STORAGE_VERSION;
    value->bytes_value[1] = flags;
    value->bytes_value[2] = combo->key_position_len;
    value->bytes_value[3] = 0;
    put_u32(&value->bytes_value[4], combo->layer_mask);
    put_u16(&value->bytes_value[8], combo->timeout_ms);
    put_u16(&value->bytes_value[10], combo->require_prior_idle_ms);

    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        put_u16(&value->bytes_value[RUNTIME_COMBO_PACKED_HEADER_SIZE + i * sizeof(uint16_t)],
                combo->key_positions[i]);
    }
    return 0;
}

int zmk_runtime_combo_read(uint32_t index, struct zmk_runtime_combo_config *combo) {
    if (!combo || index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                   ZMK_RUNTIME_COMBO_COMBOS_KEY, index, &value);
    if (ret < 0) {
        /* -ENOENT also covers "within array_max_size but the array hasn't
         * grown this far yet", which is indistinguishable from an explicit
         * empty value as far as the default fallback is concerned. */
        if (ret == -ENOENT) {
            const struct zmk_runtime_combo_default *def = runtime_combo_find_default(index);
            if (def) {
                *combo = def->config;
                return 0;
            }
        }
        return ret;
    }
    if (value.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES && value.size == 0) {
        const struct zmk_runtime_combo_default *def = runtime_combo_find_default(index);
        if (def) {
            *combo = def->config;
            return 0;
        }
    }
    ret = packed_to_combo(&value, combo);
    if (ret < 0 || combo->key_position_len == 0) {
        /* An empty (size 0) record with no default decodes to a zeroed combo
         * that has no behavior binding to read. */
        return ret;
    }
    return read_combo_behavior(index, &combo->behavior);
}

int zmk_runtime_combo_read_name(uint32_t index, char *name, size_t name_size) {
    if (!name || name_size == 0 || index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                   ZMK_RUNTIME_COMBO_NAMES_KEY, index, &value);
    if (ret < 0) {
        if (ret == -ENOENT) {
            const struct zmk_runtime_combo_default *def = runtime_combo_find_default(index);
            if (def) {
                strncpy(name, def->name, name_size - 1);
                name[name_size - 1] = '\0';
                return 0;
            }
        }
        name[0] = '\0';
        return ret;
    }
    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        return -EINVAL;
    }
    if (value.string_value[0] == '\0') {
        const struct zmk_runtime_combo_default *def = runtime_combo_find_default(index);
        if (def) {
            strncpy(name, def->name, name_size - 1);
            name[name_size - 1] = '\0';
            return 0;
        }
    }
    strncpy(name, value.string_value, name_size - 1);
    name[name_size - 1] = '\0';
    return 0;
}

static void runtime_combo_cache_rebuild_slot(uint32_t index) {
    struct runtime_combo_cache_entry *entry = &runtime_combo_cache[index];
    struct zmk_runtime_combo_config combo;
    if (zmk_runtime_combo_read(index, &combo) < 0) {
        *entry = (struct runtime_combo_cache_entry){.valid = false};
        return;
    }

    *entry = (struct runtime_combo_cache_entry){
        .valid = true,
        .enabled = combo.enabled,
        .key_position_len = combo.key_position_len,
        .layer_mask = combo.layer_mask,
        .behavior = combo.behavior,
        .timeout_ms = combo.timeout_ms,
        .require_prior_idle_ms = combo.require_prior_idle_ms,
        .slow_release_override = combo.slow_release_override,
    };
    for (uint8_t i = 0; i < combo.key_position_len; i++) {
        entry->key_positions[i] = (uint16_t)combo.key_positions[i];
    }
}

static void runtime_combo_cache_rebuild_all(void) {
    for (uint32_t i = 0; i < CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS; i++) {
        runtime_combo_cache_rebuild_slot(i);
    }
}

void zmk_runtime_combo_invalidate_cache(void) {
    runtime_combo_cache_rebuild_all();
    runtime_combo_global_cache_valid = false;
    runtime_combo_cache_ready = true;
}

static void runtime_combo_cache_ensure_ready(void) {
    if (!runtime_combo_cache_ready) {
        zmk_runtime_combo_invalidate_cache();
    }
}

static int runtime_combo_cache_read(uint32_t index, struct zmk_runtime_combo_config *combo) {
    if (index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }
    const struct runtime_combo_cache_entry *entry = &runtime_combo_cache[index];
    if (!entry->valid) {
        return -ENOENT;
    }

    *combo = (struct zmk_runtime_combo_config){
        .enabled = entry->enabled,
        .key_position_len = entry->key_position_len,
        .layer_mask = entry->layer_mask,
        .behavior = entry->behavior,
        .timeout_ms = entry->timeout_ms,
        .require_prior_idle_ms = entry->require_prior_idle_ms,
        .slow_release_override = entry->slow_release_override,
    };
    for (uint8_t i = 0; i < entry->key_position_len; i++) {
        combo->key_positions[i] = entry->key_positions[i];
    }
    return 0;
}

int zmk_runtime_combo_read_global_settings(struct zmk_runtime_combo_global_settings *settings) {
    if (!settings) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value timeout_value;
    int ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                             ZMK_RUNTIME_COMBO_TIMEOUT_MS_KEY, &timeout_value);
    if (ret < 0) {
        return ret;
    }
    if (timeout_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 ||
        timeout_value.int32_value < 1 || timeout_value.int32_value > UINT16_MAX) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value slow_release_value;
    ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                         ZMK_RUNTIME_COMBO_SLOW_RELEASE_KEY, &slow_release_value);
    if (ret < 0) {
        return ret;
    }
    if (slow_release_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value require_prior_idle_value;
    ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                         ZMK_RUNTIME_COMBO_REQUIRE_PRIOR_IDLE_MS_KEY,
                                         &require_prior_idle_value);
    if (ret < 0) {
        return ret;
    }
    if (require_prior_idle_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 ||
        require_prior_idle_value.int32_value < 0 ||
        require_prior_idle_value.int32_value > UINT16_MAX) {
        return -EINVAL;
    }

    *settings = (struct zmk_runtime_combo_global_settings){
        .timeout_ms = timeout_value.int32_value,
        .slow_release = slow_release_value.bool_value,
        .max_combo = zmk_runtime_combo_max_count(),
        .require_prior_idle_ms = require_prior_idle_value.int32_value,
    };
    return 0;
}

static int ensure_name_array_size(uint32_t index, bool persist) {
    const struct zmk_custom_setting *setting = name_setting(index);
    if (!setting) {
        return -ENOENT;
    }
    if (zmk_custom_setting_array_size(setting) > index) {
        return 0;
    }
    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    return zmk_custom_setting_write_array_element(
        setting, &ZMK_CUSTOM_SETTING_VALUE_STRING(""), array_size,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int zmk_runtime_combo_write(uint32_t index, const struct zmk_runtime_combo_config *combo,
                            bool persist) {
    if (!combo || index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    /* Resolve and validate the behavior binding up front so an invalid binding
     * fails before any array element is written or grown. */
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(combo->behavior.behavior_dev);
    if (behavior_id == 0 || behavior_id == UINT16_MAX) {
        return -ENODEV;
    }
    int ret = zmk_behavior_validate_binding(&combo->behavior);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value value;
    ret = combo_to_packed(combo, &value);
    if (ret < 0) {
        return ret;
    }

    const struct zmk_custom_setting *setting = combo_setting(index);
    if (!setting) {
        return -ENOENT;
    }

    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    int write_ret = zmk_custom_setting_write_array_element(
        setting, &value, array_size,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    int behavior_ret = write_combo_behavior(index, &combo->behavior, persist);
    int name_ret = ensure_name_array_size(index, persist);
    /* The in-memory value updates even if persisting to flash failed
     * (zmk_custom_setting_write_array_element applies memory_value before
     * attempting the flash save), so the cache must refresh regardless of
     * any call's outcome. */
    runtime_combo_cache_rebuild_slot(index);
    if (write_ret < 0) {
        return write_ret;
    }
    if (behavior_ret < 0) {
        return behavior_ret;
    }
    return name_ret;
}

int zmk_runtime_combo_write_name(uint32_t index, const char *name, bool persist) {
    if (!name || index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }
    const struct zmk_custom_setting *setting = name_setting(index);
    if (!setting) {
        return -ENOENT;
    }

    struct zmk_custom_setting_value value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    };
    strncpy(value.string_value, name, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    value.string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE] = '\0';
    value.size = strlen(value.string_value);

    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    return zmk_custom_setting_write_array_element(setting, &value, array_size,
                                                  persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                          : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int zmk_runtime_combo_write_timeout_ms(uint16_t timeout_ms, bool persist) {
    if (timeout_ms == 0) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(timeout_ms);
    int ret = zmk_custom_setting_write_by_key(
        ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_TIMEOUT_MS_KEY, &value,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    /* The in-memory value updates even if persisting to flash failed (see
     * zmk_runtime_combo_write()), so the cache must invalidate regardless of
     * this call's outcome. */
    runtime_combo_global_cache_valid = false;
    return ret;
}

int zmk_runtime_combo_write_slow_release(bool slow_release, bool persist) {
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_BOOL(slow_release);
    int ret = zmk_custom_setting_write_by_key(
        ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_SLOW_RELEASE_KEY, &value,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    runtime_combo_global_cache_valid = false;
    return ret;
}

int zmk_runtime_combo_write_require_prior_idle_ms(uint16_t require_prior_idle_ms, bool persist) {
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(require_prior_idle_ms);
    int ret = zmk_custom_setting_write_by_key(
        ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_REQUIRE_PRIOR_IDLE_MS_KEY, &value,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    runtime_combo_global_cache_valid = false;
    return ret;
}

int zmk_runtime_combo_delete(uint32_t index, bool persist) {
    struct zmk_runtime_combo_config combo;
    int ret = zmk_runtime_combo_read(index, &combo);
    if (ret < 0) {
        return ret;
    }
    combo.enabled = false;
    return zmk_runtime_combo_write(index, &combo, persist);
}

bool zmk_runtime_combo_has_override(uint32_t index) {
    if (index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return false;
    }
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                   ZMK_RUNTIME_COMBO_COMBOS_KEY, index, &value);
    return ret == 0 && value.size > 0;
}

int zmk_runtime_combo_reset(uint32_t index) {
    if (index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    /* zmk_custom_setting_reset() restores default_array_size, which is
     * shared by the whole array (not per-element): calling it here would
     * shrink every other combo's visible array_size back to 0. Persisting an
     * explicit empty value at just this index is observably identical to an
     * unset slot for zmk_runtime_combo_read()'s fallback, without that
     * side effect. */
    const struct zmk_custom_setting *setting = combo_setting(index);
    if (!setting) {
        return -ENOENT;
    }
    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    int ret = zmk_custom_setting_write_array_element(
        setting, &RUNTIME_COMBO_EMPTY_BYTES, array_size, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);

    const struct zmk_custom_setting *name = name_setting(index);
    if (name) {
        uint32_t name_array_size = MAX(zmk_custom_setting_array_size(name), index + 1);
        zmk_custom_setting_write_array_element(name, &ZMK_CUSTOM_SETTING_VALUE_STRING(""),
                                               name_array_size,
                                               ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    }
    /* The in-memory value updates even if persisting to flash failed (see
     * zmk_runtime_combo_write()), so the cache must refresh regardless of
     * the write's outcome. */
    runtime_combo_cache_rebuild_slot(index);
    return ret;
}

static bool combo_contains_position(const struct zmk_runtime_combo_config *combo,
                                    int32_t position) {
    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        if (combo->key_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static bool combo_active_on_layer(const struct zmk_runtime_combo_config *combo, uint8_t layer) {
    return combo->layer_mask == 0 || (combo->layer_mask & BIT(layer)) != 0;
}

static struct zmk_runtime_combo_global_settings current_global_settings(void) {
    struct zmk_runtime_combo_global_settings settings = {
        .timeout_ms = CONFIG_ZMK_RUNTIME_COMBO_DEFAULT_TIMEOUT_MS,
        .slow_release = false,
        .require_prior_idle_ms = CONFIG_ZMK_RUNTIME_COMBO_DEFAULT_REQUIRE_PRIOR_IDLE_MS,
    };
    if (zmk_runtime_combo_read_global_settings(&settings) < 0) {
        LOG_WRN("Using default runtime combo global settings");
    }
    return settings;
}

static struct zmk_runtime_combo_global_settings runtime_combo_cached_global_settings(void) {
    if (!runtime_combo_global_cache_valid) {
        runtime_combo_global_cache = current_global_settings();
        runtime_combo_global_cache_valid = true;
    }
    return runtime_combo_global_cache;
}

static uint16_t effective_timeout_ms(const struct zmk_runtime_combo_config *combo,
                                     const struct zmk_runtime_combo_global_settings *global) {
    return combo->timeout_ms != 0 ? combo->timeout_ms : global->timeout_ms;
}

static uint16_t
effective_require_prior_idle_ms(const struct zmk_runtime_combo_config *combo,
                                const struct zmk_runtime_combo_global_settings *global) {
    return combo->require_prior_idle_ms != 0 ? combo->require_prior_idle_ms
                                             : global->require_prior_idle_ms;
}

static bool effective_slow_release(const struct zmk_runtime_combo_config *combo,
                                   const struct zmk_runtime_combo_global_settings *global) {
    switch (combo->slow_release_override) {
    case ZMK_RUNTIME_COMBO_SLOW_RELEASE_ON:
        return true;
    case ZMK_RUNTIME_COMBO_SLOW_RELEASE_OFF:
        return false;
    default:
        return global->slow_release;
    }
}

static bool combo_matches_pending(const struct zmk_runtime_combo_config *combo,
                                  const struct zmk_runtime_combo_global_settings *global,
                                  int64_t timestamp) {
    if (!combo->enabled || pending_count == 0) {
        return false;
    }
    uint16_t timeout_ms = effective_timeout_ms(combo, global);
    if (pending_keys[0].data.timestamp + timeout_ms <= timestamp) {
        return false;
    }
    for (uint8_t i = 0; i < pending_count; i++) {
        if (!combo_contains_position(combo, pending_keys[i].data.position)) {
            return false;
        }
    }
    return true;
}

static bool is_quick_tap(const struct zmk_runtime_combo_config *combo,
                         const struct zmk_runtime_combo_global_settings *global,
                         int64_t timestamp) {
    uint16_t require_prior_idle_ms = effective_require_prior_idle_ms(combo, global);
    if (require_prior_idle_ms == 0 || last_tapped_timestamp == INT32_MIN) {
        return false;
    }
    return last_tapped_timestamp > last_combo_timestamp &&
           last_tapped_timestamp + require_prior_idle_ms > timestamp;
}

static int read_enabled_combo(uint32_t index, struct zmk_runtime_combo_config *combo) {
    int ret = runtime_combo_cache_read(index, combo);
    if (ret < 0 || !combo->enabled) {
        return ret < 0 ? ret : -ENOENT;
    }
    return 0;
}

static int count_candidates_for_position(int32_t position, int64_t timestamp) {
    int count = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();
    struct zmk_runtime_combo_global_settings global = runtime_combo_cached_global_settings();

    for (uint32_t i = 0; i < CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS; i++) {
        struct zmk_runtime_combo_config combo;
        if (read_enabled_combo(i, &combo) < 0) {
            continue;
        }
        if (!combo_contains_position(&combo, position) ||
            !combo_active_on_layer(&combo, highest_active_layer) ||
            is_quick_tap(&combo, &global, timestamp)) {
            continue;
        }
        if (pending_count == 0 || combo_matches_pending(&combo, &global, timestamp)) {
            count++;
        }
    }
    return count;
}

/* True if some combo strictly longer than `completed_len` still matches everything
 * currently pending, meaning a shorter fully-pressed combo should not fire yet. */
static bool has_longer_viable_candidate(uint8_t completed_len,
                                        const struct zmk_runtime_combo_global_settings *global,
                                        int64_t timestamp) {
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (uint32_t i = 0; i < CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS; i++) {
        struct zmk_runtime_combo_config combo;
        if (read_enabled_combo(i, &combo) < 0 || combo.key_position_len <= completed_len) {
            continue;
        }
        if (!combo_active_on_layer(&combo, highest_active_layer) ||
            is_quick_tap(&combo, global, timestamp)) {
            continue;
        }
        if (combo_matches_pending(&combo, global, timestamp)) {
            return true;
        }
    }
    return false;
}

static int release_pending_keys(void) {
    uint8_t count = pending_count;
    pending_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (i == 0) {
            ZMK_EVENT_RELEASE(pending_keys[i]);
        } else {
            ZMK_EVENT_RAISE(pending_keys[i]);
        }
    }
    return count;
}

static void cleanup_pending(void) {
    k_work_cancel_delayable(&timeout_task);
    timeout_task_timeout_at = 0;
    fully_pressed_combo_idx = -1;
    release_pending_keys();
}

/* Fire the combo remembered as fully pressed (if any), replaying any pending keys
 * beyond its own positions as regular key events. Returns false if none was stored. */
static bool fire_fully_pressed_combo(void) {
    if (fully_pressed_combo_idx < 0) {
        return false;
    }
    uint32_t combo_idx = fully_pressed_combo_idx;
    fully_pressed_combo_idx = -1;

    struct zmk_runtime_combo_config combo;
    if (runtime_combo_cache_read(combo_idx, &combo) < 0 || combo.key_position_len > pending_count) {
        return false;
    }
    finish_pending_and_activate(combo_idx, &combo);
    return true;
}

static void update_timeout_task(void) {
    if (pending_count == 0) {
        k_work_cancel_delayable(&timeout_task);
        timeout_task_timeout_at = 0;
        return;
    }

    int64_t timeout_at = LLONG_MAX;
    struct zmk_runtime_combo_global_settings global = runtime_combo_cached_global_settings();
    for (uint32_t i = 0; i < CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS; i++) {
        struct zmk_runtime_combo_config combo;
        if (read_enabled_combo(i, &combo) < 0 ||
            !combo_matches_pending(&combo, &global, k_uptime_get())) {
            continue;
        }
        uint16_t timeout_ms = effective_timeout_ms(&combo, &global);
        timeout_at = MIN(timeout_at, pending_keys[0].data.timestamp + timeout_ms);
    }

    if (timeout_at == LLONG_MAX) {
        if (!fire_fully_pressed_combo()) {
            cleanup_pending();
        }
        return;
    }
    if (timeout_task_timeout_at != timeout_at &&
        k_work_schedule(&timeout_task, K_MSEC(MAX(timeout_at - k_uptime_get(), 0))) >= 0) {
        timeout_task_timeout_at = timeout_at;
    }
}

static int press_combo_behavior(uint32_t combo_idx, const struct zmk_runtime_combo_config *combo,
                                int64_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(ZMK_COMBOS_LEN + combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    last_combo_timestamp = timestamp;
    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static int release_combo_behavior(uint32_t combo_idx, const struct zmk_runtime_combo_config *combo,
                                  int64_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(ZMK_COMBOS_LEN + combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static struct runtime_combo_active *store_active_combo(uint32_t combo_idx, uint8_t count) {
    if (active_combo_count >= CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS) {
        LOG_WRN("Unable to store runtime combo; active combo limit reached");
        return NULL;
    }

    struct runtime_combo_active *active = &active_combos[active_combo_count++];
    *active = (struct runtime_combo_active){.combo_idx = combo_idx};
    for (uint8_t i = 0; i < count; i++) {
        active->key_positions_pressed[i] = pending_keys[i];
    }
    active->key_positions_pressed_count = count;
    return active;
}

static int activate_combo(uint32_t combo_idx, const struct zmk_runtime_combo_config *combo) {
    struct runtime_combo_active *active = store_active_combo(combo_idx, pending_count);
    if (!active) {
        cleanup_pending();
        return ZMK_EV_EVENT_CAPTURED;
    }
    pending_count = 0;
    fully_pressed_combo_idx = -1;
    k_work_cancel_delayable(&timeout_task);
    timeout_task_timeout_at = 0;
    press_combo_behavior(combo_idx, combo, active->key_positions_pressed[0].data.timestamp);
    return ZMK_EV_EVENT_CAPTURED;
}

/* Fire `combo` (whose positions are the first `combo->key_position_len` pending keys)
 * and replay any remaining pending keys as regular key events, since they were
 * captured while a longer candidate looked viable but ended up unused. */
static void finish_pending_and_activate(uint32_t combo_idx,
                                        const struct zmk_runtime_combo_config *combo) {
    uint8_t combo_len = combo->key_position_len;
    uint8_t leftover_count = pending_count - combo_len;
    struct zmk_position_state_changed_event
        leftover[CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO];
    for (uint8_t i = 0; i < leftover_count; i++) {
        leftover[i] = pending_keys[combo_len + i];
    }

    struct runtime_combo_active *active = store_active_combo(combo_idx, combo_len);
    pending_count = 0;
    k_work_cancel_delayable(&timeout_task);
    timeout_task_timeout_at = 0;

    if (active) {
        press_combo_behavior(combo_idx, combo, active->key_positions_pressed[0].data.timestamp);
    }

    for (uint8_t i = 0; i < leftover_count; i++) {
        if (i == 0) {
            ZMK_EVENT_RELEASE(leftover[i]);
        } else {
            ZMK_EVENT_RAISE(leftover[i]);
        }
    }
}

static int find_complete_combo(struct zmk_runtime_combo_config *combo,
                               const struct zmk_runtime_combo_global_settings *global) {
    for (uint32_t i = 0; i < CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS; i++) {
        if (read_enabled_combo(i, combo) < 0 || combo->key_position_len != pending_count ||
            !combo_matches_pending(combo, global, pending_keys[pending_count - 1].data.timestamp)) {
            continue;
        }
        return i;
    }
    return -ENOENT;
}

static int position_state_down(struct zmk_position_state_changed *data) {
    int candidates = count_candidates_for_position(data->position, data->timestamp);
    if (candidates == 0) {
        if (fire_fully_pressed_combo()) {
            return position_state_down(data);
        }
        if (pending_count > 0) {
            cleanup_pending();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (pending_count >= CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO) {
        cleanup_pending();
        return ZMK_EV_EVENT_BUBBLE;
    }

    pending_keys[pending_count++] = copy_raised_zmk_position_state_changed(data);

    struct zmk_runtime_combo_global_settings global = runtime_combo_cached_global_settings();
    struct zmk_runtime_combo_config combo;
    int complete_combo = find_complete_combo(&combo, &global);
    if (complete_combo >= 0) {
        if (has_longer_viable_candidate(combo.key_position_len, &global, data->timestamp)) {
            fully_pressed_combo_idx = complete_combo;
            update_timeout_task();
            return ZMK_EV_EVENT_CAPTURED;
        }
        return activate_combo(complete_combo, &combo);
    }

    update_timeout_task();
    return ZMK_EV_EVENT_CAPTURED;
}

static void deactivate_combo(uint8_t active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        active_combos[active_combo_index] = active_combos[active_combo_count];
    }
    active_combos[active_combo_count] = (struct runtime_combo_active){0};
}

static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (uint8_t active_idx = 0; active_idx < active_combo_count; active_idx++) {
        struct runtime_combo_active *active = &active_combos[active_idx];
        struct zmk_runtime_combo_config combo;
        if (runtime_combo_cache_read(active->combo_idx, &combo) < 0) {
            deactivate_combo(active_idx);
            return true;
        }

        bool key_released = false;
        bool all_keys_pressed = active->key_positions_pressed_count == combo.key_position_len;
        bool all_keys_released = true;
        for (uint8_t i = 0; i < active->key_positions_pressed_count; i++) {
            if (key_released) {
                active->key_positions_pressed[i - 1] = active->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else {
                key_released = true;
            }
        }

        if (!key_released) {
            continue;
        }

        active->key_positions_pressed_count--;
        struct zmk_runtime_combo_global_settings global = runtime_combo_cached_global_settings();
        bool slow_release = effective_slow_release(&combo, &global);
        if ((slow_release && all_keys_released) || (!slow_release && all_keys_pressed)) {
            release_combo_behavior(active->combo_idx, &combo, timestamp);
        }
        if (all_keys_released) {
            deactivate_combo(active_idx);
        }
        return true;
    }
    return false;
}

static int position_state_up(struct zmk_position_state_changed *data) {
    if (pending_count > 0 && !fire_fully_pressed_combo()) {
        cleanup_pending();
    }
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    ARG_UNUSED(item);
    if (timeout_task_timeout_at != 0 && k_uptime_get() >= timeout_task_timeout_at) {
        if (!fire_fully_pressed_combo()) {
            cleanup_pending();
        }
    }
}

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_COMBO_TEST)
static bool runtime_combo_test_setup_done;

static int runtime_combo_test_setup(void) {
    if (runtime_combo_test_setup_done) {
        return 0;
    }

    const char *key_press = "key_press";
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(key_press);
    if (behavior_id == 0 || behavior_id == UINT16_MAX) {
        LOG_ERR("Runtime combo test cannot find key_press behavior");
        return -ENODEV;
    }

    struct zmk_runtime_combo_config combo = {
        .enabled = true,
        .key_position_len = 2,
        .behavior = {.behavior_dev = key_press, .param1 = B},
        .key_positions = {0, 1},
    };

    int ret = zmk_runtime_combo_write(0, &combo, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo test setup failed: %d", ret);
        return ret;
    }

#if RUNTIME_COMBO_HAS_DEFAULTS
    /* Exercises the DT-default read-path fallback (see tests/defaults):
     * slot 1's default is left untouched, slot 2's is tombstoned, slot 3's
     * is overridden to a different behavior, and slot 4's is tombstoned then
     * restored via reset. Slot numbers and positions must match that
     * fixture's cormoran,runtime-combo-defaults node. */
    ret = zmk_runtime_combo_delete(2, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo default tombstone setup failed: %d", ret);
        return ret;
    }

    struct zmk_runtime_combo_config default_override = {
        .enabled = true,
        .key_position_len = 2,
        .behavior = {.behavior_dev = key_press, .param1 = J},
        .key_positions = {6, 7},
    };
    ret = zmk_runtime_combo_write(3, &default_override, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo default override setup failed: %d", ret);
        return ret;
    }

    ret = zmk_runtime_combo_delete(4, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo default reset setup (delete) failed: %d", ret);
        return ret;
    }
    ret = zmk_runtime_combo_reset(4);
    if (ret < 0) {
        LOG_ERR("Runtime combo default reset setup (reset) failed: %d", ret);
        return ret;
    }
#elif IS_ENABLED(CONFIG_ZMK_RUNTIME_COMBO_TEST_DISCARD)
    /* Written to memory only, then reverted outside of
     * zmk_runtime_combo_write()/reset() via the same
     * zmk_custom_settings_discard_scope() + zmk_runtime_combo_invalidate_cache()
     * pair the Save/Discard RPC handlers use, to exercise that cache
     * invalidation path directly (see tests/discard). */
    struct zmk_runtime_combo_config to_be_discarded = {
        .enabled = true,
        .key_position_len = 2,
        .behavior = {.behavior_dev = key_press, .param1 = C},
        .key_positions = {2, 3},
    };
    ret = zmk_runtime_combo_write(1, &to_be_discarded, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo discard setup (write) failed: %d", ret);
        return ret;
    }
    uint32_t discard_affected = 0;
    ret = zmk_custom_settings_discard_scope(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, NULL, NULL,
                                            &discard_affected);
    if (ret < 0) {
        LOG_ERR("Runtime combo discard setup (discard_scope) failed: %d", ret);
        return ret;
    }
    zmk_runtime_combo_invalidate_cache();
#else
    /* Slots 1-4 exercise overlap resolution and per-combo overrides (see
     * tests/overrides). Skipped when a cormoran,runtime-combo-defaults node
     * is compiled in, since that fixture (tests/defaults) reuses these slot
     * numbers to exercise the default/override/reset paths instead. */

    /* Subset of combo 2 below; used to exercise overlap resolution. */
    struct zmk_runtime_combo_config overlap_subset = {
        .enabled = true,
        .key_position_len = 2,
        .behavior = {.behavior_dev = key_press, .param1 = C},
        .key_positions = {2, 3},
    };
    ret = zmk_runtime_combo_write(1, &overlap_subset, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo overlap subset setup failed: %d", ret);
        return ret;
    }

    /* Strict superset of combo 1 above; the longer combo should always win. */
    struct zmk_runtime_combo_config overlap_superset = {
        .enabled = true,
        .key_position_len = 3,
        .behavior = {.behavior_dev = key_press, .param1 = D},
        .key_positions = {2, 3, 4},
    };
    ret = zmk_runtime_combo_write(2, &overlap_superset, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo overlap superset setup failed: %d", ret);
        return ret;
    }

    /* Requires a full second of prior idle time; used to exercise the
     * per-combo require-prior-idle override (global default is disabled). */
    struct zmk_runtime_combo_config idle_guarded = {
        .enabled = true,
        .key_position_len = 2,
        .behavior = {.behavior_dev = key_press, .param1 = E},
        .key_positions = {5, 6},
        .require_prior_idle_ms = 1000,
    };
    ret = zmk_runtime_combo_write(3, &idle_guarded, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo idle-guarded setup failed: %d", ret);
        return ret;
    }

    /* Longer timeout than the global default; used to exercise the per-combo
     * timeout override. */
    struct zmk_runtime_combo_config long_timeout = {
        .enabled = true,
        .key_position_len = 2,
        .behavior = {.behavior_dev = key_press, .param1 = F},
        .key_positions = {7, 8},
        .timeout_ms = 1000,
    };
    ret = zmk_runtime_combo_write(4, &long_timeout, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo long-timeout setup failed: %d", ret);
        return ret;
    }
#endif

    runtime_combo_test_setup_done = true;
    LOG_INF("PASS: runtime_combo_test_setup");
    return 0;
}
#endif

static int position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(eh);
    if (!data) {
        return ZMK_EV_EVENT_BUBBLE;
    }
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_COMBO_TEST)
    runtime_combo_test_setup();
#endif
    /* First real position event is guaranteed to run after main() calls
     * settings_load(), unlike any SYS_INIT hook. */
    runtime_combo_cache_ensure_ready();
    return data->state ? position_state_down(data) : position_state_up(data);
}

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev && ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int runtime_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    }
    if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(runtime_combo, runtime_combo_listener);
ZMK_SUBSCRIPTION(runtime_combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(runtime_combo, zmk_keycode_state_changed);

static int runtime_combo_init(void) {
    k_work_init_delayable(&timeout_task, combo_timeout_handler);
    return 0;
}

SYS_INIT(runtime_combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
