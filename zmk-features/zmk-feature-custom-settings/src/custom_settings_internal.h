/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Private, shared-within-module contract between src/custom_settings.c
 * (core lifecycle, always compiled) and the optional feature translation
 * units: src/custom_settings_pool.c (CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES),
 * src/custom_settings_array.c (CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY),
 * src/custom_settings_keyspace.c (CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE, which
 * selects LARGE_VALUES: a keyspace slot is a pool-backed blob), and
 * src/custom_settings_rpc_convert.c (CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS).
 * src/custom_settings_record.c (CONFIG_ZMK_CUSTOM_SETTINGS_RECORD) does NOT
 * need this header - it only calls the public API.
 *
 * NOT a public header - lives under src/, not include/. Only files inside
 * this module may include it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/kernel.h>

#include <cormoran/zmk/custom_settings.h>

/* Shared settings-subsystem naming, used by both custom_settings.c
 * (setting_storage_name) and custom_settings_array.c (array_size_storage_name/
 * split_array_size_key) to agree on the on-flash key layout. */
#define SETTINGS_SUBTREE "custom_settings"
#define ARRAY_SIZE_STORAGE_KEY "_size"

/* The single lock serializing all custom-settings state access. Defined
 * (non-static) in custom_settings.c so every feature translation unit can
 * take it too. */
extern struct k_mutex custom_settings_lock;

/*
 * ---------------------------------------------------------------------
 * POOL entry points (defined in custom_settings_pool.c when
 * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES is enabled). Declared here
 * unconditionally so core call sites can be IS_ENABLED-guarded without
 * custom_settings_pool.c needing to be compiled.
 * ---------------------------------------------------------------------
 */

/* Ensure a pooled setting (setting->blob.pool != NULL) has a region of at
 * least `needed` bytes, compacting other pool members if necessary. See the
 * implementation comment in custom_settings_pool.c for the full contract. */
int pool_ensure_region(const struct zmk_custom_setting *setting, size_t needed);

/* Release `setting`'s pool region (if any) and unlink it from its pool's
 * member list. Caller holds custom_settings_lock. */
void pool_release_locked(const struct zmk_custom_setting *setting);

/* Apply a large (> carrier) raw BYTES/STRING payload to a blob setting.
 * Caller holds custom_settings_lock. */
int write_large_locked(const struct zmk_custom_setting *setting, const void *data, size_t size,
                       enum zmk_custom_setting_write_mode mode);

/*
 * ---------------------------------------------------------------------
 * ARRAY entry points (defined in custom_settings_array.c when
 * CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY is enabled). Declared here
 * unconditionally so core call sites can be guarded by
 * zmk_custom_setting_is_array() (which itself folds to a compile-time
 * constant when the feature is off - see the inline definition in
 * include/cormoran/zmk/custom_settings.h) without custom_settings_array.c
 * needing to be compiled.
 * ---------------------------------------------------------------------
 */

/* Acquire the shared "index view" for one (array descriptor, index) pair -
 * see the struct zmk_custom_setting_array_view_slot doc in
 * custom_settings_array.c. Caller holds custom_settings_lock. */
struct zmk_custom_setting *array_view_acquire(const struct zmk_custom_setting *array_descriptor,
                                              uint32_t index);

/* Per-array-element dirty/has-persistent flag storage (array_state->dirty[]/
 * has_persistent[]), the array counterpart of the scalar
 * ZMK_CUSTOM_SETTING_STATE_DIRTY/HAS_PERSISTENT flags. Caller holds
 * custom_settings_lock. */
bool *array_dirty_slot(const struct zmk_custom_setting *setting);
bool *array_has_persistent_slot(const struct zmk_custom_setting *setting);

/* Set an array's active (in-memory) / persisted length, clearing any
 * temporary override past the new size. Caller holds custom_settings_lock. */
void set_array_memory_size_locked(const struct zmk_custom_setting *array_element,
                                  uint32_t array_size);
void set_array_persistent_size_locked(const struct zmk_custom_setting *array_element,
                                      uint32_t array_size);

/* Settings-subtree name of an array's "_size" marker record. */
int array_size_storage_name(const struct zmk_custom_setting *setting, char *name, size_t name_size);

/* Persist an array setting: every active element plus the "_size" marker,
 * then delete any now-inactive elements' records. Caller holds
 * custom_settings_lock. */
int save_array_locked(const struct zmk_custom_setting *array_descriptor);

/* Discard/reset one array element view's in-memory value, the array
 * counterparts of the scalar zmk_custom_setting_discard/reset bodies.
 * Caller holds custom_settings_lock. */
int discard_array_element_locked(struct zmk_custom_setting *view);
int reset_array_element_locked(struct zmk_custom_setting *view);

/* Apply a loaded "_size" record's raw payload to an array's persistent/
 * memory size. Caller holds custom_settings_lock. */
int array_size_from_storage(const struct zmk_custom_setting *array_element, const void *data,
                            size_t len);

/* Recognize a stored settings name as an array's "_size" marker
 * (split_array_size_key) or one array element record (split_array_element_key),
 * extracting the array's public key (and, for the latter, the element
 * index). Used by custom_settings_handle_set (the settings-load callback) to
 * route a loaded record to the right array element/marker. */
bool split_array_size_key(const char *name, char *array_key, size_t array_key_size);
bool split_array_element_key(const char *name, char *array_key, size_t array_key_size,
                             uint32_t *index);

/*
 * ---------------------------------------------------------------------
 * KEYSPACE entry points (defined in custom_settings_keyspace.c when
 * CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE is enabled). Declared here
 * unconditionally so core call sites can be guarded by
 * zmk_custom_setting_keyspace_of() (which folds to a compile-time constant
 * NULL when the feature is off) without custom_settings_keyspace.c needing to
 * be compiled. keyspace_blob_key_len_locked is NOT here: it stays defined in
 * custom_settings.c (core) because custom_settings_pool.c calls it from a
 * branch keyed on a runtime field (setting->_keyspace), which the compiler
 * cannot prove dead, so the symbol must exist even when KEYSPACE is off.
 * ---------------------------------------------------------------------
 */

/* Decode a live keyspace slot's blob into its presented PAYLOAD value (typed
 * per the owning keyspace's value_type), respecting a temporary override.
 * Caller does NOT hold custom_settings_lock (this takes it itself). */
int keyspace_read_payload(const struct zmk_custom_setting *setting,
                          struct zmk_custom_setting_value *out_value);

/* Keyspace counterpart of zmk_custom_setting_read_into: strips the key
 * prefix from either the raw large-store payload or the carrier-sized/
 * temporary-override path. Caller does NOT hold custom_settings_lock. */
int keyspace_read_into(const struct zmk_custom_setting *setting, void *buf, size_t capacity,
                       size_t *out_size, enum zmk_custom_setting_value_type *out_type);

/* Validate a PAYLOAD (not a slot's opaque blob) against keyspace->value_type/
 * constraints/max_size. */
int keyspace_validate_payload(const struct zmk_custom_setting_keyspace *keyspace,
                              const struct zmk_custom_setting_value *value);

/* `value` (typed as keyspace->value_type) is converted to raw payload bytes
 * and written as the slot's `[key\0][payload]` blob; `key` is the literal
 * user key for a fresh create, or NULL to reuse the slot's current key. */
int keyspace_write_blob(const struct zmk_custom_setting *setting, const char *key,
                        const struct zmk_custom_setting_value *value,
                        enum zmk_custom_setting_write_mode mode);

/* Write an already-raw payload (e.g. the WriteValueChunk commit path) as a
 * keyspace slot's blob, reusing the slot's current key. */
int keyspace_write_raw_payload(const struct zmk_custom_setting *setting, const void *data,
                               size_t size, enum zmk_custom_setting_write_mode mode);

/* Release a live slot back to the pool: clears any temporary override,
 * releases its pool region, and marks it free. Caller holds
 * custom_settings_lock. */
void keyspace_release_slot_locked(struct zmk_custom_setting_keyspace *keyspace, uint32_t index);

/* Release the slot that `setting` is the descriptor of (recovering the slot
 * index from the descriptor pointer). Caller holds custom_settings_lock. */
void keyspace_release_slot_for_setting_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              const struct zmk_custom_setting *setting);

/* Bind slot `index` of `keyspace` to its stable ordinal storage identity and
 * wire it into the keyspace's shared pool, with an EMPTY blob - the caller
 * populates it immediately after (settings-load value-apply, or a fresh
 * create). Caller holds custom_settings_lock. */
struct zmk_custom_setting *keyspace_bind_slot_locked(struct zmk_custom_setting_keyspace *keyspace,
                                                     uint32_t index);

/* Parse a stored record name's remainder as "<key_prefix>#<index>" for
 * `keyspace` - used by custom_settings_handle_set (the settings-load
 * callback) to re-bind a persisted keyspace slot on boot without any module
 * code running. */
bool keyspace_parse_ordinal_name(const struct zmk_custom_setting_keyspace *keyspace,
                                 const char *name, uint32_t *out_index);

/* Decode a live keyspace slot's blob into its user key, the keyspace
 * counterpart of zmk_custom_setting_public_key for a normal setting. Caller
 * holds custom_settings_lock; the returned pointer is only valid until the
 * lock is released (shared scratch storage, like keyspace_read_payload's
 * caller-visible scratch and every other *_scratch_value in
 * custom_settings.c). */
const char *keyspace_public_key_locked(const struct zmk_custom_setting *setting);

/* Bound for a keyspace slot's `[user_key\0][payload]` blob assembly/decode
 * scratch, large enough for any registered keyspace's own max_key_len/
 * max_size - shared between custom_settings_keyspace.c (the scratch buffer)
 * and custom_settings.c's custom_settings_handle_set (which stages a loaded
 * record's raw bytes in a buffer of this same size before applying it). */
#define ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE                                             \
    (CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN + CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE)

/*
 * ---------------------------------------------------------------------
 * RPC_CONVERTERS entry point (defined in custom_settings_rpc_convert.c when
 * CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS is enabled). Declared here
 * unconditionally so zmk_custom_setting_serialize_rpc_value/
 * _deserialize_rpc_value (core, always compiled) can call it from inside an
 * IS_ENABLED-guarded branch without custom_settings_rpc_convert.c needing to
 * be compiled.
 * ---------------------------------------------------------------------
 */

/* Convert a BYTES value between its stored form and its RPC-presented form
 * via `converter` (a setting's or - for a keyspace slot - its owning
 * keyspace's rpc_serializer/rpc_deserializer), or copy it through unchanged
 * when the presented type isn't BYTES or `converter` is NULL. */
int convert_rpc_bytes_value(const struct zmk_custom_setting *setting,
                            const struct zmk_custom_setting_value *src,
                            struct zmk_custom_setting_value *dest,
                            zmk_custom_setting_rpc_bytes_converter_t converter);

/*
 * ---------------------------------------------------------------------
 * CORE internals used by custom_settings_pool.c / custom_settings_array.c /
 * custom_settings_keyspace.c / custom_settings_rpc_convert.c (and any future
 * feature translation unit). Defined (non-static) in custom_settings.c.
 * ---------------------------------------------------------------------
 */

/* True for a non-array BYTES/STRING setting, i.e. one whose value lives in
 * the blob store (fixed buffer or pool region) rather than a scalar union
 * member. */
bool setting_uses_blob_store(const struct zmk_custom_setting *setting);

/* True while `setting` has an active TEMPORARY-mode override. */
bool setting_temporary_active(const struct zmk_custom_setting *setting);

/* Maximum payload size `setting` may ever hold (its own blob.max_size for a
 * non-array BYTES/STRING setting, else the global carrier size). */
size_t setting_capacity(const struct zmk_custom_setting *setting);

/* Discard any active TEMPORARY-mode override for `setting`. Caller holds
 * custom_settings_lock. */
void clear_temporary_locked(const struct zmk_custom_setting *setting);

/* Mark `setting`'s in-memory value as (not) differing from its persisted
 * value. */
void set_setting_dirty(const struct zmk_custom_setting *setting, bool dirty);

/* Persist `setting`'s current in-memory value to flash. Caller holds
 * custom_settings_lock. */
int save_setting_locked(const struct zmk_custom_setting *setting);

/* Length of the embedded "user_key\0" prefix stored ahead of a keyspace
 * slot's payload in its blob, or 0 for a non-keyspace setting. Caller holds
 * custom_settings_lock. */
size_t keyspace_blob_key_len_locked(const struct zmk_custom_setting *setting);

/* Store `size` raw payload bytes into a blob setting's store (fixed buffer
 * or, for a pooled setting, a pool region obtained via pool_ensure_region()).
 * Caller must have validated size <= setting_capacity(setting). */
int blob_store_set_raw(const struct zmk_custom_setting *setting, const void *data, size_t size);

/* Bounded strnlen, used throughout the module wherever a raw string field
 * (never guaranteed NUL-terminated within its buffer) needs a safe length. */
size_t bounded_strlen(const char *str, size_t max_len);

/* Copy `src` into `dest`, re-deriving STRING's `size` from its NUL terminator
 * (see the definition for why: `size` is not always trustworthy on the
 * source side). */
void copy_value(struct zmk_custom_setting_value *dest, const struct zmk_custom_setting_value *src);

/* Split `value` into a (data pointer, length) pair suitable for
 * settings_save_one/memcpy, encoding a BEHAVIOR value into a shared scratch
 * buffer first if needed. Caller holds custom_settings_lock (the BEHAVIOR
 * scratch buffer is not otherwise synchronized). */
int value_to_storage(const struct zmk_custom_setting_value *value, const void **data, size_t *len);

/* Settings-subtree name of `setting`'s own record (a scalar/blob setting's
 * key, or - for an array element view - "<array_key>/<index>"). */
int setting_storage_name(const struct zmk_custom_setting *setting, char *name, size_t name_size);

/* Return the effective value (temporary override if active, else the memory
 * value) as a fixed carrier, or NULL when the value is a blob payload too
 * large for the carrier. Caller holds custom_settings_lock; the returned
 * pointer is only valid until the lock is released (shared scratch storage,
 * like every other *_scratch_value in custom_settings.c). */
const struct zmk_custom_setting_value *effective_value(const struct zmk_custom_setting *setting);

/* Apply a write in the selected mode (TEMPORARY/MEMORY/PERSIST) to an
 * already-resolved, in-range setting or array element view. Caller holds
 * custom_settings_lock. */
int write_value_locked(const struct zmk_custom_setting *setting,
                       const struct zmk_custom_setting_value *value,
                       enum zmk_custom_setting_write_mode mode);

/* Raise a ZMK_CUSTOM_SETTING_CHANGED event for `setting`, sourced locally. */
void raise_setting_changed(const struct zmk_custom_setting *setting,
                           enum zmk_custom_setting_changed_kind kind);

/* Decode `size` raw bytes of `type` into `dest`. Non-static: needed by
 * custom_settings_keyspace.c's keyspace_read_payload to decode a slot's
 * payload once the key prefix has been stripped. */
void value_from_raw(struct zmk_custom_setting_value *dest, enum zmk_custom_setting_value_type type,
                    const void *data, size_t size);

/* Keyspace-agnostic "write this exact raw payload as the setting's stored
 * value" primitive (validated small-carrier path, or - for a value exceeding
 * the carrier - the large-store path). Non-static: custom_settings_keyspace.c's
 * keyspace_write_raw_payload_with_key calls this to store an already-
 * assembled `[key\0][payload]` blob verbatim, bypassing the public
 * zmk_custom_setting_write_bytes (which would re-trigger the keyspace
 * interception). */
int write_bytes_raw(const struct zmk_custom_setting *setting, const void *data, size_t size,
                    enum zmk_custom_setting_write_mode mode);

/* Reset one setting's mutable state to its just-registered, nothing-loaded-
 * yet state. Non-static: custom_settings_keyspace.c's keyspace_bind_slot_locked
 * calls this to initialize a freshly (re)bound slot's embedded state block
 * the same way a compile-time setting is initialized at boot. Caller must
 * hold custom_settings_lock if called after boot. */
void init_setting_state_locked(const struct zmk_custom_setting *setting);

/* Shared visitor context/callback pairing a zmk_custom_setting_value_visitor_t
 * invocation with a fixed output buffer - the common tail of
 * zmk_custom_setting_read_into and custom_settings_keyspace.c's
 * keyspace_read_into. */
struct read_into_context {
    void *buf;
    size_t capacity;
    size_t out_size;
    enum zmk_custom_setting_value_type out_type;
    int ret;
};
void read_into_visitor(const struct zmk_custom_setting_value *value, void *user_data);
