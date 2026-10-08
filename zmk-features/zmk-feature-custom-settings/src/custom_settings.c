/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <cormoran/zmk/custom_settings.h>
#include <zmk/keymap.h>

#include "custom_settings_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_custom_setting_changed);
ZMK_EVENT_IMPL(zmk_custom_settings_initialized);

K_MUTEX_DEFINE(custom_settings_lock);

size_t bounded_strlen(const char *str, size_t max_len) {
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        len++;
    }

    return len;
}

static bool value_equals(const struct zmk_custom_setting_value *a,
                         const struct zmk_custom_setting_value *b) {
    if (a->type != b->type) {
        return false;
    }

    switch (a->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        return a->size == b->size && memcmp(a->bytes_value, b->bytes_value, a->size) == 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        return a->int32_value == b->int32_value;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        return a->bool_value == b->bool_value;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        return strncmp(a->string_value, b->string_value,
                       CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) == 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        return a->behavior_value.behavior_id == b->behavior_value.behavior_id &&
               a->behavior_value.param1 == b->behavior_value.param1 &&
               a->behavior_value.param2 == b->behavior_value.param2;
    default:
        return false;
    }
}

void copy_value(struct zmk_custom_setting_value *dest, const struct zmk_custom_setting_value *src) {
    *dest = *src;
    if (dest->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        dest->string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE] = '\0';
        dest->size = bounded_strlen(dest->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    }
}

/*
 * State flag helpers. All mutable per-setting flags live as bits in
 * setting->state->flags; the descriptor itself is const. Caller holds
 * custom_settings_lock for any use that must be coherent with other state
 * fields.
 */
static bool state_flag(const struct zmk_custom_setting *setting, uint8_t flag) {
    return (setting->state->flags & flag) != 0;
}

static void state_flag_set(const struct zmk_custom_setting *setting, uint8_t flag, bool value) {
    if (value) {
        setting->state->flags |= flag;
    } else {
        setting->state->flags &= ~flag;
    }
}

bool setting_temporary_active(const struct zmk_custom_setting *setting) {
    return state_flag(setting, ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE);
}

/*
 * Per-setting BYTES/STRING blob store.
 *
 * Every non-array BYTES/STRING setting keeps its payload behind
 * state->blob.data: either the exact-size static buffer the plain
 * ZMK_CUSTOM_SETTING_DEFINE macro emits (descriptor blob.pool == NULL; the
 * pointer never changes), or a region carved on demand from a shared pool
 * (ZMK_CUSTOM_SETTING_DEFINE_POOLED / _SIZED's private pool / a keyspace's
 * slot pool; blob.data is NULL until the first non-empty write - see
 * pool_ensure_region() below). These helpers are the single read/write path
 * for blob values of any size, and `blob.pool == NULL` is the only
 * fixed-buffer vs pool-member distinction.
 */
size_t setting_capacity(const struct zmk_custom_setting *setting) {
    if (!zmk_custom_setting_is_array(setting) &&
        (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
         setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING)) {
        return setting->blob.max_size;
    }
    return CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE;
}

bool setting_uses_blob_store(const struct zmk_custom_setting *setting) {
    return !zmk_custom_setting_is_array(setting) &&
           (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
            setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING);
}

/* Store `size` raw payload bytes into a blob setting's store. Caller must
 * have validated size <= setting_capacity(setting). For a pooled setting,
 * first (re)points blob.data at a big-enough region via pool_ensure_region(),
 * possibly relocating other members of the same pool; on -ENOSPC nothing is
 * modified. For a fixed-store setting (descriptor blob.pool == NULL) the
 * buffer never moves and, being sized capacity + 1 by the registration
 * macro, always has room for the STRING NUL. */
int blob_store_set_raw(const struct zmk_custom_setting *setting, const void *data, size_t size) {
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES) && setting->blob.pool != NULL) {
        size_t needed =
            size + (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING ? 1 : 0);
        int ret = pool_ensure_region(setting, needed);
        if (ret < 0) {
            return ret;
        }
    }

    if (size > 0) {
        memcpy(setting->state->blob.data, data, size);
    }
    if (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING &&
        setting->state->blob.data != NULL) {
        setting->state->blob.data[size] = '\0';
    }
    setting->state->blob.size = size;
    return 0;
}

/* Copy a carrier value (BYTES/STRING, <= carrier size) into a blob setting's
 * store - the normal zmk_custom_setting_write / default-application path.
 * Returns -ENOSPC for a pooled setting whose backing pool has no room (see
 * blob_store_set_raw); the setting's previous value is left untouched in
 * that case. */
static int blob_store_set_value(const struct zmk_custom_setting *setting,
                                const struct zmk_custom_setting_value *value) {
    if (value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        size_t len = bounded_strlen(value->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        len = MIN(len, setting_capacity(setting));
        return blob_store_set_raw(setting, value->string_value, len);
    } else {
        size_t len = MIN(value->size, setting_capacity(setting));
        return blob_store_set_raw(setting, value->bytes_value, len);
    }
}

/* The default a setting currently falls back to: the runtime override
 * installed by zmk_custom_setting_set_default() if any, else the
 * registration-time (flash) default. */
static const struct zmk_custom_setting_value *
setting_default_value(const struct zmk_custom_setting *setting) {
    return setting->state->default_override ? setting->state->default_override
                                            : setting->default_value;
}

/* Store a validated carrier value as a non-array setting's in-memory value:
 * blob settings route to their store (pool region or fixed buffer), scalars
 * inline into the right-sized state union. Caller holds custom_settings_lock.
 * Returns -ENOSPC only for a pooled blob setting out of pool room. */
static int store_scalar_value_locked(const struct zmk_custom_setting *setting,
                                     const struct zmk_custom_setting_value *value) {
    if (setting_uses_blob_store(setting)) {
        return blob_store_set_value(setting, value);
    }

    struct zmk_custom_setting_state *state = setting->state;
    switch (setting->value_type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        state->int32_value = value->int32_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        state->bool_value = value->bool_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        state->behavior = value->behavior_value;
        return 0;
    default:
        return -EINVAL;
    }
}

/* Apply a scalar setting's default (override or compile-time) to its memory
 * storage. Caller holds custom_settings_lock. An empty (size 0) default never
 * allocates a pool region (blob_store_set_raw's needed == 0 case). A
 * pooled setting whose non-empty default cannot fit its pool at init time
 * logs and leaves the setting without a region rather than failing boot. */
static void apply_scalar_default_locked(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_keyspace_of(setting)) {
        /* A keyspace slot has no compile-time default payload: its blob
         * always carries a user key that only exists once the entry is
         * created/bound, so there is no shared static value that could
         * represent "this not-yet-existing entry's default". Leave the
         * blob untouched here - a freshly bound slot starts empty
         * (blob.data == NULL) and is populated moments later either by
         * the settings-load value-apply step or by keyspace_write_blob()
         * (create). This also means discard()/reset() on a keyspace entry
         * with no persisted record are no-ops rather than reverting to a
         * default unrelated to the entry's own key. */
        return;
    }
    int ret = store_scalar_value_locked(setting, setting_default_value(setting));
    if (ret < 0) {
        LOG_ERR("Custom settings pool has no room for %s/%s's default value (%d)",
                setting->custom_subsystem_id, setting->key, ret);
    }
}

/*
 * Temporary overrides (ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY) are rare and
 * short-lived, so instead of a full struct zmk_custom_setting_value slot
 * embedded in every registered setting, a small shared pool holds the few
 * that are active at once. Settings whose value is larger than one slot
 * simply cannot use temporary mode (-EMSGSIZE).
 */
struct zmk_custom_settings_temp_slot {
    bool in_use;
    const struct zmk_custom_setting *owner;
    enum zmk_custom_setting_value_type type;
    size_t size;
    uint8_t data[CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE];
};

static struct zmk_custom_settings_temp_slot temp_slots[CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS];
/* Scratch space effective_value() reconstructs a temporary override into.
 * Safe as a single shared instance: all access happens while custom_settings_lock
 * is held. */
static struct zmk_custom_setting_value temp_scratch_value;
/* Scratch space effective_value() materializes a large-store setting's value
 * into when it still fits the fixed carrier. Same single-shared-instance
 * safety as temp_scratch_value (custom_settings_lock held). */
static struct zmk_custom_setting_value effective_scratch_value;

/*
 * BEHAVIOR values are stored/cached the same way ZMK's own keymap settings
 * storage persists behavior bindings (struct zmk_behavior_binding_setting in
 * dependencies/zmk's app/src/keymap.c): a packed struct with trailing
 * all-zero params truncated off before writing, since most bindings only use
 * one or zero params. Reading zero-initializes the struct first, so a
 * truncated stored value naturally decodes back to param1/param2 == 0.
 */
struct zmk_custom_setting_behavior_storage {
    zmk_behavior_local_id_t behavior_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

#define BEHAVIOR_VALUE_ENCODED_MAX_SIZE sizeof(struct zmk_custom_setting_behavior_storage)

static int encode_behavior_value(const struct zmk_custom_setting_behavior_value *behavior,
                                 uint8_t *out, size_t out_capacity, size_t *out_size) {
    if (out_capacity < BEHAVIOR_VALUE_ENCODED_MAX_SIZE) {
        return -EMSGSIZE;
    }

    struct zmk_custom_setting_behavior_storage storage = {
        .behavior_id = (zmk_behavior_local_id_t)behavior->behavior_id,
        .param1 = behavior->param1,
        .param2 = behavior->param2,
    };

    size_t len = sizeof(storage);
    if (storage.param2 == 0) {
        len -= sizeof(storage.param2);

        if (storage.param1 == 0) {
            len -= sizeof(storage.param1);
        }
    }

    memcpy(out, &storage, len);
    *out_size = len;
    return 0;
}

static int decode_behavior_value(const uint8_t *data, size_t size,
                                 struct zmk_custom_setting_behavior_value *behavior) {
    if (size > BEHAVIOR_VALUE_ENCODED_MAX_SIZE) {
        return -EMSGSIZE;
    }

    struct zmk_custom_setting_behavior_storage storage = {0};
    memcpy(&storage, data, size);

    behavior->behavior_id = storage.behavior_id;
    behavior->param1 = storage.param1;
    behavior->param2 = storage.param2;
    return 0;
}

/* Shared scratch buffer for encode_behavior_value() output. Safe as a single
 * instance: every value_to_storage() caller holds custom_settings_lock (see
 * temp_scratch_value above for the same pattern). */
static uint8_t behavior_encode_scratch[BEHAVIOR_VALUE_ENCODED_MAX_SIZE];

void value_from_raw(struct zmk_custom_setting_value *dest, enum zmk_custom_setting_value_type type,
                    const void *data, size_t size) {
    dest->type = type;
    switch (type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        dest->size = size;
        memcpy(dest->bytes_value, data, size);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        memcpy(dest->string_value, data, size);
        dest->string_value[size] = '\0';
        dest->size = size;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        memcpy(&dest->int32_value, data, sizeof(dest->int32_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        memcpy(&dest->bool_value, data, sizeof(dest->bool_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        decode_behavior_value(data, size, &dest->behavior_value);
        break;
    default:
        break;
    }
}

static int temp_slot_find(const struct zmk_custom_setting *setting) {
    for (size_t i = 0; i < ARRAY_SIZE(temp_slots); i++) {
        if (temp_slots[i].in_use && temp_slots[i].owner == setting) {
            return (int)i;
        }
    }

    return -1;
}

static int temp_slot_alloc(const struct zmk_custom_setting *setting) {
    int existing = temp_slot_find(setting);
    if (existing >= 0) {
        return existing;
    }

    for (size_t i = 0; i < ARRAY_SIZE(temp_slots); i++) {
        if (!temp_slots[i].in_use) {
            temp_slots[i] =
                (struct zmk_custom_settings_temp_slot){.in_use = true, .owner = setting};
            return (int)i;
        }
    }

    return -1;
}

static void temp_slot_free(int slot) {
    if (slot >= 0 && (size_t)slot < ARRAY_SIZE(temp_slots)) {
        temp_slots[slot] = (struct zmk_custom_settings_temp_slot){0};
    }
}

/* Clear an active temporary override, if any, and release its pool slot.
 * Use this instead of clearing the TEMPORARY_ACTIVE state flag directly so
 * the pool slot is not leaked. */
void clear_temporary_locked(const struct zmk_custom_setting *setting) {
    struct zmk_custom_setting_state *state = setting->state;
    /* Check slot ownership (not just state->temp_slot >= 0) so a state
     * block that was never initialized to temp_slot = -1 (e.g. a
     * zero-initialized view-pool entry) cannot free a slot actually owned
     * by a different setting. */
    if (state->temp_slot >= 0 && (size_t)state->temp_slot < ARRAY_SIZE(temp_slots) &&
        temp_slots[state->temp_slot].owner == setting) {
        temp_slot_free(state->temp_slot);
    }
    state->temp_slot = -1;
    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE, false);
}

static bool setting_is_dirty(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        return *array_dirty_slot(setting);
    }

    return state_flag(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY);
}

void set_setting_dirty(const struct zmk_custom_setting *setting, bool dirty) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        *array_dirty_slot(setting) = dirty;
        return;
    }

    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY, dirty);
}

static bool setting_has_persistent_value(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        return *array_has_persistent_slot(setting);
    }

    return state_flag(setting, ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT);
}

static void set_setting_has_persistent_value(const struct zmk_custom_setting *setting, bool value) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        *array_has_persistent_slot(setting) = value;
        return;
    }

    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT, value);
}

/*
 * Materialize the current MEMORY value (never a temporary override) as a
 * fixed carrier, or NULL when a blob payload does not fit the carrier
 * (callers must then use zmk_custom_setting_read_into / the raw blob path
 * instead). Array elements are stored as carriers already and are returned
 * in place; a scalar or a carrier-sized blob value is materialized into the
 * shared effective_scratch_value (custom_settings_lock held, like every scratch in
 * this file).
 */
static const struct zmk_custom_setting_value *
memory_value_locked(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        return &setting->array_state->values[setting->array_index];
    }

    const struct zmk_custom_setting_state *state = setting->state;

    if (setting_uses_blob_store(setting)) {
        if (state->blob.size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return NULL;
        }
        /* A pooled setting with no region yet (blob.data == NULL) holds an
         * empty value - value_from_raw must not be handed a NULL source
         * pointer even for a zero-length copy. */
        value_from_raw(&effective_scratch_value, setting->value_type,
                       state->blob.data != NULL ? state->blob.data : (const uint8_t *)"",
                       state->blob.size);
        return &effective_scratch_value;
    }

    effective_scratch_value = (struct zmk_custom_setting_value){.type = setting->value_type};
    switch (setting->value_type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        effective_scratch_value.int32_value = state->int32_value;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        effective_scratch_value.bool_value = state->bool_value;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        effective_scratch_value.behavior_value = state->behavior;
        break;
    default:
        break;
    }
    return &effective_scratch_value;
}

/*
 * Return the effective value as a fixed carrier, or NULL when the value is a
 * blob payload that does not fit the carrier (callers must then use
 * zmk_custom_setting_read_into / the chunked RPC instead). A temporary
 * override (always carrier-sized) takes precedence over the memory value.
 */
const struct zmk_custom_setting_value *effective_value(const struct zmk_custom_setting *setting) {
    if (setting_temporary_active(setting) && setting->state->temp_slot >= 0) {
        const struct zmk_custom_settings_temp_slot *slot = &temp_slots[setting->state->temp_slot];
        value_from_raw(&temp_scratch_value, slot->type, slot->data, slot->size);
        return &temp_scratch_value;
    }

    return memory_value_locked(setting);
}

void raise_setting_changed(const struct zmk_custom_setting *setting,
                           enum zmk_custom_setting_changed_kind kind) {
    raise_zmk_custom_setting_changed((struct zmk_custom_setting_changed){
        .setting = setting,
        .kind = kind,
        .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
    });
}

bool zmk_custom_setting_matches(const struct zmk_custom_setting *setting,
                                const char *custom_subsystem_id, const char *key,
                                const char *key_prefix) {
    const char *public_key = zmk_custom_setting_public_key(setting);

    if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
        strncmp(setting->custom_subsystem_id, custom_subsystem_id,
                CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
        return false;
    }

    if (key && key[0] != '\0') {
        bool key_matches_storage =
            strncmp(setting->key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0;
        bool key_matches_public =
            strncmp(public_key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0;
        if (!key_matches_storage && !key_matches_public) {
            return false;
        }
    }

    if (key_prefix && key_prefix[0] != '\0') {
        bool prefix_matches_storage = strncmp(setting->key, key_prefix, strlen(key_prefix)) == 0;
        bool prefix_matches_public = strncmp(public_key, key_prefix, strlen(key_prefix)) == 0;
        if (!prefix_matches_storage && !prefix_matches_public) {
            return false;
        }
    }

    return true;
}

const struct zmk_custom_setting *zmk_custom_setting_find(const char *custom_subsystem_id,
                                                         const char *key) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
            strncmp(setting->custom_subsystem_id, custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
            continue;
        }

        if (key && key[0] != '\0' &&
            strncmp(setting->key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0) {
            return setting;
        }
    }

    if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE) || !key || key[0] == '\0') {
        return NULL;
    }

    /* Keyspace slots are not reachable through ZMK_CUSTOM_SETTING_FOREACH,
     * so check keyspaces explicitly to keep a user-created entry find-able
     * by its literal user key. Guarded by IS_ENABLED (not
     * zmk_custom_setting_keyspace_of, which needs a live setting) so a
     * CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=n build never references
     * custom_settings_keyspace.c's zmk_custom_settings_keyspace_find_for_key/
     * zmk_custom_setting_keyspace_find. */
    struct zmk_custom_setting_keyspace *keyspace =
        zmk_custom_settings_keyspace_find_for_key(custom_subsystem_id, key);
    return keyspace ? zmk_custom_setting_keyspace_find(keyspace, key) : NULL;
}

const char *zmk_custom_setting_public_key(const struct zmk_custom_setting *setting) {
    const struct zmk_custom_setting_keyspace *keyspace = zmk_custom_setting_keyspace_of(setting);
    if (keyspace) {
        k_mutex_lock(&custom_settings_lock, K_FOREVER);
        const char *key = keyspace_public_key_locked(setting);
        k_mutex_unlock(&custom_settings_lock);
        return key;
    }

    return zmk_custom_setting_is_array(setting) ? setting->array_key : setting->key;
}

/* Validate a BEHAVIOR value's behaviorId/param1/param2 against the target
 * behavior's real ZMK parameter metadata (CONFIG_ZMK_BEHAVIOR_METADATA),
 * mirroring how validate_behavior_id_constraint below resolves a behavior
 * local id, but additionally checking param1/param2 through
 * zmk_behavior_validate_binding instead of accepting any uint32_t. */
static int validate_behavior_value(const struct zmk_custom_setting_behavior_value *behavior) {
    if (behavior->behavior_id >= UINT16_MAX) {
        return -ERANGE;
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    const char *behavior_dev_name = zmk_behavior_find_behavior_name_from_local_id(
        (zmk_behavior_local_id_t)behavior->behavior_id);
    if (!behavior_dev_name) {
        return -EINVAL;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = behavior_dev_name,
        .param1 = behavior->param1,
        .param2 = behavior->param2,
    };
    return zmk_behavior_validate_binding(&binding);
#else
    return -ENOTSUP;
#endif
}

static int value_type_validate(const struct zmk_custom_setting *setting,
                               const struct zmk_custom_setting_value *value) {
    if (setting->value_type != value->type) {
        return -EINVAL;
    }

    switch (value->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        return value->size <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? 0 : -EMSGSIZE;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        return value->size <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? 0 : -EMSGSIZE;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        return validate_behavior_value(&value->behavior_value);
    default:
        return -EINVAL;
    }
}

static int compare_values(const struct zmk_custom_setting_value *a,
                          const struct zmk_custom_setting_value *b) {
    if (a->type != b->type) {
        return 0;
    }

    switch (a->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        return (a->int32_value > b->int32_value) - (a->int32_value < b->int32_value);
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        return (a->bool_value > b->bool_value) - (a->bool_value < b->bool_value);
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        return strncmp(a->string_value, b->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES: {
        size_t len = MIN(a->size, b->size);
        int cmp = memcmp(a->bytes_value, b->bytes_value, len);
        if (cmp != 0) {
            return cmp;
        }
        return (a->size > b->size) - (a->size < b->size);
    }
    default:
        return 0;
    }
}

static int validate_int32_value(const struct zmk_custom_setting_value *value,
                                int32_t *int32_value) {
    if (value->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        return -EINVAL;
    }

    *int32_value = value->int32_value;
    return 0;
}

static int validate_hid_usage_constraint(const struct zmk_custom_setting_constraint *constraint,
                                         const struct zmk_custom_setting_value *value) {
    int32_t int32_value;
    int ret = validate_int32_value(value, &int32_value);
    if (ret < 0) {
        return ret;
    }
    if (int32_value < 0) {
        return -ERANGE;
    }

    uint32_t usage = (uint32_t)int32_value;
    uint32_t usage_page = ZMK_HID_USAGE_PAGE(usage);
    uint32_t usage_id = ZMK_HID_USAGE_ID(usage);

    if (usage_page == 0U) {
        usage_page = constraint->hid_usage.usage_page;
        usage_id = usage;
    }

    if (usage_page != constraint->hid_usage.usage_page ||
        usage_id < constraint->hid_usage.usage_min || usage_id > constraint->hid_usage.usage_max) {
        return -ERANGE;
    }

    return 0;
}

static int validate_layer_id_constraint(const struct zmk_custom_setting_value *value) {
    int32_t layer_id;
    int ret = validate_int32_value(value, &layer_id);
    if (ret < 0) {
        return ret;
    }

    return layer_id >= 0 && layer_id < ZMK_KEYMAP_LAYERS_LEN ? 0 : -ERANGE;
}

static int validate_behavior_id_constraint(const struct zmk_custom_setting_value *value) {
    int32_t behavior_id;
    int ret = validate_int32_value(value, &behavior_id);
    if (ret < 0) {
        return ret;
    }
    if (behavior_id < 0 || behavior_id >= UINT16_MAX) {
        return -ERANGE;
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    return zmk_behavior_find_behavior_name_from_local_id((zmk_behavior_local_id_t)behavior_id)
               ? 0
               : -EINVAL;
#else
    return -ENOTSUP;
#endif
}

int zmk_custom_setting_validate(const struct zmk_custom_setting *setting,
                                const struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    int ret = value_type_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    for (size_t c = 0; c < setting->constraints_count; c++) {
        const struct zmk_custom_setting_constraint *constraint = &setting->constraints[c];

        switch (constraint->type) {
        case ZMK_CUSTOM_SETTING_CONSTRAINT_NONE:
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE:
            ret = validate_hid_usage_constraint(constraint, value);
            if (ret < 0) {
                return ret;
            }
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID:
            ret = validate_layer_id_constraint(value);
            if (ret < 0) {
                return ret;
            }
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID:
            ret = validate_behavior_id_constraint(value);
            if (ret < 0) {
                return ret;
            }
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE:
            if (compare_values(value, &constraint->range.min) < 0 ||
                compare_values(value, &constraint->range.max) > 0) {
                return -ERANGE;
            }
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS: {
            bool matched = false;
            for (size_t i = 0; i < constraint->options.count; i++) {
                if (value_equals(value, &constraint->options.values[i])) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return -EINVAL;
            }
            break;
        }
        default:
            return -EINVAL;
        }
    }

    return 0;
}

int zmk_custom_setting_set_default(const struct zmk_custom_setting *setting,
                                   const struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    /* Array elements have per-index defaults from array_state->defaults,
     * set once at registration time (ZMK_CUSTOM_SETTING_ARRAY_DEFINE); there
     * is no runtime-replaceable default for an array element/view. */
    if (zmk_custom_setting_is_array(setting)) {
        return -ENOTSUP;
    }

    int ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    /* The descriptor (and its default_value pointer) is const/flash-resident,
     * so the replacement default lives on the RAM state block instead and
     * setting_default_value() consults it first everywhere a default is read
     * (init, discard, reset). */
    setting->state->default_override = value;
    /* No persisted value has been loaded and no in-memory write has happened
     * yet, so the current memory value (whatever it was materialized to, or
     * even its zero-initialized pre-init state) still represents "unset" -
     * refresh it too. This makes the call safe regardless of whether it runs
     * before or after this module's own registry init, as long as it is
     * before settings_load() (i.e. from any SYS_INIT). */
    if (!setting_has_persistent_value(setting) &&
        !state_flag(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY)) {
        int store_ret = store_scalar_value_locked(setting, value);
        if (store_ret < 0) {
            k_mutex_unlock(&custom_settings_lock);
            return store_ret;
        }
    }
    k_mutex_unlock(&custom_settings_lock);

    return 0;
}

int setting_storage_name(const struct zmk_custom_setting *setting, char *name, size_t name_size) {
    int ret;

    /* Array element views do not carry a baked-in "key/index" string (there
     * is no per-element registration anymore); build the storage name from
     * the shared array_key plus this view's array_index instead. */
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        ret = snprintf(name, name_size, SETTINGS_SUBTREE "/%s/%s/%u", setting->custom_subsystem_id,
                       setting->array_key, setting->array_index);
    } else {
        ret = snprintf(name, name_size, SETTINGS_SUBTREE "/%s/%s", setting->custom_subsystem_id,
                       setting->key);
    }
    if (ret < 0 || ret >= name_size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

int value_to_storage(const struct zmk_custom_setting_value *value, const void **data, size_t *len) {
    switch (value->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        *data = value->bytes_value;
        *len = value->size;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        *data = &value->int32_value;
        *len = sizeof(value->int32_value);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        *data = &value->bool_value;
        *len = sizeof(value->bool_value);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        *data = value->string_value;
        *len = bounded_strlen(value->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR: {
        size_t size;
        int ret = encode_behavior_value(&value->behavior_value, behavior_encode_scratch,
                                        sizeof(behavior_encode_scratch), &size);
        if (ret < 0) {
            return ret;
        }
        *data = behavior_encode_scratch;
        *len = size;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

/* True when a non-array setting's current in-memory value is byte-for-byte the
 * value it would fall back to with no persisted record (its runtime override or
 * compile-time default). Caller holds custom_settings_lock. Keyspace slots have
 * no compile-time default (their blob always carries a user key), so callers
 * must exclude them before asking. */
static bool value_matches_default_locked(const struct zmk_custom_setting *setting) {
    const struct zmk_custom_setting_value *def = setting_default_value(setting);
    if (!def) {
        return false;
    }

    if (setting_uses_blob_store(setting)) {
        const void *def_data;
        size_t def_len;
        if (def->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
            def_data = def->string_value;
            def_len = bounded_strlen(def->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        } else {
            def_data = def->bytes_value;
            def_len = def->size;
        }

        if (setting->state->blob.size != def_len) {
            return false;
        }
        if (def_len == 0) {
            return true;
        }
        return setting->state->blob.data != NULL &&
               memcmp(setting->state->blob.data, def_data, def_len) == 0;
    }

    const struct zmk_custom_setting_value *memory = memory_value_locked(setting);
    return memory != NULL && value_equals(memory, def);
}

int save_setting_locked(const struct zmk_custom_setting *setting) {
    /* An array descriptor or any index view of it saves the whole array
     * (all active elements plus the "_size" marker) - see save_array_locked.
     * This is also correct for a single element view (e.g. the "tail"
     * element passed by zmk_custom_setting_array_pop_back): saving the
     * whole array is a superset of saving one element and keeps this
     * function's callers (zmk_custom_setting_save, write_value_locked)
     * simple. */
    if (zmk_custom_setting_is_array(setting)) {
        return save_array_locked(setting);
    }

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(setting, name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    /* Persisting a value equal to the setting's default would just store a
     * redundant copy that also masks any later change to that default. Delete
     * the record instead so the setting transparently falls back to the
     * (possibly updated) default on the next boot. Keyspace slots are excluded:
     * they carry a user key in their blob and have no compile-time default, so
     * "equals default" is meaningless for them (and deleting would drop the
     * whole entry). */
    if (!zmk_custom_setting_keyspace_of(setting) && value_matches_default_locked(setting)) {
        if (setting_has_persistent_value(setting)) {
            ret = settings_delete(name);
            if (ret < 0) {
                return ret;
            }
        }
        set_setting_has_persistent_value(setting, false);
        set_setting_dirty(setting, false);
        return 0;
    }

    const void *data;
    size_t len;
    if (setting_uses_blob_store(setting)) {
        /* A BYTES/STRING payload is stored raw; persist it directly from
         * the blob store (any size). A pooled setting with no region (empty
         * value) has blob.data == NULL - settings_save_one(name, NULL, 0)
         * means "delete this record" to the settings subsystem, not "save
         * an empty value", so substitute a valid (never dereferenced past
         * its 0-byte length) pointer. */
        data = setting->state->blob.data != NULL ? setting->state->blob.data : (const void *)"";
        len = setting->state->blob.size;
    } else {
        const struct zmk_custom_setting_value *memory = memory_value_locked(setting);
        if (!memory) {
            return -EMSGSIZE;
        }
        ret = value_to_storage(memory, &data, &len);
        if (ret < 0) {
            return ret;
        }
    }

    ret = settings_save_one(name, data, len);
    if (ret < 0) {
        return ret;
    }

    set_setting_has_persistent_value(setting, true);
    set_setting_dirty(setting, false);
    return 0;
}

int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                            struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    if (zmk_custom_setting_keyspace_of(setting)) {
        return keyspace_read_payload(setting, value);
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index >= setting->array_state->size) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOENT;
    }

    const struct zmk_custom_setting_value *effective = effective_value(setting);
    if (!effective) {
        /* Large value that does not fit the fixed carrier - the caller must
         * use zmk_custom_setting_read_into (or, over RPC, the ordinary
         * streamed GetSetting/ListSettings response - see
         * custom_settings_handler.c) instead of the fixed-carrier read. */
        k_mutex_unlock(&custom_settings_lock);
        return -EMSGSIZE;
    }
    copy_value(value, effective);
    k_mutex_unlock(&custom_settings_lock);

    return 0;
}

int zmk_custom_setting_read_default(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    const struct zmk_custom_setting_value *def = setting_default_value(setting);
    if (!def) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOENT;
    }
    copy_value(value, def);
    k_mutex_unlock(&custom_settings_lock);

    return 0;
}

bool zmk_custom_setting_matches_default(const struct zmk_custom_setting *setting) {
    if (!setting) {
        return false;
    }

    /* value_matches_default_locked has no meaning for keyspace slots (no
     * compile-time default) or arrays (compared per whole array elsewhere), so
     * exclude them before asking - see its own contract comment. */
    if (zmk_custom_setting_keyspace_of(setting) || zmk_custom_setting_is_array(setting)) {
        return false;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    bool matches = value_matches_default_locked(setting);
    k_mutex_unlock(&custom_settings_lock);

    return matches;
}

int zmk_custom_setting_read_by_key(const char *custom_subsystem_id, const char *key,
                                   struct zmk_custom_setting_value *value) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find(custom_subsystem_id, key);
    if (!setting) {
        return -ENOENT;
    }

    return zmk_custom_setting_read(setting, value);
}

/* convert_rpc_bytes_value (the actual per-setting/per-keyspace converter
 * dispatch, defined in custom_settings_rpc_convert.c when
 * CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS is enabled) is only referenced
 * from inside an IS_ENABLED-guarded branch below, so when the feature is off
 * it is never called and custom_settings_rpc_convert.c need not be compiled
 * at all - it drops out of the image with the gate off. */
int zmk_custom_setting_serialize_rpc_value(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *internal_value,
                                           struct zmk_custom_setting_value *rpc_value) {
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS) && setting) {
        const struct zmk_custom_setting_keyspace *keyspace =
            zmk_custom_setting_keyspace_of(setting);
        return convert_rpc_bytes_value(setting, internal_value, rpc_value,
                                       keyspace ? keyspace->rpc_serializer
                                                : setting->rpc_serializer);
    }
    if (!setting || !internal_value || !rpc_value) {
        return -EINVAL;
    }
    copy_value(rpc_value, internal_value);
    return 0;
}

int zmk_custom_setting_deserialize_rpc_value(const struct zmk_custom_setting *setting,
                                             const struct zmk_custom_setting_value *rpc_value,
                                             struct zmk_custom_setting_value *internal_value) {
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS) && setting) {
        const struct zmk_custom_setting_keyspace *keyspace =
            zmk_custom_setting_keyspace_of(setting);
        return convert_rpc_bytes_value(setting, rpc_value, internal_value,
                                       keyspace ? keyspace->rpc_deserializer
                                                : setting->rpc_deserializer);
    }
    if (!setting || !rpc_value || !internal_value) {
        return -EINVAL;
    }
    copy_value(internal_value, rpc_value);
    return 0;
}

/* Store `value` as `setting`'s in-memory value, routing to the right
 * backing storage: array element carrier, blob store, or the inline scalar
 * state union. Caller must hold custom_settings_lock. */
static int store_memory_value_locked(const struct zmk_custom_setting *setting,
                                     const struct zmk_custom_setting_value *value) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        copy_value(&setting->array_state->values[setting->array_index], value);
        return 0;
    }

    return store_scalar_value_locked(setting, value);
}

/* Apply a write in the selected mode to an already-resolved, in-range
 * setting. Caller must hold custom_settings_lock. Non-static:
 * custom_settings_array.c's zmk_custom_setting_write_array_element calls
 * this too. */
int write_value_locked(const struct zmk_custom_setting *setting,
                       const struct zmk_custom_setting_value *value,
                       enum zmk_custom_setting_write_mode mode) {
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY: {
        const void *data;
        size_t size;
        int ret = value_to_storage(value, &data, &size);
        if (ret < 0) {
            return ret;
        }
        if (size > sizeof(temp_slots[0].data)) {
            return -EMSGSIZE;
        }

        int slot = temp_slot_alloc(setting);
        if (slot < 0) {
            return -EBUSY;
        }

        temp_slots[slot].type = value->type;
        temp_slots[slot].size = size;
        memcpy(temp_slots[slot].data, data, size);
        setting->state->temp_slot = slot;
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE, true);
        return 0;
    }
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY: {
        int ret = store_memory_value_locked(setting, value);
        if (ret < 0) {
            return ret;
        }
        set_setting_dirty(setting, true);
        clear_temporary_locked(setting);
        return 0;
    }
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST: {
        int ret = store_memory_value_locked(setting, value);
        if (ret < 0) {
            return ret;
        }
        clear_temporary_locked(setting);
        return save_setting_locked(setting);
    }
    default:
        return -EINVAL;
    }
}

/*
 * Keyspace-agnostic "write this exact typed value to this exact setting"
 * primitive. Used directly by the public zmk_custom_setting_write for a
 * normal setting, AND by write_bytes_raw's small-carrier tail (further down)
 * to store an already-fully-assembled keyspace slot blob verbatim - that
 * second caller must bypass the public zmk_custom_setting_write since `value`
 * there already IS the blob (typed BYTES), not a payload needing re-encoding.
 */
static int write_scalar_value(const struct zmk_custom_setting *setting,
                              const struct zmk_custom_setting_value *value,
                              enum zmk_custom_setting_write_mode mode) {
    int ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);

    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index >= setting->array_state->size) {
        ret = -ENOENT;
        goto unlock;
    }

    ret = write_value_locked(setting, value, mode);

unlock:
    k_mutex_unlock(&custom_settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_write(const struct zmk_custom_setting *setting,
                             const struct zmk_custom_setting_value *value,
                             enum zmk_custom_setting_write_mode mode) {
    if (!setting || !value) {
        return -EINVAL;
    }

    const struct zmk_custom_setting_keyspace *keyspace = zmk_custom_setting_keyspace_of(setting);
    if (keyspace) {
        /* `value` here is the PAYLOAD, typed as keyspace->value_type - not
         * the slot's own (always BYTES) blob representation. Validate it
         * against the keyspace's declared type/constraints/max_size, then
         * re-encode blob(current key, new payload) and write that via the
         * normal pooled-BYTES path (keyspace_write_blob's NULL `key` means
         * "reuse the slot's current key" - see the keyspace design comment
         * in the header). */
        int ret = keyspace_validate_payload(keyspace, value);
        if (ret < 0) {
            return ret;
        }
        return keyspace_write_blob(setting, NULL, value, mode);
    }

    return write_scalar_value(setting, value, mode);
}

int zmk_custom_setting_write_by_key(const char *custom_subsystem_id, const char *key,
                                    const struct zmk_custom_setting_value *value,
                                    enum zmk_custom_setting_write_mode mode) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find(custom_subsystem_id, key);
    if (!setting) {
        return -ENOENT;
    }

    return zmk_custom_setting_write(setting, value, mode);
}

int zmk_custom_setting_save(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    clear_temporary_locked(setting);
    int ret = save_setting_locked(setting);
    k_mutex_unlock(&custom_settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_SAVED);
    }

    return ret;
}

int zmk_custom_setting_discard(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);

    if (zmk_custom_setting_is_array(setting)) {
        /* Called directly on the array descriptor (array_index ==
         * ZMK_CUSTOM_SETTING_ARRAY_NONE): discard every element up to the
         * (about to be restored) persistent size. Called on a single
         * element view (from apply_scope's per-active-element expansion):
         * discard just that element. */
        if (setting->array_index == ZMK_CUSTOM_SETTING_ARRAY_NONE) {
            struct zmk_custom_setting_array_state *array_state = setting->array_state;
            uint32_t restore_size = array_state->persistent_size;
            for (uint32_t index = 0; index < restore_size; index++) {
                struct zmk_custom_setting *view = array_view_acquire(setting, index);
                discard_array_element_locked(view);
            }
            set_array_memory_size_locked(setting, restore_size);
        } else {
            discard_array_element_locked(setting);
        }
        k_mutex_unlock(&custom_settings_lock);
        raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_DISCARDED);
        return 0;
    }

    if (setting_has_persistent_value(setting)) {
        /* No RAM-resident persistent_value copy is kept; re-read the
         * persisted value from flash straight into the memory store.
         * Discard is a rare, explicit user action, so a flash read here is
         * fine. */
        char name[SETTINGS_MAX_NAME_LEN];
        int ret = setting_storage_name(setting, name, sizeof(name));
        if (ret == 0) {
            ret = settings_load_subtree(name);
        }
        if (ret < 0) {
            apply_scalar_default_locked(setting);
        }
    } else {
        apply_scalar_default_locked(setting);
    }
    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY, false);
    clear_temporary_locked(setting);
    k_mutex_unlock(&custom_settings_lock);

    raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_DISCARDED);
    return 0;
}

int zmk_custom_setting_reset(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    if (zmk_custom_setting_is_array(setting)) {
        k_mutex_lock(&custom_settings_lock, K_FOREVER);
        struct zmk_custom_setting_array_state *array_state = setting->array_state;
        int ret = 0;

        if (setting->array_index == ZMK_CUSTOM_SETTING_ARRAY_NONE) {
            /* Whole-array reset: restore every element up to the current
             * active size to its default and erase all persisted state,
             * including the "_size" marker. */
            uint32_t reset_size = array_state->size;
            for (uint32_t index = 0; index < reset_size && ret == 0; index++) {
                struct zmk_custom_setting *view = array_view_acquire(setting, index);
                ret = reset_array_element_locked(view);
            }
            if (ret == 0) {
                char size_name[SETTINGS_MAX_NAME_LEN];
                ret = array_size_storage_name(setting, size_name, sizeof(size_name));
                if (ret == 0) {
                    ret = settings_delete(size_name);
                    if (ret == -ENOENT) {
                        ret = 0;
                    }
                }
            }
            if (ret == 0) {
                set_array_persistent_size_locked(setting, array_state->default_size);
                set_array_memory_size_locked(setting, array_state->default_size);
            }
        } else {
            /* Single active-element reset, as invoked once per element by
             * apply_scope. */
            ret = reset_array_element_locked(setting);
            if (ret == 0) {
                char size_name[SETTINGS_MAX_NAME_LEN];
                int size_ret = array_size_storage_name(setting, size_name, sizeof(size_name));
                if (size_ret == 0) {
                    size_ret = settings_delete(size_name);
                    if (size_ret != 0 && size_ret != -ENOENT) {
                        ret = size_ret;
                    }
                } else {
                    ret = size_ret;
                }
                if (ret == 0) {
                    set_array_persistent_size_locked(setting, array_state->default_size);
                    set_array_memory_size_locked(setting, array_state->default_size);
                }
            }
        }

        k_mutex_unlock(&custom_settings_lock);
        if (ret == 0) {
            raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_RESET);
        }
        return ret;
    }

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(setting, name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    ret = settings_delete(name);
    if (ret == -ENOENT) {
        ret = 0;
    }

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
    struct zmk_custom_setting_keyspace *keyspace =
        (struct zmk_custom_setting_keyspace *)zmk_custom_setting_keyspace_of(setting);
    if (ret == 0 && keyspace) {
        /* A keyspace entry is created at runtime and carries its user key
         * inside its own blob, so it has no compile-time default to revert to:
         * resetting it means the entry ceases to exist. Release the slot and
         * free its pool region (exactly what zmk_custom_setting_keyspace_delete
         * does) rather than deleting only the flash record. Without this, a
         * reset that reaches keyspace slots - notably ZMK's official factory
         * reset, which walks every live slot - would erase the record but leave
         * the slot in_use and its pool bytes occupied until reboot, so a
         * pool-backed namespace like zmk-feature-runtime-macro's would survive
         * the reset in RAM. Deletion is silent to the change event the same way
         * keyspace_delete is: there is no "deleted" notification kind, and a
         * RESET notification would wrongly imply the entry still exists with a
         * default value. */
        keyspace_release_slot_for_setting_locked(keyspace, setting);
        k_mutex_unlock(&custom_settings_lock);
        return 0;
    }
#endif

    if (ret == 0) {
        apply_scalar_default_locked(setting);
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT, false);
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY, false);
        clear_temporary_locked(setting);
    }
    k_mutex_unlock(&custom_settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_RESET);
    }

    return ret;
}

int zmk_custom_setting_rollback_temporary(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    clear_temporary_locked(setting);
    k_mutex_unlock(&custom_settings_lock);

    raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    return 0;
}

/* Apply callback to one setting, expanding an array descriptor into one
 * callback invocation per currently-active element (via the index-view
 * mechanism) instead of a single call on the descriptor. Each element
 * contributes its own count to *affected_count - e.g.
 * zmk_custom_settings_reset_scope("test", "array_value", NULL,
 * &affected_count) reports exactly 3 for a 3-element active array, not 1 for
 * "one array descriptor". Internally this means save/discard/reset each run
 * their whole-array bookkeeping update redundantly once per element, which is
 * harmless and still O(active count), not O(full registry). */
static int apply_scope_to_setting(const struct zmk_custom_setting *setting,
                                  int (*callback)(const struct zmk_custom_setting *),
                                  uint32_t *count, int *first_error) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index == ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        k_mutex_lock(&custom_settings_lock, K_FOREVER);
        uint32_t active_size = setting->array_state->size;
        k_mutex_unlock(&custom_settings_lock);

        for (uint32_t index = 0; index < active_size; index++) {
            const struct zmk_custom_setting *view = zmk_custom_setting_find_array_element(
                setting->custom_subsystem_id, setting->array_key, index);
            if (!view) {
                continue;
            }

            int ret = callback(view);
            if (ret < 0 && *first_error == 0) {
                *first_error = ret;
                continue;
            }
            if (ret == 0) {
                (*count)++;
            }
        }
        return 0;
    }

    int ret = callback(setting);
    if (ret < 0 && *first_error == 0) {
        *first_error = ret;
    } else if (ret == 0) {
        (*count)++;
    }
    return 0;
}

static int apply_scope(const char *custom_subsystem_id, const char *key, const char *key_prefix,
                       uint32_t *affected_count,
                       int (*callback)(const struct zmk_custom_setting *)) {
    uint32_t count = 0;
    int first_error = 0;
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!zmk_custom_setting_matches(setting, custom_subsystem_id, key, key_prefix)) {
            continue;
        }

        apply_scope_to_setting(setting, callback, &count, &first_error);
    }

    /* Keyspace slots are not reachable through ZMK_CUSTOM_SETTING_FOREACH,
     * so walk every keyspace's live slots explicitly to keep save/discard/
     * reset scope reaching user-created entries. A slot is never an array, so
     * no expansion step is needed the way apply_scope_to_setting needs one for
     * arrays. IS_ENABLED-guarded so a CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=n
     * build never walks keyspace slot state that only
     * custom_settings_keyspace.c knows how to keep consistent. */
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)) {
        ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(keyspace) {
            for (uint32_t i = 0; i < keyspace->max_entries; i++) {
                k_mutex_lock(&custom_settings_lock, K_FOREVER);
                bool in_use = keyspace->slots[i].in_use;
                k_mutex_unlock(&custom_settings_lock);
                if (!in_use) {
                    continue;
                }

                const struct zmk_custom_setting *slot_setting = &keyspace->slots[i].setting;
                if (!zmk_custom_setting_matches(slot_setting, custom_subsystem_id, key,
                                                key_prefix)) {
                    continue;
                }

                apply_scope_to_setting(slot_setting, callback, &count, &first_error);
            }
        }
    }

    if (affected_count) {
        *affected_count = count;
    }

    return first_error;
}

int zmk_custom_settings_save_scope(const char *custom_subsystem_id, const char *key,
                                   const char *key_prefix, uint32_t *affected_count) {
    return apply_scope(custom_subsystem_id, key, key_prefix, affected_count,
                       zmk_custom_setting_save);
}

int zmk_custom_settings_discard_scope(const char *custom_subsystem_id, const char *key,
                                      const char *key_prefix, uint32_t *affected_count) {
    return apply_scope(custom_subsystem_id, key, key_prefix, affected_count,
                       zmk_custom_setting_discard);
}

int zmk_custom_settings_reset_scope(const char *custom_subsystem_id, const char *key,
                                    const char *key_prefix, uint32_t *affected_count) {
    return apply_scope(custom_subsystem_id, key, key_prefix, affected_count,
                       zmk_custom_setting_reset);
}

bool zmk_custom_setting_has_unsaved_value(const struct zmk_custom_setting *setting) {
    if (!setting) {
        return false;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    bool has_unsaved = setting_temporary_active(setting) || setting_is_dirty(setting);
    if (zmk_custom_setting_is_array(setting)) {
        has_unsaved =
            has_unsaved || setting->array_state->size != setting->array_state->persistent_size;
    }
    k_mutex_unlock(&custom_settings_lock);

    return has_unsaved;
}

int zmk_custom_setting_with_value(const struct zmk_custom_setting *setting,
                                  zmk_custom_setting_value_visitor_t visitor, void *user_data) {
    if (!setting || !visitor) {
        return -EINVAL;
    }

    if (zmk_custom_setting_keyspace_of(setting)) {
        /* Present the decoded PAYLOAD, not the raw [key\0][payload] blob.
         * Materialized into a stack carrier (one copy) rather than borrowed
         * zero-copy - acceptable for the rarely-hot visitor API; large
         * payloads still return -EMSGSIZE here like any other >carrier
         * value. */
        struct zmk_custom_setting_value payload;
        int ret = keyspace_read_payload(setting, &payload);
        if (ret < 0) {
            return ret;
        }
        visitor(&payload, user_data);
        return 0;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index >= setting->array_state->size) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOENT;
    }

    const struct zmk_custom_setting_value *effective = effective_value(setting);
    if (!effective) {
        /* Large value that does not fit the fixed carrier - callers wanting
         * the raw bytes use zmk_custom_setting_read_into (which reads the
         * large store directly) instead. */
        k_mutex_unlock(&custom_settings_lock);
        return -EMSGSIZE;
    }
    visitor(effective, user_data);
    k_mutex_unlock(&custom_settings_lock);

    return 0;
}

/* struct read_into_context is declared in custom_settings_internal.h:
 * custom_settings_keyspace.c's keyspace_read_into shares this same
 * visitor/context pairing for its carrier-sized fallback path. */
void read_into_visitor(const struct zmk_custom_setting_value *value, void *user_data) {
    struct read_into_context *ctx = user_data;

    const void *data;
    size_t size;
    switch (value->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        data = value->bytes_value;
        size = value->size;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        data = value->string_value;
        size = value->size;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        data = &value->int32_value;
        size = sizeof(value->int32_value);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        data = &value->bool_value;
        size = sizeof(value->bool_value);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        data = &value->behavior_value;
        size = sizeof(value->behavior_value);
        break;
    default:
        ctx->ret = -EINVAL;
        return;
    }

    if (size > ctx->capacity) {
        ctx->ret = -EMSGSIZE;
        return;
    }

    memcpy(ctx->buf, data, size);
    ctx->out_size = size;
    ctx->out_type = value->type;
    ctx->ret = 0;
}

int zmk_custom_setting_read_into(const struct zmk_custom_setting *setting, void *buf,
                                 size_t capacity, size_t *out_size,
                                 enum zmk_custom_setting_value_type *out_type) {
    if (!setting || (!buf && capacity > 0)) {
        return -EINVAL;
    }

    if (zmk_custom_setting_keyspace_of(setting)) {
        return keyspace_read_into(setting, buf, capacity, out_size, out_type);
    }

    /* Blob fast path: read the raw payload straight from the setting's
     * store, so a value larger than the fixed carrier is still readable (the
     * effective_value carrier below cannot represent it). Skipped while a
     * temporary override is active - those are always carrier-sized. */
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (setting_uses_blob_store(setting) && !setting_temporary_active(setting)) {
        size_t size = setting->state->blob.size;
        int ret = 0;
        if (size > capacity) {
            ret = -EMSGSIZE;
        } else {
            /* A pooled setting with no region yet (blob.data == NULL) has
             * size == 0 here; skip the copy rather than pass a NULL source
             * to memcpy. */
            if (size > 0) {
                memcpy(buf, setting->state->blob.data, size);
            }
            if (out_size) {
                *out_size = size;
            }
            if (out_type) {
                *out_type = setting->value_type;
            }
        }
        k_mutex_unlock(&custom_settings_lock);
        return ret;
    }
    k_mutex_unlock(&custom_settings_lock);

    struct read_into_context ctx = {.buf = buf, .capacity = capacity, .ret = -EIO};
    int ret = zmk_custom_setting_with_value(setting, read_into_visitor, &ctx);
    if (ret < 0) {
        return ret;
    }
    if (ctx.ret < 0) {
        return ctx.ret;
    }

    if (out_size) {
        *out_size = ctx.out_size;
    }
    if (out_type) {
        *out_type = ctx.out_type;
    }
    return 0;
}

int zmk_custom_setting_value_size(const struct zmk_custom_setting *setting, size_t *out_size) {
    if (!setting || !out_size) {
        return -EINVAL;
    }

    const struct zmk_custom_setting_keyspace *keyspace = zmk_custom_setting_keyspace_of(setting);
    enum zmk_custom_setting_value_type presented_type =
        keyspace ? keyspace->value_type : setting->value_type;
    if (presented_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
        presented_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        return -EINVAL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (setting_uses_blob_store(setting) && !setting_temporary_active(setting)) {
        size_t blob_size = setting->state->blob.size;
        size_t key_len = keyspace ? keyspace_blob_key_len_locked(setting) : 0;
        *out_size = blob_size > key_len ? blob_size - (key_len ? key_len + 1 : 0) : 0;
        k_mutex_unlock(&custom_settings_lock);
        return 0;
    }
    k_mutex_unlock(&custom_settings_lock);

    struct zmk_custom_setting_value value;
    int ret = keyspace ? keyspace_read_payload(setting, &value)
                       : zmk_custom_setting_read(setting, &value);
    if (ret < 0) {
        /* Shouldn't happen: the large-store branch above already handles any
         * setting whose value could exceed the carrier. */
        return ret;
    }
    *out_size = value.size;
    return 0;
}

/*
 * Keyspace-agnostic "write this exact raw payload as the setting's stored
 * value" primitive. Used directly by the public zmk_custom_setting_write_bytes
 * for a normal setting, AND (via its small-carrier tail calling
 * write_scalar_value, not zmk_custom_setting_write) to store an
 * already-assembled keyspace slot blob verbatim - see keyspace_write_blob/
 * keyspace_write_raw_payload further down, which build that blob and call
 * this function directly to avoid re-triggering the keyspace interception in
 * the public dispatchers.
 */
int write_bytes_raw(const struct zmk_custom_setting *setting, const void *data, size_t size,
                    enum zmk_custom_setting_write_mode mode) {
    /* Blob values exceeding the fixed carrier take a dedicated raw path
     * (the carrier below cannot hold them). Values that still fit the
     * carrier fall through to the normal validated path so constraints keep
     * being enforced. */
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES) && setting_uses_blob_store(setting) &&
        size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        if (size > setting_capacity(setting)) {
            return -EMSGSIZE;
        }

        k_mutex_lock(&custom_settings_lock, K_FOREVER);
        int ret = write_large_locked(setting, data, size, mode);
        k_mutex_unlock(&custom_settings_lock);

        if (ret == 0) {
            raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                               ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                               : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
        }
        return ret;
    }

    struct zmk_custom_setting_value value = {.type = setting->value_type};
    switch (setting->value_type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        if (size > sizeof(value.bytes_value)) {
            return -EMSGSIZE;
        }
        value.size = size;
        memcpy(value.bytes_value, data, size);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING: {
        /* Reject rather than silently truncate when the value cannot fit the
         * fixed carrier (mirrors the BYTES guard above). A larger value must
         * be written to a large-capable setting via the raw path. */
        if (size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        memcpy(value.string_value, data, size);
        value.string_value[size] = '\0';
        value.size = size;
        break;
    }
    default:
        return -EINVAL;
    }

    return write_scalar_value(setting, &value, mode);
}

int zmk_custom_setting_write_bytes(const struct zmk_custom_setting *setting, const void *data,
                                   size_t size, enum zmk_custom_setting_write_mode mode) {
    if (!setting || (!data && size > 0)) {
        return -EINVAL;
    }

    if (zmk_custom_setting_keyspace_of(setting)) {
        return keyspace_write_raw_payload(setting, data, size, mode);
    }

    return write_bytes_raw(setting, data, size, mode);
}

int zmk_custom_setting_get_int32(const struct zmk_custom_setting *setting, int32_t *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    enum zmk_custom_setting_value_type type;
    int ret = zmk_custom_setting_read_into(setting, value, sizeof(*value), NULL, &type);
    if (ret < 0) {
        return ret;
    }

    return type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 ? 0 : -EINVAL;
}

int zmk_custom_setting_set_int32(const struct zmk_custom_setting *setting, int32_t value,
                                 enum zmk_custom_setting_write_mode mode) {
    if (!setting) {
        return -EINVAL;
    }

    return zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(value), mode);
}

int zmk_custom_setting_get_bool(const struct zmk_custom_setting *setting, bool *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    enum zmk_custom_setting_value_type type;
    int ret = zmk_custom_setting_read_into(setting, value, sizeof(*value), NULL, &type);
    if (ret < 0) {
        return ret;
    }

    return type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL ? 0 : -EINVAL;
}

int zmk_custom_setting_set_bool(const struct zmk_custom_setting *setting, bool value,
                                enum zmk_custom_setting_write_mode mode) {
    if (!setting) {
        return -EINVAL;
    }

    return zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_BOOL(value), mode);
}

int zmk_custom_setting_get_behavior(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_behavior_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    enum zmk_custom_setting_value_type type;
    int ret = zmk_custom_setting_read_into(setting, value, sizeof(*value), NULL, &type);
    if (ret < 0) {
        return ret;
    }

    return type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR ? 0 : -EINVAL;
}

int zmk_custom_setting_set_behavior(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_behavior_value value,
                                    enum zmk_custom_setting_write_mode mode) {
    if (!setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value setting_value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
        .behavior_value = value,
    };
    return zmk_custom_setting_write(setting, &setting_value, mode);
}

static int value_from_storage(const struct zmk_custom_setting *setting, const void *data,
                              size_t len) {
    /* A blob (BYTES/STRING) payload larger than the fixed carrier loads
     * directly into the blob store, bypassing the carrier (which cannot
     * hold it). Constraints for these types are size-only, already enforced
     * by the capacity check. */
    if (setting_uses_blob_store(setting) && len > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        if (len > setting_capacity(setting)) {
            return -EMSGSIZE;
        }
        int ret = blob_store_set_raw(setting, data, len);
        if (ret < 0) {
            /* Pool exhausted (e.g. the pool shrank across a firmware update,
             * or this boot's other pool members already claim the rest of
             * the budget) - skip this persisted value rather than fail boot,
             * mirroring the keyspace-pool-exhaustion policy. */
            LOG_WRN("Custom settings pool exhausted; skipping persisted value for %s/%s",
                    setting->custom_subsystem_id, setting->key);
            return ret;
        }
        set_setting_has_persistent_value(setting, true);
        set_setting_dirty(setting, false);
        clear_temporary_locked(setting);
        return 0;
    }

    struct zmk_custom_setting_value value = {.type = setting->value_type};

    switch (setting->value_type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        if (len > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        value.size = len;
        memcpy(value.bytes_value, data, len);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        if (len != sizeof(value.int32_value)) {
            return -EINVAL;
        }
        memcpy(&value.int32_value, data, sizeof(value.int32_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        if (len != sizeof(value.bool_value)) {
            return -EINVAL;
        }
        memcpy(&value.bool_value, data, sizeof(value.bool_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        if (len > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        value.size = len;
        memcpy(value.string_value, data, len);
        value.string_value[len] = '\0';
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR: {
        int ret = decode_behavior_value(data, len, &value.behavior_value);
        if (ret < 0) {
            return ret;
        }
        break;
    }
    default:
        return -EINVAL;
    }

    int ret = zmk_custom_setting_validate(setting, &value);
    if (ret < 0) {
        return ret;
    }

    ret = store_memory_value_locked(setting, &value);
    if (ret < 0) {
        LOG_WRN("Custom settings pool exhausted; skipping persisted value for %s/%s",
                setting->custom_subsystem_id, setting->key);
        return ret;
    }
    set_setting_has_persistent_value(setting, true);
    set_setting_dirty(setting, false);
    clear_temporary_locked(setting);
    return 0;
}

/* Length of the user-key portion of a keyspace slot's blob (bytes before the
 * embedded NUL separator - see the `[user_key\0][payload]` blob format doc
 * on struct zmk_custom_setting_keyspace in the header), or 0 if the slot has
 * no blob yet (blob.data == NULL - only possible transiently between
 * keyspace_bind_slot_locked binding a slot and the settings-load callback
 * applying its persisted bytes moments later, or between
 * zmk_custom_setting_keyspace_create claiming a slot and writing its first
 * value). Caller holds custom_settings_lock (the blob can move on pool
 * compaction).
 *
 * Stays here in core (not in custom_settings_keyspace.c) even though it is
 * only meaningful for a keyspace slot: custom_settings_pool.c's
 * zmk_custom_setting_with_large_raw_bytes calls it from a branch keyed on the
 * runtime field setting->_keyspace, which the compiler cannot prove dead
 * (unlike the IS_ENABLED-gated zmk_custom_setting_keyspace_of() calls
 * elsewhere), so this symbol must exist even in a
 * CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=n build. */
size_t keyspace_blob_key_len_locked(const struct zmk_custom_setting *setting) {
    const struct zmk_custom_setting_state *state = setting->state;
    if (!state->blob.data || state->blob.size == 0) {
        return 0;
    }
    const uint8_t *nul = memchr(state->blob.data, '\0', state->blob.size);
    return nul ? (size_t)(nul - state->blob.data) : state->blob.size;
}

/* The opaque-blob keyspace presentation/lookup layer
 * (zmk_custom_setting_keyspace_create/delete/find,
 * keyspace_read_payload/read_into/write_blob/write_raw_payload/
 * bind_slot_locked/release_slot_locked/parse_ordinal_name, ...) lives in
 * src/custom_settings_keyspace.c (CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE, which
 * selects CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES - a keyspace slot is a
 * pool-backed blob). Core call sites guard their use of it with
 * zmk_custom_setting_keyspace_of() (folds to a compile-time NULL when the
 * feature is off), except zmk_custom_setting_find's keyspace-lookup fallback
 * (no live setting to derive keyspace_of() from - guarded by IS_ENABLED
 * directly) and the settings-load ordinal-name re-bind branch below (same
 * reason). */

static int custom_settings_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                      void *cb_arg) {
    const char *next;
    char custom_subsystem_id[CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN];

    int match_len = settings_name_next(name, &next);
    if (match_len <= 0 || match_len >= sizeof(custom_subsystem_id) || !next) {
        return -ENOENT;
    }

    memcpy(custom_subsystem_id, name, match_len);
    custom_subsystem_id[match_len] = '\0';

    char array_key[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) &&
        split_array_size_key(next, array_key, sizeof(array_key))) {
        struct zmk_custom_setting *setting =
            (struct zmk_custom_setting *)zmk_custom_setting_find_array_element(custom_subsystem_id,
                                                                               array_key, 0);
        if (!setting) {
            return -ENOENT;
        }

        uint32_t array_size;
        if (len != sizeof(array_size)) {
            return -EINVAL;
        }

        ssize_t read = read_cb(cb_arg, &array_size, sizeof(array_size));
        if (read < 0) {
            return read;
        }

        k_mutex_lock(&custom_settings_lock, K_FOREVER);
        int ret = array_size_from_storage(setting, &array_size, read);
        k_mutex_unlock(&custom_settings_lock);
        return ret;
    }

    struct zmk_custom_setting *setting;
    uint32_t element_index;
    if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) &&
        split_array_element_key(next, array_key, sizeof(array_key), &element_index)) {
        /* Array elements are not individually registered (see
         * ZMK_CUSTOM_SETTING_ARRAY_DEFINE), so a stored "array_key/index"
         * name is resolved through the index-view mechanism instead of a
         * direct registry lookup by key. */
        setting = (struct zmk_custom_setting *)zmk_custom_setting_find_array_element(
            custom_subsystem_id, array_key, element_index);
    } else {
        setting = (struct zmk_custom_setting *)zmk_custom_setting_find(custom_subsystem_id, next);
        if (!setting) {
            /* `next` may be a keyspace slot's stable ordinal storage name,
             * "<key_prefix>#<index>" (e.g. "macro/#3"),
             * for a slot that has not been (re)bound yet this boot - e.g. a
             * fresh boot loading a value persisted by a CreateSetting RPC in
             * a previous session, with no application code creating the
             * entry again. Bind slot `index` directly here (no key needed
             * yet - it lives inside the blob about to be loaded) and fall
             * through to apply this record's value to the freshly bound
             * descriptor exactly like any other setting - this is the
             * "re-bound during settings load" requirement: no module code
             * has to run for user-created entries to survive reboot.
             * IS_ENABLED-guarded (not zmk_custom_setting_keyspace_of, which
             * needs a live setting - there is none yet here) so a
             * CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=n build never references
             * custom_settings_keyspace.c's keyspace_parse_ordinal_name/
             * keyspace_bind_slot_locked. */
            if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)) {
                k_mutex_lock(&custom_settings_lock, K_FOREVER);
                ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(keyspace) {
                    if (strncmp(keyspace->custom_subsystem_id, custom_subsystem_id,
                                CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
                        continue;
                    }
                    uint32_t index;
                    if (!keyspace_parse_ordinal_name(keyspace, next, &index)) {
                        continue;
                    }
                    if (index >= keyspace->max_entries) {
                        LOG_WRN("Custom settings keyspace record %s/%s has out-of-range ordinal "
                                "index %u (max_entries=%u) - dropping",
                                custom_subsystem_id, next, index, keyspace->max_entries);
                        continue;
                    }
                    if (keyspace->slots[index].in_use) {
                        /* Already bound this boot (e.g. a targeted
                         * settings_load_subtree from zmk_custom_setting_discard,
                         * or a repeated settings_load pass): apply the record to
                         * the LIVE slot rather than re-binding it -
                         * keyspace_bind_slot_locked resets the descriptor
                         * (blob.data/_pool_next), which would corrupt the
                         * pool's member list for a slot that already holds a
                         * region. */
                        setting = &keyspace->slots[index].setting;
                    } else {
                        setting = keyspace_bind_slot_locked(keyspace, index);
                    }
                    break;
                }
                k_mutex_unlock(&custom_settings_lock);
            }
        }
    }
    if (!setting) {
        return -ENOENT;
    }

    /* Sized to the largest per-setting value so a large-store setting's
     * persisted payload can be staged on load, not just the fixed carrier -
     * a keyspace slot's blob additionally carries its user key alongside
     * the payload, so this is wider than CONFIG_ZMK_CUSTOM_SETTINGS_
     * LARGE_VALUE_MAX_SIZE alone to fit that too (see
     * ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE, which every
     * registered keyspace's own max_key_len/max_size are bounded against). */
    uint8_t data[ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE];
    if (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        len = sizeof(int32_t);
    } else if (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        len = sizeof(bool);
    }

    if (len > sizeof(data)) {
        return -EMSGSIZE;
    }

    ssize_t read = read_cb(cb_arg, data, len);
    if (read < 0) {
        return read;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int ret = value_from_storage(setting, data, read);
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}

/* Raise the one-shot zmk_custom_settings_initialized event the first time the
 * "custom_settings" subtree finishes a settings_load pass. settings_commit for
 * this subtree runs at the end of every settings_load()/settings_load_subtree()
 * covering it: the boot load (where every persisted value has just been applied
 * by custom_settings_handle_set), and each later reload that
 * zmk_custom_setting_discard triggers to re-read one setting from flash. The
 * guard makes sure consumers get a single "settings are ready" signal at boot
 * and that the post-boot discard reloads - which run while holding
 * custom_settings_lock - never re-raise it (both avoiding a spurious event and
 * keeping the raise off the locked discard path). The first commit is always
 * the boot load: discard can only be reached from a Studio RPC, long after the
 * boot settings_load has run. */
static bool custom_settings_initialized;

static int custom_settings_handle_commit(void) {
    if (custom_settings_initialized) {
        return 0;
    }
    custom_settings_initialized = true;
    raise_zmk_custom_settings_initialized((struct zmk_custom_settings_initialized){0});
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(custom_settings, SETTINGS_SUBTREE, NULL, custom_settings_handle_set,
                               custom_settings_handle_commit, NULL);

/* Reset one setting's mutable state to its just-registered, nothing-loaded-
 * yet state: defaults copied in, no persistent/dirty/temporary flags set.
 * Shared by custom_settings_init() (every compile-time setting, at boot)
 * and custom_settings_keyspace.c's keyspace_bind_slot_locked() (one keyspace
 * slot, (re)bound at any point - boot-time settings-load or a later
 * CreateSetting RPC) so the two initialization paths cannot drift apart.
 * Non-static: declared in custom_settings_internal.h for that cross-file
 * call. Caller must hold custom_settings_lock if called after boot
 * (custom_settings_init runs before any other thread can contend for it, so
 * it does not bother). */
void init_setting_state_locked(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting)) {
        /* One call covers the whole array (there is exactly one registered
         * descriptor per array - see ZMK_CUSTOM_SETTING_ARRAY_DEFINE):
         * initialize every element slot from its own per-index default
         * instead of walking "sibling" registrations. */
        struct zmk_custom_setting_array_state *array_state = setting->array_state;
        for (uint32_t index = 0; index < array_state->max_size; index++) {
            copy_value(&array_state->values[index], &array_state->defaults[index]);
            array_state->has_persistent[index] = false;
            array_state->dirty[index] = false;
        }
        array_state->persistent_size = array_state->default_size;
        array_state->size = array_state->default_size;
    } else {
        /* Deliberately does NOT clear state->default_override or
         * state->blob.data.
         * - default_override: zmk_custom_setting_set_default() may legally
         *   run from a SYS_INIT ordered BEFORE this module's own init;
         *   apply_scalar_default_locked() below reads through
         *   setting_default_value(), so an already-installed override is
         *   both preserved and applied. (Keyspace binds reset the whole
         *   embedded state block beforehand - see
         *   keyspace_bind_slot_locked.)
         * - blob.data: for a fixed-store setting it points permanently at
         *   the macro-emitted buffer. */
        apply_scalar_default_locked(setting);
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT, false);
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY, false);
    }
    setting->state->temp_slot = -1;
    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE, false);
    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_INITIALIZED, true);
}

static int custom_settings_init(void) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) { init_setting_state_locked(setting); }

    return 0;
}

SYS_INIT(custom_settings_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
