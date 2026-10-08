/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Per-setting RPC bytes converter hooks (CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS).
 *
 * zmk_custom_setting_serialize_rpc_value / _deserialize_rpc_value (the public
 * entry points a converter is reached through) stay in core so the API is
 * always available; when this feature is off they fall back to a plain
 * identity copy without referencing anything here. This file only holds the
 * per-setting/per-keyspace converter dispatch (convert_rpc_bytes_value).
 */

#include <errno.h>

#include <cormoran/zmk/custom_settings.h>

#include "custom_settings_internal.h"

int convert_rpc_bytes_value(const struct zmk_custom_setting *setting,
                            const struct zmk_custom_setting_value *src,
                            struct zmk_custom_setting_value *dest,
                            zmk_custom_setting_rpc_bytes_converter_t converter) {
    if (!setting || !src || !dest) {
        return -EINVAL;
    }

    /* A keyspace slot's own value_type is always BYTES internally (the
     * opaque blob); the PRESENTED type - what `src`/`dest` here are actually
     * typed as (the PAYLOAD, not the blob) - is the owning keyspace's
     * declared value_type. */
    const struct zmk_custom_setting_keyspace *keyspace = zmk_custom_setting_keyspace_of(setting);
    enum zmk_custom_setting_value_type presented_type =
        keyspace ? keyspace->value_type : setting->value_type;

    if (presented_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
        src->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES || converter == NULL) {
        copy_value(dest, src);
        return 0;
    }

    *dest = (struct zmk_custom_setting_value){
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    };
    size_t dest_size = 0;
    int ret = converter(setting, src->bytes_value, src->size, dest->bytes_value, &dest_size,
                        sizeof(dest->bytes_value));
    if (ret < 0) {
        return ret;
    }
    if (dest_size > sizeof(dest->bytes_value)) {
        return -EMSGSIZE;
    }

    dest->size = dest_size;
    return 0;
}
