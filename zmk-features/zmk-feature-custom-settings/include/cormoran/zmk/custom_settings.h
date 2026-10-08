/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>

#define ZMK_CUSTOM_SETTING_SOURCE_LOCAL 0U
#define ZMK_CUSTOM_SETTING_SOURCE_ALL UINT32_MAX
#define ZMK_CUSTOM_SETTING_ARRAY_NONE UINT32_MAX

/* Supported scalar value types for custom settings. */
enum zmk_custom_setting_value_type {
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES = 1,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 = 2,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL = 3,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING = 4,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR = 5,
};

/* A ZMK behavior binding: a behavior local ID (see CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
 * plus its two binding parameters. Writes are validated against the target
 * behavior's parameter metadata (see zmk_behavior_validate_binding in
 * <drivers/behavior.h>) when CONFIG_ZMK_BEHAVIOR_LOCAL_IDS is enabled. */
struct zmk_custom_setting_behavior_value {
    uint32_t behavior_id;
    uint32_t param1;
    uint32_t param2;
};

/* Write mode controls whether a new value is saved, staged in RAM, or temporary. */
enum zmk_custom_setting_write_mode {
    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,
    ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST,
    ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY,
};

/* Controls whether RPC may expose the setting value. */
enum zmk_custom_setting_confidentiality {
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE,
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
};

/* Secure settings require Studio unlock before RPC read/write access. */
enum zmk_custom_setting_permission {
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
};

enum zmk_custom_setting_constraint_type {
    ZMK_CUSTOM_SETTING_CONSTRAINT_NONE,
    ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
    ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS,
    ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE,
    ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID,
    ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID,
};

enum zmk_custom_setting_changed_kind {
    ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
    ZMK_CUSTOM_SETTING_CHANGED_SAVED,
    ZMK_CUSTOM_SETTING_CHANGED_DISCARDED,
    ZMK_CUSTOM_SETTING_CHANGED_RESET,
};

struct zmk_custom_setting_value {
    enum zmk_custom_setting_value_type type;
    size_t size;
    union {
        uint8_t bytes_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
        int32_t int32_value;
        bool bool_value;
        char string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
        struct zmk_custom_setting_behavior_value behavior_value;
    };
};

struct zmk_custom_setting;

/*
 * A statically-allocated shared budget that multiple BYTES/STRING large-value
 * settings (see ZMK_CUSTOM_SETTING_DEFINE_POOLED) draw regions from, instead
 * of each paying for its own worst-case max_size buffer.
 * ZMK_CUSTOM_SETTING_DEFINE_SIZED is sugar for a private, single-member pool,
 * so this is the module's only large-value backing store. A setting's region
 * can be moved within `data` by any write that needs more room than it
 * currently has (see pool_ensure_region in custom_settings.c); that is safe
 * because every access site re-reads state->blob.data under
 * custom_settings_lock instead of caching it across the lock.
 *
 * `members` is the head of an intrusive singly-linked list of every setting
 * currently holding a region of this pool (blob.pool == this and
 * state->blob.data != NULL). The list links DESCRIPTOR pointers, while the
 * link field itself (`_pool_next`) lives on each setting's RAM state block,
 * because the const flash-resident descriptor cannot hold a runtime-mutable
 * field. Pool code needs the descriptor anyway (for value_type and
 * blob.max_size), so it stores `const struct zmk_custom_setting *` and reaches
 * the link via `member->state->_pool_next`. Walking this list rather than the
 * global registry lets a pool member (e.g. a keyspace slot) live outside
 * ZMK_CUSTOM_SETTING_FOREACH. Internal to custom_settings.c.
 */
struct zmk_custom_setting_large_pool {
    uint8_t *data;
    size_t size;
    const struct zmk_custom_setting *members;
};

/* Statically define a shared pool of _pool_size bytes. Heap-free: declares
 * the backing array and the pool descriptor (named _name) as file-scope
 * statics. Pass _name (not &_name) as ZMK_CUSTOM_SETTING_DEFINE_POOLED's
 * _pool argument; that macro takes the address itself. */
#define ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(_name, _pool_size)                                    \
    BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES),                              \
                 "enable CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES to use "                          \
                 "ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE");                                          \
    static uint8_t _name##_pool_data[_pool_size];                                                  \
    static struct zmk_custom_setting_large_pool _name = {                                          \
        .data = _name##_pool_data,                                                                 \
        .size = (_pool_size),                                                                      \
    }

/* Sum of the current payload sizes (incl. STRING NUL bytes) of every setting
 * currently occupying a region of `pool` - useful for UI/diagnostics (e.g.
 * showing remaining budget). Runs under settings_lock. */
size_t zmk_custom_setting_large_pool_used(const struct zmk_custom_setting_large_pool *pool);

typedef int (*zmk_custom_setting_rpc_bytes_converter_t)(const struct zmk_custom_setting *setting,
                                                        const uint8_t *src, size_t src_size,
                                                        uint8_t *dest, size_t *dest_size,
                                                        size_t dest_capacity);

struct zmk_custom_setting_range {
    struct zmk_custom_setting_value min;
    struct zmk_custom_setting_value max;
};

struct zmk_custom_setting_options {
    const struct zmk_custom_setting_value *values;
    const char *const *labels;
    size_t count;
};

struct zmk_custom_setting_hid_usage {
    uint32_t usage_page;
    uint32_t usage_min;
    uint32_t usage_max;
};

struct zmk_custom_setting_constraint {
    enum zmk_custom_setting_constraint_type type;
    union {
        struct zmk_custom_setting_range range;
        struct zmk_custom_setting_options options;
        struct zmk_custom_setting_hid_usage hid_usage;
    };
};

/*
 * Mutable, per-array state for one array setting: a single contiguous backing
 * buffer plus per-slot bookkeeping, sized once by max_count. Allocated
 * statically by ZMK_CUSTOM_SETTING_ARRAY_DEFINE (one instance per array) and
 * referenced by `array_state` from both the array's single registered
 * descriptor and any short-lived "index view" struct zmk_custom_setting
 * instances that zmk_custom_setting_find_array_element hands out (see
 * array_view_pool in custom_settings.c). All access happens under
 * custom_settings_lock.
 */
struct zmk_custom_setting_array_state {
    struct zmk_custom_setting_value *values;
    /* True if `values[i]`/`dirty[i]`/`has_persistent[i]` has ever been
     * written to (memory or persisted) since the last reset - lets save/
     * discard/reset operate in O(active count) without walking a registry. */
    bool *dirty;
    bool *has_persistent;
    uint32_t max_size;
    uint32_t default_size;
    uint32_t size;
    uint32_t persistent_size;
    const struct zmk_custom_setting_value *defaults;
};

/*
 * Per-setting mutable RAM state, split out of the const, flash-resident
 * struct zmk_custom_setting descriptor below. One instance per setting,
 * emitted by the same registration macro that emits the descriptor (or
 * embedded in the keyspace slot / array-view pool entry for runtime-built
 * descriptor instances). ~16-24 bytes on ARM32.
 *
 * All fields are internal to custom_settings.c and are only accessed under
 * custom_settings_lock; callers never touch a state block directly.
 */
struct zmk_custom_setting_state {
    /* ZMK_CUSTOM_SETTING_STATE_* bits below. Meaningful only for non-array
     * settings; array settings keep the equivalent per-element state in
     * array_state (has_persistent[i]/dirty[i]/values[i]) since a single
     * struct zmk_custom_setting does not represent one element. An array
     * index view still uses the TEMPORARY_ACTIVE bit (temp overrides live
     * on the view, keyed by (array_state, array_index) - see
     * array_view_pool in custom_settings.c). */
    uint8_t flags;
    /* Index into the shared temporary-override pool (see custom_settings.c),
     * or -1 when the TEMPORARY_ACTIVE flag is clear. Avoids a full-size
     * struct zmk_custom_setting_value slot per setting for a mode that is
     * rarely used and never persisted. */
    int8_t temp_slot;
    /* The effective in-memory value, right-sized per value_type:
     * - INT32/BOOL/BEHAVIOR: stored inline (<= 12 bytes);
     * - BYTES/STRING: always behind `blob.data` - a region of the
     *   descriptor's blob.pool (NULL until the first non-empty write, can
     *   move on any pool compaction; never cached across custom_settings_lock),
     *   or the exact-size static store buffer the plain
     *   ZMK_CUSTOM_SETTING_DEFINE macro emits (`<name>_store[capacity + 1]`,
     *   pointed at permanently). `blob.size` is the current payload length
     *   (excluding the STRING NUL stored in the buffer right behind it).
     * - Array settings do not use this union at all (element values live in
     *   array_state->values[]). */
    union {
        int32_t int32_value;
        bool bool_value;
        struct zmk_custom_setting_behavior_value behavior;
        struct {
            uint8_t *data;
            uint16_t size;
        } blob;
    };
    /* Runtime-installed default from zmk_custom_setting_set_default() (which
     * cannot write the const descriptor's default_value pointer). Checked
     * before the descriptor's default_value everywhere a default is read.
     * NULL unless set_default was called. */
    const struct zmk_custom_setting_value *default_override;
    /* Intrusive singly-linked list pointer for the descriptor's blob.pool
     * member list (see the `members` doc on struct
     * zmk_custom_setting_large_pool for why the list nodes are descriptor
     * pointers while the link lives here in state). Pool-internal: NULL
     * whenever blob.data is NULL, and only touched by pool_ensure_region /
     * zmk_custom_setting_large_pool_used and by
     * zmk_custom_setting_keyspace_delete (which must unlink a setting before
     * its storage is reused by a future slot). */
    const struct zmk_custom_setting *_pool_next;
};

/* struct zmk_custom_setting_state.flags bits. */
#define ZMK_CUSTOM_SETTING_STATE_INITIALIZED BIT(0)
#define ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT BIT(1)
/* Set on any memory-mode write, cleared on save/discard/reset, so
 * zmk_custom_setting_has_unsaved_value stays a cheap flag check even though
 * it runs on the Studio RPC list hot path. */
#define ZMK_CUSTOM_SETTING_STATE_DIRTY BIT(2)
#define ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE BIT(3)

/*
 * The setting descriptor, and the public handle type - every API takes
 * `const struct zmk_custom_setting *`. Compile-time registrations (every
 * ZMK_CUSTOM_SETTING_*DEFINE macro) emit this as a `const` object placed in
 * flash/rodata (ITERABLE_SECTION_ROM in the module's linker include), with
 * all runtime-mutable state behind the `state` pointer.
 *
 * Two descriptor populations are NOT flash-resident and share this struct
 * type as plain mutable RAM instances (their identity fields are only known
 * at runtime): keyspace slot descriptors (struct
 * zmk_custom_setting_keyspace_slot.setting) and array index views
 * (array_view_pool in custom_settings.c). Both embed their state block next
 * to the descriptor instead of pointing at a macro-emitted one.
 */
struct zmk_custom_setting {
    const char *custom_subsystem_id;
    const char *key;
    const char *array_key;
    /* ZMK_CUSTOM_SETTING_ARRAY_NONE for scalars. For the one registered
     * array descriptor, also ZMK_CUSTOM_SETTING_ARRAY_NONE (the descriptor
     * itself does not represent one element). For an index view handed out
     * by zmk_custom_setting_find_array_element, the requested element
     * index. */
    uint32_t array_index;
    /* Enums packed to uint8_t - all their values fit comfortably. */
    uint8_t value_type;                                /* enum zmk_custom_setting_value_type */
    uint8_t confidentiality;                           /* enum zmk_custom_setting_confidentiality */
    uint8_t read_permission : 4, write_permission : 4; /* enum zmk_custom_setting_permission */
    uint8_t constraints_count;
    const struct zmk_custom_setting_constraint *constraints;
    /* Points at a static const object generated by the registration macros,
     * so the default lives in flash. Unused (NULL) for the array descriptor
     * and its views; array element defaults live in array_state->defaults
     * instead, since each element has its own default. Because the descriptor
     * is const, zmk_custom_setting_set_default() installs a runtime override
     * in state->default_override, which is consulted first wherever a default
     * is read. */
    const struct zmk_custom_setting_value *default_value;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;

    /* Kind-specific, mutually exclusive descriptor data. Which arm is
     * meaningful follows from fields outside the union: `array_state` for an
     * array setting (array_key != NULL, i.e. zmk_custom_setting_is_array()),
     * `blob` for a non-array BYTES/STRING setting. Scalar INT32/BOOL/BEHAVIOR
     * settings use neither (zero-initialized). */
    union {
        /* BYTES/STRING: every such value lives behind state->blob.data.
         *
         * `max_size` is the value's byte capacity: the fixed
         * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE for a plain
         * ZMK_CUSTOM_SETTING_DEFINE (backed by the macro-emitted exact-size
         * store buffer), or the explicit per-setting max_size of
         * ZMK_CUSTOM_SETTING_DEFINE_SIZED/_POOLED (up to
         * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE).
         *
         * `pool` is NULL for a fixed-store setting (state->blob.data points
         * permanently at the macro-emitted buffer, which never moves), or
         * the shared budget the value's region is carved from on demand
         * (ZMK_CUSTOM_SETTING_DEFINE_POOLED against a caller-shared
         * ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE, ZMK_CUSTOM_SETTING_DEFINE_SIZED
         * against its private single-member pool, or a keyspace's slot
         * pool). This `pool == NULL` test is the single fork between the
         * two backing stores; everything else in the read/write path is
         * shared. See struct zmk_custom_setting_large_pool for the
         * compaction/locking rules (nothing caches state->blob.data across
         * custom_settings_lock). A value larger than
         * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE is reachable via
         * zmk_custom_setting_read_into / zmk_custom_setting_write_bytes,
         * the WriteValueChunk RPC (writes only - reads stream through the
         * ordinary GetSetting/ListSettings RPC response), and
         * zmk_custom_setting_with_large_raw_bytes; the fixed-carrier
         * zmk_custom_setting_read/write report -EMSGSIZE once the value
         * exceeds the carrier. */
        struct {
            uint32_t max_size;
            struct zmk_custom_setting_large_pool *pool;
        } blob;
        /* Non-NULL for both the array's descriptor and any index view of it.
         * Points straight at the shared mutable array_state (rather than at
         * the owning struct zmk_custom_setting, which would add an
         * indirection), since that is all effective_value() /
         * write_value_locked() / etc. need to read or write element
         * `array_index`. */
        struct zmk_custom_setting_array_state *array_state;
    };

    /*
     * Non-NULL only for a live keyspace slot (see
     * ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE further down) - points at the owning
     * keyspace. NULL for every compile-time (STRUCT_SECTION_ITERABLE) setting
     * and array element view.
     *
     * A keyspace slot's OWN value_type/constraints/blob.pool always describe
     * the opaque `[user_key\0][payload]` BLOB it stores (value_type is always
     * ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES internally, regardless of the
     * keyspace's declared value_type). Use zmk_custom_setting_keyspace_of() to
     * get the owning keyspace and read *its* value_type/constraints/max_size
     * to interpret the slot's PAYLOAD; the generic read/write/save/pool code
     * never needs to know the difference and treats a slot as a plain pooled
     * BYTES setting.
     */
    const struct zmk_custom_setting_keyspace *_keyspace;

    /* The setting's RAM state block (macro-emitted for compile-time
     * registrations, embedded for keyspace slots / array views). Never NULL
     * on a registered/bound descriptor. */
    struct zmk_custom_setting_state *state;
};

struct zmk_custom_setting_changed {
    const struct zmk_custom_setting *setting;
    enum zmk_custom_setting_changed_kind kind;
    uint32_t source;
};

ZMK_EVENT_DECLARE(zmk_custom_setting_changed);

/*
 * Raised exactly once per boot, after the settings subsystem has finished
 * loading persisted values into every registered setting - i.e. once every
 * setting holds its effective value (persisted value, or default when nothing
 * was persisted) and is safe to read. Subscribe to this if a consumer needs to
 * apply loaded setting values to hardware at startup: reading a setting from a
 * plain SYS_INIT is racy because it may run before settings_load() has
 * populated the persisted value, whereas this event fires strictly after that
 * load completes.
 *
 * Fires from the settings-subtree commit that concludes the boot settings_load
 * pass; the later, targeted settings_load_subtree() reloads that
 * zmk_custom_setting_discard performs do NOT re-raise it. The event carries no
 * payload today. Note that in a build without CONFIG_SETTINGS (so nothing ever
 * calls settings_load()) this event never fires.
 */
struct zmk_custom_settings_initialized {
    /* Reserved for future use; present because a ZMK event requires a data
     * struct. */
    uint8_t reserved;
};

ZMK_EVENT_DECLARE(zmk_custom_settings_initialized);

#define ZMK_CUSTOM_SETTING_VALUE_INT32(_value)                                                     \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                \
                                       .int32_value = (_value)})

#define ZMK_CUSTOM_SETTING_VALUE_BOOL(_value)                                                      \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,                 \
                                       .bool_value = (_value)})

#define ZMK_CUSTOM_SETTING_VALUE_STRING(_value)                                                    \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,               \
                                       .size = sizeof(_value) - 1,                                 \
                                       .string_value = (_value)})

#define ZMK_CUSTOM_SETTING_VALUE_BYTES(...)                                                        \
    ((struct zmk_custom_setting_value){                                                            \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,                                               \
        .size = sizeof((uint8_t[]){__VA_ARGS__}),                                                  \
        .bytes_value = {__VA_ARGS__},                                                              \
    })

#define ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(_behavior_id, _param1, _param2)                          \
    ((struct zmk_custom_setting_value){                                                            \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,                                            \
        .behavior_value = {.behavior_id = (_behavior_id),                                          \
                           .param1 = (_param1),                                                    \
                           .param2 = (_param2)},                                                   \
    })

#define ZMK_CUSTOM_SETTINGS_STRINGIFY(x) ZMK_CUSTOM_SETTINGS_STRINGIFY_INNER(x)
#define ZMK_CUSTOM_SETTINGS_STRINGIFY_INNER(x) #x

#define ZMK_CUSTOM_SETTING_NO_CONSTRAINT                                                           \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_NONE})

/* Field initializers (not compound literals) so this expands to a constant
 * expression usable in a static initializer even under strict -std=c11; GCC
 * rejects an outer compound literal that nests another compound literal. */
#define ZMK_CUSTOM_SETTING_RANGE_INT32(_min, _max)                                                 \
    ((struct zmk_custom_setting_constraint){                                                       \
        .type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,                                               \
        .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_min)},     \
                  .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_max)}}})

#define ZMK_CUSTOM_SETTING_HID_USAGE(_usage_page, _usage_min, _usage_max)                          \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE,       \
                                            .hid_usage = {.usage_page = (_usage_page),             \
                                                          .usage_min = (_usage_min),               \
                                                          .usage_max = (_usage_max)}})

#define ZMK_CUSTOM_SETTING_LAYER_ID                                                                \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID})

#define ZMK_CUSTOM_SETTING_BEHAVIOR_ID                                                             \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID})

/* True (compile-time constant) if _value_type stores its payload behind
 * state->blob.data (see struct zmk_custom_setting_state) instead of inline
 * in the state union. */
#define ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(_value_type)                                               \
    ((_value_type) == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||                                       \
     (_value_type) == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING)

/* Size of the exact-size static value store buffer a (non-pooled)
 * registration macro emits for _value_type: capacity + 1 (room for the
 * STRING NUL; also reserved for BYTES to keep this simple - one byte) for
 * BYTES/STRING, 0 for every scalar type (whose value lives inline in the
 * state union - a BOOL setting emits no value buffer at all; the zero-length
 * array costs zero bytes). */
#define ZMK_CUSTOM_SETTING_VALUE_STORE_SIZE(_value_type, _capacity)                                \
    (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(_value_type) ? (_capacity) + 1 : 0)

/* Register one custom setting in the iterable setting registry. */
#define ZMK_CUSTOM_SETTING_DEFINE(_name, _custom_subsystem_id, _key, _value_type, _default_value,  \
                                  _confidentiality, _read_permission, _write_permission,           \
                                  _constraint)                                                     \
    ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(_name, _custom_subsystem_id, _key, _value_type,     \
                                               _default_value, _confidentiality, _read_permission, \
                                               _write_permission, _constraint)

/*
 * Register one BYTES/STRING setting able to store a value up to _max_size
 * bytes - larger than the fixed CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE
 * carrier. _max_size must not exceed
 * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE. This is sugar for a
 * private, single-member ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE sized for
 * exactly this setting plus ZMK_CUSTOM_SETTING_DEFINE_POOLED against it - the
 * pool is this module's only large-value backing store, so "sized" and
 * "pooled" are the same mechanism at different sharing granularities. A
 * one-member pool never moves (compaction is a no-op), so this behaves like a
 * dedicated buffer while costing one small pool descriptor extra. A value
 * larger than CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE can be read through
 * the ordinary GetSetting/ListSettings Studio RPC (streamed, any size),
 * written through the chunked WriteValueChunk Studio RPC, and read/written
 * through the zmk_custom_setting_read_into / zmk_custom_setting_write_bytes
 * firmware API; the fixed-carrier zmk_custom_setting_read/write report
 * -EMSGSIZE once the value exceeds the carrier.
 */
#define ZMK_CUSTOM_SETTING_DEFINE_SIZED(_name, _max_size, _custom_subsystem_id, _key, _value_type, \
                                        _default_value, _confidentiality, _read_permission,        \
                                        _write_permission, _constraint)                            \
    ZMK_CUSTOM_SETTING_DEFINE_SIZED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                           \
        _name, _max_size, _custom_subsystem_id, _key, _value_type, _default_value,                 \
        _confidentiality, _read_permission, _write_permission, NULL, NULL, _constraint)

/* Register one custom setting with custom bytes RPC conversion hooks. */
#define ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS(                                             \
    _name, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,              \
    _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, _constraint)          \
    BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS),                            \
                 "enable CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS to use "                        \
                 "ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS");                                 \
    ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                                 \
        _name, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,          \
        _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, _constraint)

/* Register one custom setting with zero or more constraints. */
#define ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(_name, _custom_subsystem_id, _key, _value_type, \
                                                   _default_value, _confidentiality,               \
                                                   _read_permission, _write_permission, ...)       \
    ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                                 \
        _name, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,          \
        _read_permission, _write_permission, NULL, NULL, __VA_ARGS__)

/*
 * Innermost plain registration macro (every ZMK_CUSTOM_SETTING_DEFINE* variant
 * that does not take an explicit max_size funnels through here). Emits a
 * const, flash-resident descriptor plus its RAM state block. For a BYTES/STRING
 * setting, an exact-size static store buffer (`_name##_store[capacity + 1]`,
 * capacity = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) is emitted and pointed
 * at permanently by the state block; scalar settings emit no value buffer at
 * all (their value lives inline in the state union). Compare
 * ZMK_CUSTOM_SETTING_DEFINE_POOLED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS below,
 * the equivalent innermost macro for a setting whose value is carved from a
 * shared pool instead.
 */
#define ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                             \
    _name, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,              \
    _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, ...)                  \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT(sizeof(_key) <= CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                           \
                 "Custom setting key is too long");                                                \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    static const struct zmk_custom_setting_value _name##_default = _default_value;                 \
    static uint8_t _name##_store[ZMK_CUSTOM_SETTING_VALUE_STORE_SIZE(                              \
        _value_type, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE)] __unused;                         \
    static struct zmk_custom_setting_state _name##_state = {                                       \
        .temp_slot = -1,                                                                           \
        .blob.data = ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(_value_type) ? _name##_store : NULL,          \
    };                                                                                             \
    const STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                   \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key = _key,                                                                               \
        .array_key = NULL,                                                                         \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .value_type = _value_type,                                                                 \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = &_name##_default,                                                         \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
        .blob = {.max_size = ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(_value_type)                          \
                                 ? CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE                       \
                                 : 0,                                                              \
                 .pool = NULL},                                                                    \
        .state = &_name##_state,                                                                   \
    }

/*
 * DEFINE_SIZED's full (converters + variadic constraints) innermost macro:
 * sugar over a private pool (see the large-value-pool doc on struct
 * zmk_custom_setting_large_pool). The private pool is sized _max_size + 1 so
 * a full-length STRING - whose trailing NUL is stored inside the region, see
 * large_store_set_raw in custom_settings.c - still fits, matching what a
 * dedicated buffer would have reserved. Emitting a (tiny) private pool even
 * when _max_size does not exceed the fixed carrier is harmless (a one-member
 * pool that never moves) and keeps this macro simple; use plain
 * ZMK_CUSTOM_SETTING_DEFINE when no large-value store is wanted at all.
 */
#define ZMK_CUSTOM_SETTING_DEFINE_SIZED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                       \
    _name, _max_size, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,   \
    _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, ...)                  \
    ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(_name##_auto_pool, (_max_size) + 1);                      \
    ZMK_CUSTOM_SETTING_DEFINE_POOLED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                          \
        _name, _max_size, _name##_auto_pool, _custom_subsystem_id, _key, _value_type,              \
        _default_value, _confidentiality, _read_permission, _write_permission, _rpc_serializer,    \
        _rpc_deserializer, __VA_ARGS__)

/*
 * Register one BYTES/STRING setting whose payload lives in a shared
 * ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE budget instead of a private one
 * (compare ZMK_CUSTOM_SETTING_DEFINE_SIZED). Prefer this over DEFINE_SIZED
 * when many large settings (e.g. N macro bodies) share one RAM budget and are
 * not all expected to hold their maximum value simultaneously; prefer
 * DEFINE_SIZED for one standalone large setting that should always have its
 * own guaranteed capacity.
 *
 * _pool is the pool object itself (as declared by
 * ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE), not a pointer - this macro takes its
 * address. _max_size must not exceed CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE
 * (same ceiling as DEFINE_SIZED), but is NOT checked against _pool's size at
 * compile time: the pool object's `size` is a runtime field by the time this
 * macro can see it (not a constant expression available at this expansion),
 * so an over-committed pool (sum of every member's _max_size > pool size) is
 * fine by design - see pool_ensure_region in custom_settings.c - and is only
 * an actual problem once enough members are simultaneously non-empty to
 * exceed the pool's real size, at which point the write that would exceed it
 * fails with -ENOSPC and leaves the setting's previous value untouched. A
 * persisted value that no longer fits on load (e.g. after shrinking the pool
 * across a firmware update) is skipped with a LOG_WRN instead of failing
 * boot, mirroring the existing keyspace-pool-exhaustion policy.
 */
#define ZMK_CUSTOM_SETTING_DEFINE_POOLED(_name, _max_size, _pool, _custom_subsystem_id, _key,      \
                                         _value_type, _default_value, _confidentiality,            \
                                         _read_permission, _write_permission, _constraint)         \
    ZMK_CUSTOM_SETTING_DEFINE_POOLED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                          \
        _name, _max_size, _pool, _custom_subsystem_id, _key, _value_type, _default_value,          \
        _confidentiality, _read_permission, _write_permission, NULL, NULL, _constraint)

/*
 * Innermost pooled registration macro: both ZMK_CUSTOM_SETTING_DEFINE_POOLED
 * and ZMK_CUSTOM_SETTING_DEFINE_SIZED (against its own private pool) funnel
 * through here, so this is the only place a `struct zmk_custom_setting` with
 * `blob.pool` set is built. No fixed store buffer is emitted: the value's
 * region is carved from the pool on demand (state->blob.data starts NULL -
 * an empty/default pooled value costs zero pool bytes).
 */
#define ZMK_CUSTOM_SETTING_DEFINE_POOLED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                      \
    _name, _max_size, _pool, _custom_subsystem_id, _key, _value_type, _default_value,              \
    _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer,     \
    ...)                                                                                           \
    BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES),                              \
                 "enable CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES to use "                          \
                 "ZMK_CUSTOM_SETTING_DEFINE_SIZED/_POOLED");                                       \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT(sizeof(_key) <= CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                           \
                 "Custom setting key is too long");                                                \
    BUILD_ASSERT((_max_size) <= CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,                   \
                 "Custom setting max_size exceeds "                                                \
                 "CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE");                               \
    BUILD_ASSERT((_value_type) == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||                           \
                     (_value_type) == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,                        \
                 "ZMK_CUSTOM_SETTING_DEFINE_POOLED only supports BYTES/STRING value types");       \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    static const struct zmk_custom_setting_value _name##_default = _default_value;                 \
    static struct zmk_custom_setting_state _name##_state = {                                       \
        .temp_slot = -1,                                                                           \
    };                                                                                             \
    const STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                   \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key = _key,                                                                               \
        .array_key = NULL,                                                                         \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .value_type = _value_type,                                                                 \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = &_name##_default,                                                         \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
        .blob = {.max_size = (_max_size), .pool = &(_pool)},                                       \
        .state = &_name##_state,                                                                   \
    }

/*
 * ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE (which registered one setting per
 * array element) has been removed. Register the whole array at once with
 * ZMK_CUSTOM_SETTING_ARRAY_DEFINE (below) instead - one descriptor owns a
 * single contiguous backing buffer for all elements.
 */

/* Register an array setting: one descriptor owning a single contiguous
 * backing buffer for up to _max_count elements. _defaults must be a pointer
 * to a separately-declared `static const struct zmk_custom_setting_value[]`
 * of length _max_count (see ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE) -
 * a pointer, not a compound literal, because nesting a compound literal here
 * breaks strict -std=c11 constant-initializer rules. _default_size is the
 * active length new/reset arrays start with (<= _max_count). */
#define ZMK_CUSTOM_SETTING_ARRAY_DEFINE(_name, _custom_subsystem_id, _key, _value_type,            \
                                        _max_count, _default_size, _defaults, _confidentiality,    \
                                        _read_permission, _write_permission, _constraint)          \
    ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_CONSTRAINTS(                                              \
        _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,      \
        _confidentiality, _read_permission, _write_permission, _constraint)

/* Register an array setting with custom bytes RPC conversion hooks. */
#define ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS(                                       \
    _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,          \
    _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer,     \
    _constraint)                                                                                   \
    BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS),                            \
                 "enable CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS to use "                        \
                 "ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS");                           \
    ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                           \
        _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,      \
        _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, \
        _constraint)

#define ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_CONSTRAINTS(                                          \
    _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,          \
    _confidentiality, _read_permission, _write_permission, ...)                                    \
    ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                           \
        _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,      \
        _confidentiality, _read_permission, _write_permission, NULL, NULL, __VA_ARGS__)

#define ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                       \
    _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,          \
    _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer,     \
    ...)                                                                                           \
    BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY),                                     \
                 "enable CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY to use "                                 \
                 "ZMK_CUSTOM_SETTING_ARRAY_DEFINE");                                               \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT(sizeof(_key) <= CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                           \
                 "Custom setting array key is too long");                                          \
    BUILD_ASSERT((_default_size) <= (_max_count),                                                  \
                 "Custom setting array default size must not exceed max count");                   \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    static struct zmk_custom_setting_value _name##_values[_max_count];                             \
    static bool _name##_dirty[_max_count];                                                         \
    static bool _name##_has_persistent[_max_count];                                                \
    static struct zmk_custom_setting_array_state _name##_array_state = {                           \
        .values = _name##_values,                                                                  \
        .dirty = _name##_dirty,                                                                    \
        .has_persistent = _name##_has_persistent,                                                  \
        .max_size = (_max_count),                                                                  \
        .default_size = (_default_size),                                                           \
        .defaults = (_defaults),                                                                   \
    };                                                                                             \
    static struct zmk_custom_setting_state _name##_state = {                                       \
        .temp_slot = -1,                                                                           \
    };                                                                                             \
    const STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                   \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key = _key,                                                                               \
        .array_key = _key,                                                                         \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .array_state = &_name##_array_state,                                                       \
        .value_type = _value_type,                                                                 \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = NULL,                                                                     \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
        .state = &_name##_state,                                                                   \
    }

/* Declares a `static const struct zmk_custom_setting_value[]` of INT32
 * defaults suitable for ZMK_CUSTOM_SETTING_ARRAY_DEFINE's _defaults
 * argument. Field-initializer syntax (not the ZMK_CUSTOM_SETTING_VALUE_INT32
 * compound literal) is used for the same reason as
 * ZMK_CUSTOM_SETTING_RANGE_INT32: nesting a compound literal inside this
 * array's own initializer breaks strict -std=c11 constant-initializer
 * rules. */
#define ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(_name, ...)                                  \
    static const struct zmk_custom_setting_value _name[] = {                                       \
        ZMK_CUSTOM_SETTINGS_INT32_LIST(__VA_ARGS__)}

#define ZMK_CUSTOM_SETTINGS_INT32_LIST(...)                                                        \
    FOR_EACH(ZMK_CUSTOM_SETTINGS_INT32_LIST_ITEM, (, ), __VA_ARGS__)
#define ZMK_CUSTOM_SETTINGS_INT32_LIST_ITEM(_value)                                                \
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_value)}

/*
 * Iterate every compile-time registered setting (STRUCT_SECTION_ITERABLE, via
 * ZMK_CUSTOM_SETTING_DEFINE/ZMK_CUSTOM_SETTING_ARRAY_DEFINE) - a plain linker
 * section walk.
 *
 * Keyspace slots (see ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE further down) are
 * NOT compile-time settings and are therefore NOT visited by this macro -
 * they live in their own keyspace's `slots[]` array and are reached by
 * iterating keyspaces (ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH) instead. The
 * handful of call sites that need to see both (find, list/get RPC handlers,
 * save/discard/reset scope application) do so explicitly.
 */
#define ZMK_CUSTOM_SETTING_FOREACH(_var) STRUCT_SECTION_FOREACH(zmk_custom_setting, _var)

const struct zmk_custom_setting *zmk_custom_setting_find(const char *custom_subsystem_id,
                                                         const char *key);
/* Find any element of an array setting by its public base key. */
const struct zmk_custom_setting *zmk_custom_setting_find_array(const char *custom_subsystem_id,
                                                               const char *key);
/* Find one element of an array setting by its public base key and element index. */
const struct zmk_custom_setting *
zmk_custom_setting_find_array_element(const char *custom_subsystem_id, const char *key,
                                      uint32_t index);
/* Return the public key exposed by RPC. Array elements return their base key. */
const char *zmk_custom_setting_public_key(const struct zmk_custom_setting *setting);
/* static inline (not out-of-line) so it folds to a compile-time constant
 * `false` when CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY is off: every
 * `if (zmk_custom_setting_is_array(setting)) { ...array-only code... }` branch
 * then becomes provably dead code, so the array-only functions it calls
 * (defined in src/custom_settings_array.c, not compiled when the feature is
 * off) are dropped by the linker without an #ifdef at each call site. */
static inline bool zmk_custom_setting_is_array(const struct zmk_custom_setting *setting) {
    return IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) && setting && setting->array_key != NULL;
}
/* Return the active array length for an array element. */
uint32_t zmk_custom_setting_array_size(const struct zmk_custom_setting *setting);
/* Return the maximum array length allocated by the registry. */
uint32_t zmk_custom_setting_array_max_size(const struct zmk_custom_setting *setting);
/* Match a setting against optional subsystem, exact key, and key-prefix filters. */
bool zmk_custom_setting_matches(const struct zmk_custom_setting *setting,
                                const char *custom_subsystem_id, const char *key,
                                const char *key_prefix);

/* Read the effective value. A temporary override takes precedence over memory value. */
int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                            struct zmk_custom_setting_value *value);
/* Read the value the setting currently falls back to when it has no persisted
 * record: the runtime override installed by zmk_custom_setting_set_default() if
 * any, otherwise the compile-time default. A default is always small enough to
 * fit the fixed carrier. Returns -ENOENT when the setting has no default (e.g. a
 * keyspace slot, whose blob always carries a user key). */
int zmk_custom_setting_read_default(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_value *value);
/* True when a non-array, non-keyspace setting's current in-memory value equals
 * the value it would fall back to with no persisted record (see
 * zmk_custom_setting_read_default). False when they differ or the setting has no
 * default. */
bool zmk_custom_setting_matches_default(const struct zmk_custom_setting *setting);
int zmk_custom_setting_read_by_key(const char *custom_subsystem_id, const char *key,
                                   struct zmk_custom_setting_value *value);
/* Read one array setting element by its public base key and element index. */
int zmk_custom_setting_read_array_by_key(const char *custom_subsystem_id, const char *key,
                                         uint32_t index, struct zmk_custom_setting_value *value);
/* Convert an internal bytes value into its RPC bytes representation. */
int zmk_custom_setting_serialize_rpc_value(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *internal_value,
                                           struct zmk_custom_setting_value *rpc_value);
/* Convert an RPC bytes value into its internal firmware representation. */
int zmk_custom_setting_deserialize_rpc_value(const struct zmk_custom_setting *setting,
                                             const struct zmk_custom_setting_value *rpc_value,
                                             struct zmk_custom_setting_value *internal_value);

/* Write a value using the selected memory, persist, or temporary mode. */
int zmk_custom_setting_write(const struct zmk_custom_setting *setting,
                             const struct zmk_custom_setting_value *value,
                             enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_write_by_key(const char *custom_subsystem_id, const char *key,
                                    const struct zmk_custom_setting_value *value,
                                    enum zmk_custom_setting_write_mode mode);
/* Write one array setting element by its public base key and element index. */
int zmk_custom_setting_write_array_by_key(const char *custom_subsystem_id, const char *key,
                                          uint32_t index,
                                          const struct zmk_custom_setting_value *value,
                                          enum zmk_custom_setting_write_mode mode);
/* Write one array setting element and update the active array length. */
int zmk_custom_setting_write_array_element(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *value,
                                           uint32_t array_size,
                                           enum zmk_custom_setting_write_mode mode);
/* Append one value to an array setting and grow the active length by one. */
int zmk_custom_setting_array_push_back(const struct zmk_custom_setting *setting,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode);
/* Remove the last active array element and optionally return its previous value. */
int zmk_custom_setting_array_pop_back(const struct zmk_custom_setting *setting,
                                      struct zmk_custom_setting_value *value,
                                      enum zmk_custom_setting_write_mode mode);
/* Insert one value at index, shifting existing elements at and after index
 * one slot later (memmove over the array's single contiguous buffer) and
 * growing the active length by one. index == current array size behaves
 * like array_push_back. Fails with -ERANGE if the array is already at its
 * maximum size or index is past the (post-insert) end. */
int zmk_custom_setting_array_insert_at(const struct zmk_custom_setting *array_setting_or_element,
                                       uint32_t index, const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode);
/* Remove the value at index, shifting later elements one slot earlier
 * (memmove) and shrinking the active length by one. Optionally returns the
 * removed value. Fails with -ENOENT if index is past the active length. */
int zmk_custom_setting_array_remove_at(const struct zmk_custom_setting *array_setting_or_element,
                                       uint32_t index, struct zmk_custom_setting_value *out_value,
                                       enum zmk_custom_setting_write_mode mode);

/* Save the current memory value to persistent storage. */
int zmk_custom_setting_save(const struct zmk_custom_setting *setting);
/* Discard unsaved memory/temporary changes and restore the persistent/default value. */
int zmk_custom_setting_discard(const struct zmk_custom_setting *setting);
/* Erase the persistent value and restore the default value. */
int zmk_custom_setting_reset(const struct zmk_custom_setting *setting);
/* End a temporary override and reveal the current memory value again. */
int zmk_custom_setting_rollback_temporary(const struct zmk_custom_setting *setting);

/* Apply save to settings matching the optional subsystem/key/prefix filters. */
int zmk_custom_settings_save_scope(const char *custom_subsystem_id, const char *key,
                                   const char *key_prefix, uint32_t *affected_count);
/* Apply discard to settings matching the optional subsystem/key/prefix filters. */
int zmk_custom_settings_discard_scope(const char *custom_subsystem_id, const char *key,
                                      const char *key_prefix, uint32_t *affected_count);
/* Apply reset to settings matching the optional subsystem/key/prefix filters. */
int zmk_custom_settings_reset_scope(const char *custom_subsystem_id, const char *key,
                                    const char *key_prefix, uint32_t *affected_count);

bool zmk_custom_setting_has_unsaved_value(const struct zmk_custom_setting *setting);
/* Validate value type and all constraints for a setting without writing it. */
int zmk_custom_setting_validate(const struct zmk_custom_setting *setting,
                                const struct zmk_custom_setting_value *value);

/*
 * Replace a setting's default value with one resolved at boot time (e.g. from
 * devicetree data that needs behavior local IDs or other runtime-only state
 * to encode). `value` must point to storage that outlives the setting -
 * typically a caller-owned static/BSS-resident struct - since only the
 * pointer is stored, not a copy.
 *
 * Also refreshes the effective (memory) value to match, but only if no
 * persistent value has been loaded and no write has happened yet - so this
 * is safe to call from any SYS_INIT regardless of ordering relative to this
 * module's own registry init, as long as it runs before settings_load()
 * (which always happens later, from main()). Once a persisted or written
 * value exists, this only changes what a later save/discard/reset falls
 * back to.
 */
int zmk_custom_setting_set_default(const struct zmk_custom_setting *setting,
                                   const struct zmk_custom_setting_value *value);

/*
 * View-based read/write API.
 *
 * These avoid requiring callers to declare a full struct
 * zmk_custom_setting_value (sized by CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE)
 * on their own stack just to read or write one small or dynamically-sized
 * value, and let bytes/string values be written from a runtime buffer
 * instead of only the compile-time-literal ZMK_CUSTOM_SETTING_VALUE_BYTES(...)
 * macro.
 */

/* Callback invoked with the effective value while the settings lock is held.
 * Runs synchronously; keep it short, copy out anything needed, and do not
 * call back into other zmk_custom_setting_* functions for the same or other
 * settings from within it. */
typedef void (*zmk_custom_setting_value_visitor_t)(const struct zmk_custom_setting_value *value,
                                                   void *user_data);

/* Borrow the effective value without copying it into a caller-owned
 * struct zmk_custom_setting_value first. */
int zmk_custom_setting_with_value(const struct zmk_custom_setting *setting,
                                  zmk_custom_setting_value_visitor_t visitor, void *user_data);

/* Copy the effective value's raw payload into a caller-sized buffer.
 * Returns -EMSGSIZE if capacity is smaller than the value. out_size and
 * out_type may be NULL. */
int zmk_custom_setting_read_into(const struct zmk_custom_setting *setting, void *buf,
                                 size_t capacity, size_t *out_size,
                                 enum zmk_custom_setting_value_type *out_type);

/* Query a BYTES/STRING setting's current raw payload length without copying
 * it. Used by the streaming RPC value-encode callback and the split relay's
 * pre-encode budget check; -EINVAL if the setting is not BYTES/STRING. */
int zmk_custom_setting_value_size(const struct zmk_custom_setting *setting, size_t *out_size);

/* Visitor invoked with a pointer straight into a BYTES/STRING setting's
 * backing region (no copy) while the settings lock is held. Runs
 * synchronously; keep it short and do not call back into other
 * zmk_custom_setting_* functions from within it. Lets the streaming RPC
 * value-encode callback write a value of any size straight to the output
 * stream with no intermediate copy. */
typedef void (*zmk_custom_setting_raw_bytes_visitor_t)(const uint8_t *data, size_t size,
                                                       void *user_data);

/* Streams a non-array BYTES/STRING value of any size (every such value lives
 * behind state->blob.data). Returns -ENOTSUP if the setting is not
 * BYTES/STRING, is an array element, or currently has a temporary override
 * active - use zmk_custom_setting_with_value / zmk_custom_setting_read_into
 * for those. */
int zmk_custom_setting_with_large_raw_bytes(const struct zmk_custom_setting *setting,
                                            zmk_custom_setting_raw_bytes_visitor_t visitor,
                                            void *user_data);

/* Write a BYTES or STRING setting from a runtime buffer instead of a
 * compile-time-literal value. For STRING settings, size is measured before
 * any truncation to CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE. */
int zmk_custom_setting_write_bytes(const struct zmk_custom_setting *setting, const void *data,
                                   size_t size, enum zmk_custom_setting_write_mode mode);

/* Typed scalar convenience wrappers over read_into/write. Return -EINVAL if
 * the setting is not of the matching type. */
int zmk_custom_setting_get_int32(const struct zmk_custom_setting *setting, int32_t *value);
int zmk_custom_setting_set_int32(const struct zmk_custom_setting *setting, int32_t value,
                                 enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_get_bool(const struct zmk_custom_setting *setting, bool *value);
int zmk_custom_setting_set_bool(const struct zmk_custom_setting *setting, bool value,
                                enum zmk_custom_setting_write_mode mode);

/* Get/set a BEHAVIOR setting. set validates behaviorId/param1/param2 against
 * the target behavior's parameter metadata (see zmk_custom_setting_validate). */
int zmk_custom_setting_get_behavior(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_behavior_value *value);
int zmk_custom_setting_set_behavior(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_behavior_value value,
                                    enum zmk_custom_setting_write_mode mode);

/*
 * Record settings: a struct with a declared field schema, stored as a
 * versioned TLV (field id, length, value) encoding inside a BYTES setting.
 * Unknown field ids are skipped on decode and fields absent from the stored
 * data are left untouched in the destination struct (so callers should
 * populate defaults before calling zmk_custom_setting_record_get); adding a
 * field to the struct does not invalidate previously stored data.
 *
 * This builds on the existing BYTES value type and zmk_custom_setting_write_bytes/
 * read_into; there is no new RPC value type, so a record setting's RPC/export
 * representation is the encoded TLV bytes. zmk_custom_setting_record_set only
 * enforces ZMK_CUSTOM_SETTING_CONSTRAINT_NONE and
 * ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE (on INT32 fields); other constraint
 * kinds fail with -ENOTSUP.
 */
#define ZMK_CUSTOM_SETTING_RECORD_VERSION 1
#define ZMK_CUSTOM_SETTING_RECORD_MAX_FIELD_SIZE 255

struct zmk_custom_setting_record_field {
    uint8_t field_id;
    size_t offset;
    size_t size;
    enum zmk_custom_setting_value_type type;
    const struct zmk_custom_setting_constraint *constraint;
};

struct zmk_custom_setting_record_schema {
    const struct zmk_custom_setting_record_field *fields;
    size_t fields_count;
    size_t record_size;
};

/* _constraint is a pointer to a separately-declared static const
 * struct zmk_custom_setting_constraint (see
 * ZMK_CUSTOM_SETTING_RECORD_RANGE_INT32_DEFINE), or NULL for no constraint.
 * It must be a pointer, not the ZMK_CUSTOM_SETTING_RANGE_INT32(...) compound
 * literal used elsewhere: nesting that inside this field's own initializer
 * hits the same non-constant-expression restriction that
 * ZMK_CUSTOM_SETTING_RANGE_INT32's own definition works around. */
#define ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32(_struct, _field, _id, _constraint)                   \
    {.field_id = (_id),                                                                            \
     .offset = offsetof(_struct, _field),                                                          \
     .size = sizeof(((_struct *)0)->_field),                                                       \
     .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                                  \
     .constraint = (_constraint)}

/* Declares a named, reusable range constraint suitable for
 * ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32's _constraint argument (pass
 * &_name). Uses field initializers rather than the
 * ZMK_CUSTOM_SETTING_RANGE_INT32(...) compound literal so the declaration
 * itself is a valid constant initializer. */
#define ZMK_CUSTOM_SETTING_RECORD_RANGE_INT32_DEFINE(_name, _min, _max)                            \
    static const struct zmk_custom_setting_constraint _name = {                                    \
        .type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,                                               \
        .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_min)},     \
                  .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_max)}}}

#define ZMK_CUSTOM_SETTING_RECORD_FIELD_BOOL(_struct, _field, _id)                                 \
    {.field_id = (_id),                                                                            \
     .offset = offsetof(_struct, _field),                                                          \
     .size = sizeof(((_struct *)0)->_field),                                                       \
     .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,                                                   \
     .constraint = NULL}

/* _field must be a fixed-size char array; the encoded/decoded length always
 * leaves room for a NUL terminator within that array. */
#define ZMK_CUSTOM_SETTING_RECORD_FIELD_STRING(_struct, _field, _id)                               \
    {.field_id = (_id),                                                                            \
     .offset = offsetof(_struct, _field),                                                          \
     .size = sizeof(((_struct *)0)->_field),                                                       \
     .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,                                                 \
     .constraint = NULL}

/* Declares the fields array and schema object as two plain (non-compound-
 * literal) static objects -- required so this expands to a valid constant
 * initializer under strict -std=c11 (see ZMK_CUSTOM_SETTING_RANGE_INT32). */
#define ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE(_name, _struct, ...)                               \
    BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RECORD),                                    \
                 "enable CONFIG_ZMK_CUSTOM_SETTINGS_RECORD to use "                                \
                 "ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE");                                       \
    static const struct zmk_custom_setting_record_field _name##_fields[] = {__VA_ARGS__};          \
    static const struct zmk_custom_setting_record_schema _name = {                                 \
        .fields = _name##_fields,                                                                  \
        .fields_count = ARRAY_SIZE(_name##_fields),                                                \
        .record_size = sizeof(_struct),                                                            \
    }

/* Encode record (a pointer to a struct matching schema) into a TLV byte
 * buffer. Returns -EMSGSIZE if it does not fit out_capacity or a field
 * exceeds ZMK_CUSTOM_SETTING_RECORD_MAX_FIELD_SIZE bytes. */
int zmk_custom_setting_record_encode(const struct zmk_custom_setting_record_schema *schema,
                                     const void *record, uint8_t *out, size_t out_capacity,
                                     size_t *out_size);

/* Decode a TLV byte buffer into record. Fields present in the data but not
 * in schema are skipped; fields in schema but absent from the data are left
 * unmodified in record. Returns -ENOTSUP for an unrecognized version byte. */
int zmk_custom_setting_record_decode(const struct zmk_custom_setting_record_schema *schema,
                                     const uint8_t *data, size_t data_size, void *record);

/* Validate record's fields against schema's constraints, then encode and
 * write it to a BYTES setting via zmk_custom_setting_write_bytes. */
int zmk_custom_setting_record_set(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_setting_record_schema *schema,
                                  const void *record, enum zmk_custom_setting_write_mode mode);

/* Read a BYTES setting's stored TLV bytes and decode them into record.
 * Populate record with defaults before calling, since fields absent from
 * the stored data are left as-is. */
int zmk_custom_setting_record_get(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_setting_record_schema *schema,
                                  void *record);

/*
 * RPC-creatable keyspaces.
 *
 * A keyspace lets a module accept user-created entries (profiles, named
 * mappings, ...) whose keys are not known at compile time, without using the
 * heap. ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE statically allocates a fixed
 * `slots[max_entries]` array; the keyspace descriptor itself is a compile-time
 * STRUCT_SECTION_ITERABLE object, so the linker section *is* the keyspace
 * registry (ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH).
 *
 * A slot's entire entry - its user key *and* its payload - is one opaque
 * pooled BYTES value, `[user_key\0][payload]`. A slot's own
 * `struct zmk_custom_setting.value_type` is therefore always
 * ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES (never the keyspace's *declared*
 * value_type - use zmk_custom_setting_keyspace_of() for that), and its value
 * always comes from the keyspace's pool (descriptor blob.pool), since even a
 * tiny payload's blob still carries its key alongside it. This lets the
 * generic read/write/save/pool code treat a slot as a plain pooled BYTES
 * setting with zero keyspace-specific knowledge; only a handful of
 * presentation/lookup functions (find, the RPC list/get handlers,
 * save/discard/reset scope application) decode the blob, and they do so
 * through the ordinary zmk_custom_setting_read/read_into/with_large_raw_bytes/
 * public_key API - see zmk_custom_setting_keyspace_of().
 *
 * Entries are created/deleted at runtime via
 * zmk_custom_setting_keyspace_create / zmk_custom_setting_keyspace_delete
 * (wrapped by the Studio CreateSetting/DeleteSetting RPC requests).
 *
 * Persisted entries are re-bound to a slot during settings_load(): a slot
 * persists under a stable ordinal name, "<custom_subsystem_id>/<key_prefix>#
 * <index>", not the user key. The SETTINGS_STATIC_HANDLER_DEFINE callback in
 * custom_settings.c recognizes that pattern for any registered keyspace and
 * binds slot `index` directly (no free-slot search, no key needed yet),
 * letting the normal pooled-BYTES value-load path fill in the blob; the user
 * key is then recovered for free by decoding it. So a previously created
 * entry survives reboot with no module code running on its behalf and no
 * separate key-persistence path.
 */

struct zmk_custom_setting_keyspace;

/* Bound for a slot's ordinal storage-name buffer ("<key_prefix>#<index>" +
 * NUL); generous relative to the key_prefix lengths any keyspace is likely
 * to use plus room for CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN worth of
 * digits/prefix and then some - see ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE's
 * BUILD_ASSERT that ties a keyspace's own max_key_len to this same config. */
#define ZMK_CUSTOM_SETTINGS_KEYSPACE_ORDINAL_NAME_SIZE (CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN + 16)

struct zmk_custom_setting_keyspace_slot {
    bool in_use;
    /* This slot's stable storage identity, "<key_prefix>#<index>" - see the
     * keyspace design comment above. Generated once when the slot is
     * (re)bound (zmk_custom_setting_keyspace_create or the settings-load
     * bind path) and pointed to by `setting.key` for the descriptor's
     * lifetime; the *content* is only ever this slot's own fixed ordinal
     * index formatted with this keyspace's key_prefix, so it never actually
     * changes after the first bind in practice. */
    char ordinal_name[ZMK_CUSTOM_SETTINGS_KEYSPACE_ORDINAL_NAME_SIZE];
    /* A slot descriptor is a runtime-built RAM instance of the (normally
     * const/flash) struct zmk_custom_setting, so it embeds its own state
     * block right here; keyspace_bind_slot_locked points setting.state at it. */
    struct zmk_custom_setting_state state;
    struct zmk_custom_setting setting;
};

/*
 * Fixed-size, statically-allocated keyspace state: the `max_entries`-slot
 * array (which cannot itself be pooled, since compaction would invalidate the
 * descriptor pointers callers hold) plus the shared pool every slot's blob is
 * carved from, declared by ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE and referenced
 * by pointer so the keyspace descriptor itself stays a small, constant object.
 */
struct zmk_custom_setting_keyspace {
    const char *custom_subsystem_id;
    /* Every created entry's key must start with this prefix, e.g. "macro/".
     * Also the prefix advertised for discovery via the existing key_prefix
     * scope filter, and the prefix each slot's ordinal storage name is built
     * from. */
    const char *key_prefix;
    /* The PAYLOAD's presented type - what zmk_custom_setting_read/write and
     * the RPC layer treat a slot's value as once the key prefix has been
     * decoded/encoded away. Never equal to a slot's own (always BYTES)
     * struct zmk_custom_setting.value_type. */
    enum zmk_custom_setting_value_type value_type;
    /* Per-keyspace payload size ceiling (BYTES/STRING payloads only; other
     * types' encoded size is fixed by their type regardless of this field).
     * May be smaller or larger than the global
     * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE - up to
     * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE - since every slot's
     * blob is pool-backed regardless of size. */
    uint32_t max_size;
    /* Ceiling on a user key's length, INCLUDING the NUL terminator -
     * validated at zmk_custom_setting_keyspace_create time only (the key
     * rides inside the pooled blob, so there is no fixed per-slot key buffer
     * to size). Must not exceed CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN (see
     * the BUILD_ASSERT on ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE): this bounds the
     * blob assembly/decode scratch used internally. */
    uint32_t max_key_len;
    uint32_t max_entries;
    enum zmk_custom_setting_confidentiality confidentiality;
    enum zmk_custom_setting_permission read_permission;
    enum zmk_custom_setting_permission write_permission;
    const struct zmk_custom_setting_constraint *constraints;
    size_t constraints_count;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;

    struct zmk_custom_setting_keyspace_slot *slots;
    /* Every slot's `[user_key\0][payload]` blob is pool-backed
     * unconditionally - unlike a plain setting, a keyspace slot's stored value
     * is never small enough to skip pooling, since it always carries the key
     * alongside the payload. Sized max_entries * (max_key_len + max_size)
     * bytes by ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE (the worst-case budget for
     * every slot holding a full-size key and payload). */
    struct zmk_custom_setting_large_pool *large_pool;
};

/* Create one entry: `key` must start with keyspace->key_prefix and fit in
 * max_key_len (including NUL). Allocates a free slot and writes `value`
 * (interpreted as keyspace->value_type - validated against
 * keyspace->constraints/max_size) to it in `mode`. Returns -ENOSPC if every
 * slot is in use, -EEXIST if `key` already has a live slot, -ENAMETOOLONG if
 * `key` does not fit, -EINVAL for a mismatched prefix/value type. On
 * success, `*out_setting` (if non-NULL) receives the new entry's descriptor
 * - read/write it with the ordinary zmk_custom_setting_read/write API
 * (values are always typed/validated as keyspace->value_type, never BYTES,
 * for a BYTES-declared keyspace this is a no-op distinction). */
int zmk_custom_setting_keyspace_create(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode,
                                       const struct zmk_custom_setting **out_setting);

/* Delete one entry by key: erases its persisted storage record (if any) and
 * releases its slot back to the pool for reuse. Returns -ENOENT if `key` has
 * no live slot in this keyspace. */
int zmk_custom_setting_keyspace_delete(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key);

/* Find a keyspace's currently live entry by its literal user key (e.g.
 * "macro/my-macro-1"), or NULL if not bound to a slot right now. Decodes
 * each live slot's blob to compare (bounded by max_entries, not the pool
 * byte size). */
const struct zmk_custom_setting *
zmk_custom_setting_keyspace_find(const struct zmk_custom_setting_keyspace *keyspace,
                                 const char *key);

/* Find the registered keyspace (if any) whose custom_subsystem_id matches
 * and whose key_prefix is a prefix of `key` - used by the Studio RPC
 * CreateSetting/DeleteSetting handlers to resolve a request's plain
 * (subsystem, key) pair to the keyspace that owns it, without the caller
 * needing its own pointer to the struct zmk_custom_setting_keyspace. */
struct zmk_custom_setting_keyspace *
zmk_custom_settings_keyspace_find_for_key(const char *custom_subsystem_id, const char *key);

/*
 * Suppress the Studio "setting changed" notification that a core write
 * (zmk_custom_setting_write and friends, keyspace create/delete, save,
 * discard, reset) would otherwise emit, for the duration of a begin/end
 * bracket. Nestable (reference-counted).
 *
 * A module that mutates custom settings from inside its OWN Studio RPC
 * subsystem handler must bracket that mutation: the module's own RPC response
 * already confirms the change to the single-connection Studio client, so the
 * redundant custom-settings notification would compete with that response on
 * the shared transport and can starve it. This module's own RPC entry point
 * brackets its dispatch for exactly this reason; the notification listener
 * runs synchronously in the writing thread, so on a small RPC thread stack the
 * extra encode depth can also overflow it. Non-RPC mutations (boot
 * settings-load, background firmware writes) must NOT be bracketed - their
 * notifications are the only signal Studio gets.
 *
 * No-ops when the Studio RPC layer is not built (no notifications exist).
 */
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC)
void zmk_custom_settings_notify_suppress_begin(void);
void zmk_custom_settings_notify_suppress_end(void);
#else
static inline void zmk_custom_settings_notify_suppress_begin(void) {}
static inline void zmk_custom_settings_notify_suppress_end(void) {}
#endif

/* Returns the owning keyspace if `setting` is a live entry created/bound by
 * a ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE keyspace, or NULL for a normal
 * setting. A slot's payload is typed/constrained per the returned
 * keyspace's value_type/constraints/max_size, not setting->value_type
 * (always ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES internally - see the keyspace
 * design comment above). zmk_custom_setting_read/read_into/write/write_bytes/
 * with_large_raw_bytes/public_key all consult this already; most callers
 * never need to call it directly.
 *
 * static inline (not out-of-line) so it folds to a compile-time constant NULL
 * when CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE is off: every
 * `if (zmk_custom_setting_keyspace_of(setting)) { ...keyspace-only code... }`
 * branch then becomes provably dead code, so the keyspace-only functions it
 * calls (defined in src/custom_settings_keyspace.c, not compiled when the
 * feature is off) are dropped by the linker without an #ifdef at each call
 * site. */
static inline const struct zmk_custom_setting_keyspace *
zmk_custom_setting_keyspace_of(const struct zmk_custom_setting *setting) {
    return (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE) && setting) ? setting->_keyspace : NULL;
}

/* Iterate every registered keyspace (STRUCT_SECTION_ITERABLE - the linker
 * section is the registry, mirroring ZMK_CUSTOM_SETTING_FOREACH). Internal
 * to this module + its Studio RPC handler. */
#define ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(_var)                                                  \
    STRUCT_SECTION_FOREACH(zmk_custom_setting_keyspace, _var)

/* Register a keyspace with a fixed-size static slot array and shared blob
 * pool. _max_key_len must include room for the NUL terminator and must not
 * exceed CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN. */
#define ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(                                                        \
    _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len, _max_entries,  \
    _confidentiality, _read_permission, _write_permission, _constraint)                            \
    ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_CONSTRAINTS(                                           \
        _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len,            \
        _max_entries, _confidentiality, _read_permission, _write_permission, _constraint)

#define ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_CONSTRAINTS(                                       \
    _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len, _max_entries,  \
    _confidentiality, _read_permission, _write_permission, ...)                                    \
    ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                        \
        _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len,            \
        _max_entries, _confidentiality, _read_permission, _write_permission, NULL, NULL,           \
        __VA_ARGS__)

/* No separate BUILD_ASSERT(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS) here
 * (compare ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS /
 * ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS): unlike those two
 * features, keyspaces have no separate "plain" entry point that bypasses
 * this macro - ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_CONSTRAINTS (the
 * no-converters path) funnels through here too, passing NULL/NULL. Asserting
 * here would misfire on that plain path with RPC_CONVERTERS off. A keyspace
 * registered with real converters while RPC_CONVERTERS is off silently no-ops
 * the conversion (falls through to identity passthrough) instead of failing
 * to build; document CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS as a
 * prerequisite for keyspace converters in the README instead. */
#define ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                    \
    _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len, _max_entries,  \
    _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer,     \
    ...)                                                                                           \
    ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE_RPC_CONVERTERS_AND_CONSTRAINTS(              \
        _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len,            \
        _max_entries, (_max_entries) * ((_max_key_len) + (_max_size)), _confidentiality,           \
        _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, __VA_ARGS__)

/* Same as ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE, but takes an explicit blob-pool
 * budget (_pool_size) instead of sizing the pool at the worst case
 * max_entries * (max_key_len + max_size). Over-committing the pool lets a
 * keyspace hold many small entries or a handful of large ones from one shared
 * budget, instead of always paying for max_entries full-size entries.
 * _pool_size must be large enough for at least one full-size entry
 * (max_key_len + max_size); create()/write() return -ENOSPC once the budget
 * is exhausted, same as any other pooled BYTES setting. */
#define ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE(                                         \
    _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len, _max_entries,  \
    _pool_size, _confidentiality, _read_permission, _write_permission, _constraint)                \
    ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE_RPC_CONVERTERS_AND_CONSTRAINTS(              \
        _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len,            \
        _max_entries, _pool_size, _confidentiality, _read_permission, _write_permission, NULL,     \
        NULL, _constraint)

#define ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE_RPC_CONVERTERS_AND_CONSTRAINTS(          \
    _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len, _max_entries,  \
    _pool_size, _confidentiality, _read_permission, _write_permission, _rpc_serializer,            \
    _rpc_deserializer, ...)                                                                        \
    BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE),                                  \
                 "enable CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE to use "                              \
                 "ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE");                                            \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT((_max_size) <= CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,                   \
                 "Keyspace max_size exceeds CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE");     \
    BUILD_ASSERT((_max_key_len) > sizeof(_key_prefix),                                             \
                 "Keyspace max_key_len must leave room for at least one suffix byte plus NUL "     \
                 "after the prefix");                                                              \
    BUILD_ASSERT((_max_key_len) <= CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                         \
                 "Keyspace max_key_len exceeds CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN (a slot's "  \
                 "blob-encode/decode scratch is sized off this bound)");                           \
    BUILD_ASSERT((_max_size) <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ||                       \
                     (_value_type) == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||                       \
                     (_value_type) == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,                        \
                 "A keyspace max_size larger than the fixed carrier requires a BYTES/STRING "      \
                 "value_type");                                                                    \
    BUILD_ASSERT((_pool_size) >= (_max_key_len) + (_max_size),                                     \
                 "Keyspace pool_size must fit at least one full-size entry (max_key_len + "        \
                 "max_size)");                                                                     \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    static struct zmk_custom_setting_keyspace_slot _name##_slots[_max_entries];                    \
    /* Every entry's [key\0][payload] blob is pool-backed unconditionally -                        \
     * see the .large_pool field comment on struct                                                 \
     * zmk_custom_setting_keyspace. _pool_size may be smaller than the                             \
     * worst-case max_entries * (max_key_len + max_size) budget - see                              \
     * ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE. */                                       \
    ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(_name##_pool, (_pool_size));                              \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting_keyspace, _name) = {                                \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key_prefix = _key_prefix,                                                                 \
        .value_type = _value_type,                                                                 \
        .max_size = (_max_size),                                                                   \
        .max_key_len = (_max_key_len),                                                             \
        .max_entries = (_max_entries),                                                             \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
        .slots = _name##_slots,                                                                    \
        .large_pool = &_name##_pool,                                                               \
    }
