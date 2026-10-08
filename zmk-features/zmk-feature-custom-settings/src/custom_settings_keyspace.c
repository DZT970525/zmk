/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * RPC-creatable keyspaces / namespaces (CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE,
 * which selects CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES). A keyspace slot's
 * entire entry - its user key *and* its payload - is one opaque pool-backed
 * BYTES blob, so this feature cannot link without the pool
 * (src/custom_settings_pool.c).
 *
 * This file is the only keyspace-aware code in the module: the
 * presentation/lookup layer. Everything else (storage, pool, generic value
 * read/write/save) treats a slot as a plain pooled BYTES setting. Core call
 * sites guard their use of this file's entry points with
 * zmk_custom_setting_keyspace_of(), which folds to a compile-time constant
 * NULL when the feature is off, so none of these functions are reachable in a
 * CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=n build.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <cormoran/zmk/custom_settings.h>

#include "custom_settings_internal.h"

static bool keyspace_key_matches_prefix(const struct zmk_custom_setting_keyspace *keyspace,
                                        const char *key) {
    size_t prefix_len = strlen(keyspace->key_prefix);
    return strncmp(key, keyspace->key_prefix, prefix_len) == 0;
}

/* Find a keyspace registered for custom_subsystem_id whose key_prefix
 * matches the start of `key`. Caller must hold custom_settings_lock.
 *
 * A NULL or empty custom_subsystem_id means "match any subsystem", exactly
 * as zmk_custom_setting_find() treats it: an RPC caller that addresses a
 * keyspace entry by key alone - a SettingRef with no custom_subsystem_index,
 * which is how the runtime-macro web UI issues CreateSetting/DeleteSetting -
 * lands here with a NULL id and must still resolve the keyspace by its key
 * prefix. Without this the create/delete handlers reported "No keyspace
 * registered for this key" for every such request. */
static struct zmk_custom_setting_keyspace *
keyspace_find_for_key_locked(const char *custom_subsystem_id, const char *key) {
    ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(keyspace) {
        if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
            strncmp(keyspace->custom_subsystem_id, custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
            continue;
        }
        if (keyspace_key_matches_prefix(keyspace, key)) {
            return keyspace;
        }
    }

    return NULL;
}

struct zmk_custom_setting_keyspace *
zmk_custom_settings_keyspace_find_for_key(const char *custom_subsystem_id, const char *key) {
    if (!key) {
        return NULL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    struct zmk_custom_setting_keyspace *keyspace =
        keyspace_find_for_key_locked(custom_subsystem_id, key);
    k_mutex_unlock(&custom_settings_lock);

    return keyspace;
}

/* Scratch destination for a keyspace slot's decoded user key. Safe as a
 * single shared instance: populated and consumed synchronously by the caller
 * under the lock, not valid past the immediate call. */
static char keyspace_public_key_scratch[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];

const char *keyspace_public_key_locked(const struct zmk_custom_setting *setting) {
    size_t key_len = keyspace_blob_key_len_locked(setting);
    key_len = MIN(key_len, sizeof(keyspace_public_key_scratch) - 1);
    if (setting->state->blob.data != NULL && key_len > 0) {
        memcpy(keyspace_public_key_scratch, setting->state->blob.data, key_len);
    }
    keyspace_public_key_scratch[key_len] = '\0';
    return keyspace_public_key_scratch;
}

/* Find the slot index currently bound to `key`, decoding each live slot's
 * blob to compare (a short walk over the live slots, not a scan of raw pool
 * bytes). Caller holds custom_settings_lock. */
static int keyspace_slot_index_for_key_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              const char *key) {
    size_t key_len = strlen(key);
    for (uint32_t i = 0; i < keyspace->max_entries; i++) {
        if (!keyspace->slots[i].in_use) {
            continue;
        }
        const struct zmk_custom_setting *slot_setting = &keyspace->slots[i].setting;
        size_t blob_key_len = keyspace_blob_key_len_locked(slot_setting);
        if (blob_key_len == key_len && slot_setting->state->blob.data != NULL &&
            memcmp(slot_setting->state->blob.data, key, key_len) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int keyspace_free_slot_index_locked(struct zmk_custom_setting_keyspace *keyspace) {
    for (uint32_t i = 0; i < keyspace->max_entries; i++) {
        if (!keyspace->slots[i].in_use) {
            return (int)i;
        }
    }

    return -1;
}

/* Bind slot `index` of `keyspace` to its stable ordinal storage identity
 * ("<key_prefix>#<index>") and wire it into the keyspace's shared pool, with
 * an EMPTY blob (blob.data == NULL) - the caller populates it immediately
 * after, either via the settings-load value-apply step (a persisted record)
 * or keyspace_write_blob (a fresh create). Caller holds custom_settings_lock.
 * Never fails: the ordinal name buffer is sized for any key_prefix +
 * max_entries this module's BUILD_ASSERTs allow. */
struct zmk_custom_setting *keyspace_bind_slot_locked(struct zmk_custom_setting_keyspace *keyspace,
                                                     uint32_t index) {
    struct zmk_custom_setting_keyspace_slot *slot = &keyspace->slots[index];

    snprintf(slot->ordinal_name, sizeof(slot->ordinal_name), "%s#%u", keyspace->key_prefix, index);

    /* Reset the slot's embedded state block first: a freshly bound slot
     * starts with an empty blob (data == NULL - its region is carved from the
     * pool on the first write/load), no flags, no temp slot, and no default
     * override left over from a previous occupant. */
    slot->state = (struct zmk_custom_setting_state){.temp_slot = -1};
    slot->setting = (struct zmk_custom_setting){
        .custom_subsystem_id = keyspace->custom_subsystem_id,
        .key = slot->ordinal_name,
        .array_key = NULL,
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
        .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .confidentiality = keyspace->confidentiality,
        .read_permission = keyspace->read_permission,
        .write_permission = keyspace->write_permission,
        /* Deliberately NOT keyspace->constraints/rpc_*: those describe the
         * PAYLOAD, not the slot's own opaque BYTES blob, which has no
         * constraints of its own. */
        .constraints = NULL,
        .constraints_count = 0,
        .default_value = NULL,
        .rpc_serializer = NULL,
        .rpc_deserializer = NULL,
        .blob = {.max_size = keyspace->max_key_len + keyspace->max_size,
                 .pool = keyspace->large_pool},
        ._keyspace = keyspace,
        .state = &slot->state,
    };
    slot->in_use = true;
    init_setting_state_locked(&slot->setting);
    return &slot->setting;
}

/* Release a live slot back to the pool: clears any temporary override,
 * releases its pool region, and marks it free. Caller holds custom_settings_lock. */
void keyspace_release_slot_locked(struct zmk_custom_setting_keyspace *keyspace, uint32_t index) {
    struct zmk_custom_setting_keyspace_slot *slot = &keyspace->slots[index];
    clear_temporary_locked(&slot->setting);
    pool_release_locked(&slot->setting);
    slot->in_use = false;
}

/* Release the live slot that `setting` is the descriptor of. `setting` must be
 * a keyspace slot's own embedded descriptor (&keyspace->slots[i].setting); the
 * owning index is recovered by pointer arithmetic on the fixed slot array.
 * Used by reset, which unlike delete only has the descriptor in hand, not the
 * user key. Caller holds custom_settings_lock. */
void keyspace_release_slot_for_setting_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              const struct zmk_custom_setting *setting) {
    const struct zmk_custom_setting_keyspace_slot *slot =
        CONTAINER_OF(setting, struct zmk_custom_setting_keyspace_slot, setting);
    keyspace_release_slot_locked(keyspace, (uint32_t)(slot - keyspace->slots));
}

/* Validate a PAYLOAD (not a slot's opaque blob) against keyspace->value_type/
 * constraints/max_size - shared by zmk_custom_setting_keyspace_create and
 * zmk_custom_setting_write's keyspace branch. */
int keyspace_validate_payload(const struct zmk_custom_setting_keyspace *keyspace,
                              const struct zmk_custom_setting_value *value) {
    struct zmk_custom_setting payload_shape = {
        .value_type = keyspace->value_type,
        .constraints = keyspace->constraints,
        .constraints_count = keyspace->constraints_count,
    };
    int ret = zmk_custom_setting_validate(&payload_shape, value);
    if (ret < 0) {
        return ret;
    }

    if ((value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
         value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) &&
        value->size > keyspace->max_size) {
        /* Per-keyspace value size ceiling. */
        return -EMSGSIZE;
    }

    return 0;
}

/* Scratch buffer for assembling a keyspace slot's `[user_key\0][payload]`
 * blob before handing it to write_bytes_raw. Sized to cover any registered
 * keyspace regardless of its own (smaller) per-instance limits. Safe as a
 * single shared instance: assembled and consumed synchronously within one
 * write call. */
static uint8_t keyspace_blob_scratch[ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE];

/* Shared blob assembly for keyspace_write_blob (typed payload) and
 * keyspace_write_raw_payload (already-raw payload): builds
 * `blob = [key\0][payload]` and writes it via write_bytes_raw (the
 * keyspace-agnostic pooled-BYTES path). `key` is the literal user key for a
 * fresh create, or NULL to reuse the slot's current key (a write to an
 * already-live entry). */
static int keyspace_write_raw_payload_with_key(const struct zmk_custom_setting *setting,
                                               const char *key, const void *payload_data,
                                               size_t payload_len,
                                               enum zmk_custom_setting_write_mode mode) {
    size_t key_len;
    if (key != NULL) {
        key_len = strlen(key);
        if (key_len + 1 > sizeof(keyspace_blob_scratch)) {
            return -ENAMETOOLONG;
        }
        memcpy(keyspace_blob_scratch, key, key_len);
    } else {
        k_mutex_lock(&custom_settings_lock, K_FOREVER);
        key_len = keyspace_blob_key_len_locked(setting);
        key_len = MIN(key_len, sizeof(keyspace_blob_scratch) - 1);
        if (setting->state->blob.data != NULL && key_len > 0) {
            memcpy(keyspace_blob_scratch, setting->state->blob.data, key_len);
        }
        k_mutex_unlock(&custom_settings_lock);
    }
    keyspace_blob_scratch[key_len] = '\0';

    if (key_len + 1 + payload_len > sizeof(keyspace_blob_scratch)) {
        return -EMSGSIZE;
    }
    if (payload_len > 0) {
        memcpy(keyspace_blob_scratch + key_len + 1, payload_data, payload_len);
    }

    return write_bytes_raw(setting, keyspace_blob_scratch, key_len + 1 + payload_len, mode);
}

/* `value` (typed as keyspace->value_type) is converted to raw payload bytes
 * via value_to_storage, then assembled/written via
 * keyspace_write_raw_payload_with_key. Caller has already validated `value`
 * via keyspace_validate_payload. */
int keyspace_write_blob(const struct zmk_custom_setting *setting, const char *key,
                        const struct zmk_custom_setting_value *value,
                        enum zmk_custom_setting_write_mode mode) {
    const void *payload_data;
    size_t payload_len;
    int ret = value_to_storage(value, &payload_data, &payload_len);
    if (ret < 0) {
        return ret;
    }

    return keyspace_write_raw_payload_with_key(setting, key, payload_data, payload_len, mode);
}

int keyspace_write_raw_payload(const struct zmk_custom_setting *setting, const void *data,
                               size_t size, enum zmk_custom_setting_write_mode mode) {
    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;
    if (keyspace->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
        keyspace->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        /* A raw-bytes write only makes sense for a BYTES/STRING-declared
         * keyspace; other payload types must go through
         * zmk_custom_setting_write with a typed value. */
        return -EINVAL;
    }
    if (size > keyspace->max_size) {
        return -EMSGSIZE;
    }

    return keyspace_write_raw_payload_with_key(setting, NULL, data, size, mode);
}

/* Decode a live keyspace slot's blob into its PRESENTED payload value (typed
 * per keyspace->value_type), respecting a temporary override the same way
 * effective_value() does for any other setting: the full blob is materialized
 * once via effective_value(), then the key prefix is stripped. Returns
 * -EMSGSIZE if the blob itself does not fit the fixed carrier (large payload);
 * callers then fall back to the keyspace-aware large-bytes path. */
int keyspace_read_payload(const struct zmk_custom_setting *setting,
                          struct zmk_custom_setting_value *out_value) {
    struct zmk_custom_setting_value blob_value;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    const struct zmk_custom_setting_value *effective = effective_value(setting);
    if (!effective) {
        k_mutex_unlock(&custom_settings_lock);
        return -EMSGSIZE;
    }
    copy_value(&blob_value, effective);
    k_mutex_unlock(&custom_settings_lock);

    const uint8_t *nul = memchr(blob_value.bytes_value, '\0', blob_value.size);
    size_t key_len = nul ? (size_t)(nul - blob_value.bytes_value) : blob_value.size;
    const uint8_t *payload =
        blob_value.size > key_len ? &blob_value.bytes_value[key_len + 1] : blob_value.bytes_value;
    size_t payload_len = blob_value.size > key_len ? blob_value.size - key_len - 1 : 0;

    value_from_raw(out_value, setting->_keyspace->value_type, payload, payload_len);
    return 0;
}

/* Keyspace counterpart of zmk_custom_setting_read_into: strips the key
 * prefix from either the raw large-store payload (any size) or the
 * carrier-sized/temporary-override path (via keyspace_read_payload +
 * read_into_visitor, reusing a normal setting's read_into conversion). */
int keyspace_read_into(const struct zmk_custom_setting *setting, void *buf, size_t capacity,
                       size_t *out_size, enum zmk_custom_setting_value_type *out_type) {
    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (setting_uses_blob_store(setting) && !setting_temporary_active(setting) &&
        setting->state->blob.size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        const struct zmk_custom_setting_state *state = setting->state;
        size_t key_len = keyspace_blob_key_len_locked(setting);
        size_t payload_size = state->blob.size > key_len ? state->blob.size - key_len - 1 : 0;
        int ret = 0;
        if (payload_size > capacity) {
            ret = -EMSGSIZE;
        } else {
            if (payload_size > 0) {
                memcpy(buf, state->blob.data + key_len + 1, payload_size);
            }
            if (out_size) {
                *out_size = payload_size;
            }
            if (out_type) {
                *out_type = keyspace->value_type;
            }
        }
        k_mutex_unlock(&custom_settings_lock);
        return ret;
    }
    k_mutex_unlock(&custom_settings_lock);

    struct zmk_custom_setting_value value;
    int ret = keyspace_read_payload(setting, &value);
    if (ret < 0) {
        return ret;
    }

    struct read_into_context ctx = {.buf = buf, .capacity = capacity, .ret = -EIO};
    read_into_visitor(&value, &ctx);
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

const struct zmk_custom_setting *
zmk_custom_setting_keyspace_find(const struct zmk_custom_setting_keyspace *keyspace,
                                 const char *key) {
    if (!keyspace || !key) {
        return NULL;
    }

    struct zmk_custom_setting_keyspace *mutable_keyspace =
        (struct zmk_custom_setting_keyspace *)keyspace;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int index = keyspace_slot_index_for_key_locked(mutable_keyspace, key);
    k_mutex_unlock(&custom_settings_lock);

    return index >= 0 ? &keyspace->slots[index].setting : NULL;
}

int zmk_custom_setting_keyspace_create(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode,
                                       const struct zmk_custom_setting **out_setting) {
    if (!keyspace || !key || !value) {
        return -EINVAL;
    }

    if (!keyspace_key_matches_prefix(keyspace, key)) {
        return -EINVAL;
    }

    int ret = keyspace_validate_payload(keyspace, value);
    if (ret < 0) {
        return ret;
    }

    size_t key_len = bounded_strlen(key, keyspace->max_key_len);
    if (key_len == 0 || key_len >= keyspace->max_key_len || key[key_len] != '\0') {
        return -ENAMETOOLONG;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);

    if (keyspace_slot_index_for_key_locked(keyspace, key) >= 0) {
        k_mutex_unlock(&custom_settings_lock);
        return -EEXIST;
    }

    int index = keyspace_free_slot_index_locked(keyspace);
    if (index < 0) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOSPC;
    }

    struct zmk_custom_setting *setting = keyspace_bind_slot_locked(keyspace, (uint32_t)index);
    k_mutex_unlock(&custom_settings_lock);

    ret = keyspace_write_blob(setting, key, value, mode);
    if (ret < 0) {
        /* Roll back: the initial value write failed (e.g. constraint
         * violation, or -ENOSPC from the pool), so do not leave a
         * half-created entry occupying a slot. */
        k_mutex_lock(&custom_settings_lock, K_FOREVER);
        keyspace_release_slot_locked(keyspace, (uint32_t)index);
        k_mutex_unlock(&custom_settings_lock);
        return ret;
    }

    if (out_setting) {
        *out_setting = setting;
    }
    return 0;
}

int zmk_custom_setting_keyspace_delete(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key) {
    if (!keyspace || !key) {
        return -EINVAL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int index = keyspace_slot_index_for_key_locked(keyspace, key);
    if (index < 0) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOENT;
    }
    struct zmk_custom_setting *setting = &keyspace->slots[index].setting;
    k_mutex_unlock(&custom_settings_lock);

    /* Erase the persisted record (if any) the same way a scalar setting's
     * reset does, then release the slot. Ignore -ENOENT from a setting that
     * was never saved. */
    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(setting, name, sizeof(name));
    if (ret == 0) {
        ret = settings_delete(name);
        if (ret == -ENOENT) {
            ret = 0;
        }
    }
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    keyspace_release_slot_locked(keyspace, (uint32_t)index);
    k_mutex_unlock(&custom_settings_lock);

    return 0;
}

/* Parse a stored record name's remainder (after the custom_subsystem_id
 * component split off by settings_name_next, e.g. "macro/#3" for a keyspace
 * whose key_prefix is "macro/") as "<key_prefix>#<index>" for `keyspace`.
 * Zephyr's settings subsystem only treats '/' as a hierarchy separator -
 * '#' is an ordinary name byte, so this format needs no escaping. */
bool keyspace_parse_ordinal_name(const struct zmk_custom_setting_keyspace *keyspace,
                                 const char *name, uint32_t *out_index) {
    size_t prefix_len = strlen(keyspace->key_prefix);
    if (strncmp(name, keyspace->key_prefix, prefix_len) != 0 || name[prefix_len] != '#') {
        return false;
    }

    const char *digits = name + prefix_len + 1;
    if (*digits == '\0') {
        return false;
    }

    uint32_t value = 0;
    for (const char *p = digits; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + (uint32_t)(*p - '0');
    }

    *out_index = value;
    return true;
}
