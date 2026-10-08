/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Large (> single-frame) setting values and the shared value pool
 * (CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES).
 *
 * A pooled setting (descriptor blob.pool != NULL) does not own a fixed
 * buffer; instead its value lives in a region of its pool's shared backing
 * array, grown/relocated on demand by pool_ensure_region(). The pool's
 * member list is intrusive (state->_pool_next), so no separate allocation
 * tracks membership.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <cormoran/zmk/custom_settings.h>

#include "custom_settings_internal.h"

/* Bytes a pool member currently occupies: its payload plus the trailing NUL
 * for STRING (blob_store_set_raw always writes that NUL inside the region,
 * so it must count toward the region's extent). 0 for a member with no
 * region (blob.data == NULL). */
static size_t pool_member_extent(const struct zmk_custom_setting *setting) {
    if (setting->state->blob.data == NULL) {
        return 0;
    }
    return setting->state->blob.size +
           (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING ? 1 : 0);
}

/* Link `setting` into `pool`'s intrusive member list. Caller holds
 * custom_settings_lock and must only call this the moment `setting->state->blob.data`
 * transitions from NULL to non-NULL (i.e. `setting` is not already a
 * member). */
static void pool_link_locked(struct zmk_custom_setting_large_pool *pool,
                             const struct zmk_custom_setting *setting) {
    setting->state->_pool_next = pool->members;
    pool->members = setting;
}

/* Unlink `setting` from `pool`'s intrusive member list. Caller holds
 * custom_settings_lock. A no-op if `setting` is not currently a member (e.g. it
 * never allocated a region). */
static void pool_unlink_locked(struct zmk_custom_setting_large_pool *pool,
                               const struct zmk_custom_setting *setting) {
    if (pool->members == setting) {
        pool->members = setting->state->_pool_next;
        setting->state->_pool_next = NULL;
        return;
    }

    const struct zmk_custom_setting *prev = pool->members;
    while (prev && prev->state->_pool_next != setting) {
        prev = prev->state->_pool_next;
    }
    if (prev) {
        prev->state->_pool_next = setting->state->_pool_next;
        setting->state->_pool_next = NULL;
    }
}

/*
 * Release `setting`'s pool region (if any) and unlink it from its pool's
 * member list. Caller holds custom_settings_lock. Must run before a keyspace
 * slot's storage is reused by a future entry (keyspace_release_slot_locked,
 * called from zmk_custom_setting_keyspace_delete) - otherwise the pool's
 * member list would keep a dangling pointer at storage that no longer
 * represents this setting (see the intrusive-list note on struct
 * zmk_custom_setting_large_pool). A no-op for a non-pooled setting or one
 * with no allocated region.
 */
void pool_release_locked(const struct zmk_custom_setting *setting) {
    if (setting->blob.pool == NULL || setting->state->blob.data == NULL) {
        return;
    }
    pool_unlink_locked(setting->blob.pool, setting);
    setting->state->blob.data = NULL;
    setting->state->blob.size = 0;
}

/*
 * Ensure a pooled setting (setting->blob.pool != NULL) has a region of at
 * least `needed` bytes (payload plus the STRING NUL, if any - the caller
 * computes this the same way pool_member_extent does). Caller holds
 * custom_settings_lock. Returns 0 on success or -ENOSPC if the pool cannot fit
 * `needed` bytes for this setting even after compacting every other member;
 * on -ENOSPC nothing is modified - not `setting`, not any other pool
 * member - so the write that requested this region can fail cleanly with the
 * setting's previous value untouched.
 *
 * Deliberately simple: no free lists, no per-region headers, no cached
 * bookkeeping. A region's extent is always derived on the spot from its
 * owning setting's current blob.size, so "compacting" just means walking
 * every OTHER member of this pool (via the pool's own intrusive member list,
 * not the global setting registry - see struct zmk_custom_setting_large_pool),
 * sliding each down (in ascending address order, via memmove, which
 * tolerates the overlapping src/dest this can produce) to eliminate any gap
 * starting at pool->data, and finally handing this setting whatever space is
 * left right after the last one. That happens on every growth, not only when
 * strictly necessary, but pools are expected to hold a handful of members, so
 * this stays cheap and never needs invalidating.
 *
 * Selecting members in ascending address order is done without an
 * intermediate array (repeatedly scanning for the lowest not-yet-processed
 * address) so this has no fixed cap on pool membership. Every member walked
 * here (other than `setting` itself, which may or may not be one yet) is by
 * construction one with blob.data != NULL, since that is the pool's
 * membership invariant - see pool_link_locked/pool_unlink_locked.
 */
int pool_ensure_region(const struct zmk_custom_setting *setting, size_t needed) {
    struct zmk_custom_setting_large_pool *pool = setting->blob.pool;

    if (needed == 0) {
        if (setting->state->blob.data != NULL) {
            pool_unlink_locked(pool, setting);
            setting->state->blob.data = NULL;
        }
        return 0;
    }

    if (setting->state->blob.data != NULL && needed <= pool_member_extent(setting)) {
        /* Shrinking (or an unchanged size) keeps writing into the same
         * region - no need to move anything. */
        return 0;
    }

    /* Feasibility check before moving anything: every other member of this
     * pool keeps its current extent, plus the room this write needs. */
    size_t other_total = 0;
    for (const struct zmk_custom_setting *other = pool->members; other;
         other = other->state->_pool_next) {
        if (other == setting) {
            continue;
        }
        other_total += pool_member_extent(other);
    }
    if (other_total + needed > pool->size) {
        return -ENOSPC;
    }

    uint8_t *cursor = pool->data;
    bool have_last_addr = false;
    const uint8_t *last_addr = NULL;
    for (;;) {
        const struct zmk_custom_setting *next = NULL;
        for (const struct zmk_custom_setting *other = pool->members; other;
             other = other->state->_pool_next) {
            if (other == setting) {
                continue;
            }
            if (have_last_addr && other->state->blob.data <= last_addr) {
                /* Already compacted in an earlier iteration. */
                continue;
            }
            if (!next || other->state->blob.data < next->state->blob.data) {
                next = other;
            }
        }
        if (!next) {
            break;
        }

        have_last_addr = true;
        last_addr = next->state->blob.data;

        size_t extent = pool_member_extent(next);
        if (next->state->blob.data != cursor) {
            memmove(cursor, next->state->blob.data, extent);
            next->state->blob.data = cursor;
        }
        cursor += extent;
    }

    bool was_member = setting->state->blob.data != NULL;
    setting->state->blob.data = cursor;
    if (!was_member) {
        pool_link_locked(pool, setting);
    }
    return 0;
}

size_t zmk_custom_setting_large_pool_used(const struct zmk_custom_setting_large_pool *pool) {
    if (!pool) {
        return 0;
    }

    size_t used = 0;
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    for (const struct zmk_custom_setting *member = pool->members; member;
         member = member->state->_pool_next) {
        used += pool_member_extent(member);
    }
    k_mutex_unlock(&custom_settings_lock);
    return used;
}

int zmk_custom_setting_with_large_raw_bytes(const struct zmk_custom_setting *setting,
                                            zmk_custom_setting_raw_bytes_visitor_t visitor,
                                            void *user_data) {
    if (!setting || !visitor) {
        return -EINVAL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (!setting_uses_blob_store(setting) || setting_temporary_active(setting)) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOTSUP;
    }

    const struct zmk_custom_setting_state *state = setting->state;

    if (setting->_keyspace) {
        /* Stream only the payload slice - skip the embedded
         * "user_key\0" prefix (see the keyspace design comment in the
         * header). Re-derived under the lock every call, same invariant as
         * every other blob.data access in this file. */
        size_t key_len = keyspace_blob_key_len_locked(setting);
        const uint8_t *payload =
            state->blob.size > key_len ? state->blob.data + key_len + 1 : (const uint8_t *)"";
        size_t payload_size = state->blob.size > key_len ? state->blob.size - key_len - 1 : 0;
        visitor(payload, payload_size, user_data);
        k_mutex_unlock(&custom_settings_lock);
        return 0;
    }

    const uint8_t *data = state->blob.size > 0 ? state->blob.data : (const uint8_t *)"";
    visitor(data, state->blob.size, user_data);
    k_mutex_unlock(&custom_settings_lock);
    return 0;
}

/* Apply a large (> carrier) raw BYTES/STRING payload to a blob setting.
 * Caller holds custom_settings_lock. TEMPORARY mode is not supported for
 * large values (the temporary override pool is intentionally small). */
int write_large_locked(const struct zmk_custom_setting *setting, const void *data, size_t size,
                       enum zmk_custom_setting_write_mode mode) {
    if (size > setting_capacity(setting)) {
        return -EMSGSIZE;
    }

    int ret;
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        ret = blob_store_set_raw(setting, data, size);
        if (ret < 0) {
            return ret;
        }
        set_setting_dirty(setting, true);
        clear_temporary_locked(setting);
        return 0;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        ret = blob_store_set_raw(setting, data, size);
        if (ret < 0) {
            return ret;
        }
        clear_temporary_locked(setting);
        return save_setting_locked(setting);
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY:
        return -EMSGSIZE;
    default:
        return -EINVAL;
    }
}
