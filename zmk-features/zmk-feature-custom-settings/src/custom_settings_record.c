/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>

static size_t bounded_len(const char *str, size_t max_len) {
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        len++;
    }

    return len;
}

int zmk_custom_setting_record_encode(const struct zmk_custom_setting_record_schema *schema,
                                     const void *record, uint8_t *out, size_t out_capacity,
                                     size_t *out_size) {
    if (!schema || !record || !out) {
        return -EINVAL;
    }

    if (out_capacity < 1) {
        return -EMSGSIZE;
    }

    size_t pos = 0;
    out[pos++] = ZMK_CUSTOM_SETTING_RECORD_VERSION;

    const uint8_t *base = record;
    for (size_t i = 0; i < schema->fields_count; i++) {
        const struct zmk_custom_setting_record_field *field = &schema->fields[i];
        const uint8_t *field_ptr = base + field->offset;

        size_t len = field->size;
        if (field->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
            len = bounded_len((const char *)field_ptr, field->size);
        }
        if (len > ZMK_CUSTOM_SETTING_RECORD_MAX_FIELD_SIZE) {
            return -EMSGSIZE;
        }
        if (pos + 2 + len > out_capacity) {
            return -EMSGSIZE;
        }

        out[pos++] = field->field_id;
        out[pos++] = (uint8_t)len;
        memcpy(&out[pos], field_ptr, len);
        pos += len;
    }

    if (out_size) {
        *out_size = pos;
    }
    return 0;
}

static const struct zmk_custom_setting_record_field *
find_field(const struct zmk_custom_setting_record_schema *schema, uint8_t field_id) {
    for (size_t i = 0; i < schema->fields_count; i++) {
        if (schema->fields[i].field_id == field_id) {
            return &schema->fields[i];
        }
    }

    return NULL;
}

int zmk_custom_setting_record_decode(const struct zmk_custom_setting_record_schema *schema,
                                     const uint8_t *data, size_t data_size, void *record) {
    if (!schema || !data || !record) {
        return -EINVAL;
    }
    if (data_size < 1) {
        return -EINVAL;
    }
    if (data[0] != ZMK_CUSTOM_SETTING_RECORD_VERSION) {
        return -ENOTSUP;
    }

    uint8_t *base = record;
    size_t pos = 1;
    while (pos + 2 <= data_size) {
        uint8_t field_id = data[pos];
        uint8_t len = data[pos + 1];
        pos += 2;
        if (pos + len > data_size) {
            return -EINVAL;
        }

        const struct zmk_custom_setting_record_field *field = find_field(schema, field_id);
        if (field) {
            bool is_string = field->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING;
            size_t capacity = is_string && field->size > 0 ? field->size - 1 : field->size;
            size_t copy_len = MIN(len, capacity);
            memcpy(base + field->offset, &data[pos], copy_len);
            if (is_string) {
                ((char *)(base + field->offset))[copy_len] = '\0';
            }
        }
        /* Unknown field id: skip. Keeps decode forward/backward compatible
         * with schemas that added or removed fields. */

        pos += len;
    }

    return 0;
}

static int validate_field(const struct zmk_custom_setting_record_field *field,
                          const uint8_t *record_base) {
    if (!field->constraint || field->constraint->type == ZMK_CUSTOM_SETTING_CONSTRAINT_NONE) {
        return 0;
    }

    if (field->constraint->type != ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE ||
        field->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        return -ENOTSUP;
    }

    int32_t value;
    memcpy(&value, record_base + field->offset, sizeof(value));
    if (value < field->constraint->range.min.int32_value ||
        value > field->constraint->range.max.int32_value) {
        return -ERANGE;
    }

    return 0;
}

int zmk_custom_setting_record_set(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_setting_record_schema *schema,
                                  const void *record, enum zmk_custom_setting_write_mode mode) {
    if (!setting || !schema || !record) {
        return -EINVAL;
    }

    const uint8_t *base = record;
    for (size_t i = 0; i < schema->fields_count; i++) {
        int ret = validate_field(&schema->fields[i], base);
        if (ret < 0) {
            return ret;
        }
    }

    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE];
    size_t encoded_size;
    int ret =
        zmk_custom_setting_record_encode(schema, record, encoded, sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }

    return zmk_custom_setting_write_bytes(setting, encoded, encoded_size, mode);
}

int zmk_custom_setting_record_get(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_setting_record_schema *schema,
                                  void *record) {
    if (!setting || !schema || !record) {
        return -EINVAL;
    }

    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE];
    size_t encoded_size;
    int ret = zmk_custom_setting_read_into(setting, encoded, sizeof(encoded), &encoded_size, NULL);
    if (ret < 0) {
        return ret;
    }

    return zmk_custom_setting_record_decode(schema, encoded, encoded_size, record);
}
