/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_ext_power_generic

#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#include <drivers/ext_power.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct ext_power_generic_config {
    const struct gpio_dt_spec *control;
    const size_t control_gpios_count;
    const uint16_t init_delay_ms;
};

struct ext_power_generic_data {
    bool status;
#if IS_ENABLED(CONFIG_SETTINGS)
    atomic_t requested_on;
#endif
};

#if IS_ENABLED(CONFIG_SETTINGS)
static void ext_power_save_state_work(struct k_work *work) {
    char setting_path[40];
    const struct device *ext_power = DEVICE_DT_GET(DT_DRV_INST(0));
    struct ext_power_generic_data *data = ext_power->data;

    snprintf(setting_path, sizeof(setting_path), "ext_power/state/%s", ext_power->name);
    /* Persist user intent, never the temporary suspend state. Keep the
     * existing one-byte 0/1 storage format for installed keyboards. */
    uint8_t requested = atomic_get(&data->requested_on) ? 1 : 0;
    int rc = settings_save_one(setting_path, &requested, sizeof(requested));
    if (rc < 0) LOG_WRN("External power state save failed: %d", rc);
}

static struct k_work_delayable ext_power_save_work;
#endif

int ext_power_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&ext_power_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

/* Hardware changes during init, settings restore and PM must not enqueue
 * settings writes. Only explicit API calls change the persisted preference. */
static int ext_power_apply(const struct device *dev, bool enabled) {
    struct ext_power_generic_data *data = dev->data;
    const struct ext_power_generic_config *config = dev->config;
    for (size_t i = 0; i < config->control_gpios_count; i++) {
        int rc = gpio_pin_set_dt(&config->control[i], enabled ? 1 : 0);
        if (rc < 0) return rc;
    }
    data->status = enabled;
    return 0;
}

static int ext_power_request(const struct device *dev, bool enabled) {
    int rc = ext_power_apply(dev, enabled);
    if (rc < 0) return rc;
#if IS_ENABLED(CONFIG_SETTINGS)
    struct ext_power_generic_data *data = dev->data;
    if (atomic_set(&data->requested_on, enabled ? 1 : 0) != (enabled ? 1 : 0))
        return ext_power_save_state();
#endif
    return 0;
}

static int ext_power_generic_enable(const struct device *dev) {
    return ext_power_request(dev, true);
}

static int ext_power_generic_disable(const struct device *dev) {
    return ext_power_request(dev, false);
}

static int ext_power_generic_get(const struct device *dev) {
    struct ext_power_generic_data *data = dev->data;
    return data->status;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int ext_power_settings_set_status(const struct device *dev, size_t len,
                                         settings_read_cb read_cb, void *cb_arg) {
    struct ext_power_generic_data *data = dev->data;

    uint8_t requested;
    if (len != sizeof(requested)) return -EINVAL;
    int rc = read_cb(cb_arg, &requested, sizeof(requested));
    if (rc != sizeof(requested)) return rc < 0 ? rc : -EINVAL;
    if (requested > 1) return -EINVAL;
    rc = ext_power_apply(dev, requested != 0);
    if (rc < 0) return rc;
    atomic_set(&data->requested_on, requested);
    return 0;
}

static int ext_power_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const struct device *ext_power = DEVICE_DT_GET(DT_DRV_INST(0));

    const char *next;
    if (settings_name_steq(name, ext_power->name, &next) && !next) {
        return ext_power_settings_set_status(ext_power, len, read_cb, cb_arg);
    }

    return -ENOENT;
}

/* No commit-time save: an absent record means the default enabled state.
 * Loading settings must not create a competing writer on the system queue. */
SETTINGS_STATIC_HANDLER_DEFINE(ext_power, "ext_power/state", NULL, ext_power_settings_set,
                               NULL, NULL);

#endif

static int ext_power_generic_init(const struct device *dev) {
    const struct ext_power_generic_config *config = dev->config;

    for (int i = 0; i < config->control_gpios_count; i++) {
        const struct gpio_dt_spec *gpio = &config->control[i];
        if (gpio_pin_configure_dt(gpio, GPIO_OUTPUT_INACTIVE)) {
            LOG_ERR("Failed to configure ext-power control pin %d", i);
            return -EIO;
        }
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&ext_power_save_work, ext_power_save_state_work);
#endif

    /* Power up before probing devices, without persisting during startup. */
#if IS_ENABLED(CONFIG_SETTINGS)
    atomic_set(&((struct ext_power_generic_data *)dev->data)->requested_on, 1);
#endif
    int rc = ext_power_apply(dev, true);
    if (rc < 0) return rc;

    if (config->init_delay_ms) {
        k_msleep(config->init_delay_ms);
    }

    return 0;
}

#ifdef CONFIG_PM_DEVICE
static int ext_power_generic_pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_RESUME:
#if IS_ENABLED(CONFIG_SETTINGS)
        return ext_power_apply(dev, atomic_get(
            &((struct ext_power_generic_data *)dev->data)->requested_on) != 0);
#else
        return ext_power_apply(dev, true);
#endif
    case PM_DEVICE_ACTION_SUSPEND:
        return ext_power_apply(dev, false);
    default:
        return -ENOTSUP;
    }
}
#endif /* CONFIG_PM_DEVICE */

static const struct gpio_dt_spec ext_power_control_gpios[DT_INST_PROP_LEN(0, control_gpios)] = {
    DT_INST_FOREACH_PROP_ELEM_SEP(0, control_gpios, GPIO_DT_SPEC_GET_BY_IDX, (, ))};

static const struct ext_power_generic_config config = {
    .control = ext_power_control_gpios,
    .control_gpios_count = DT_INST_PROP_LEN(0, control_gpios),
    .init_delay_ms = DT_INST_PROP_OR(0, init_delay_ms, 0)};

static struct ext_power_generic_data data = {
    .status = false,
#if IS_ENABLED(CONFIG_SETTINGS)
    .requested_on = ATOMIC_INIT(1),
#endif
};

static const struct ext_power_api api = {.enable = ext_power_generic_enable,
                                         .disable = ext_power_generic_disable,
                                         .get = ext_power_generic_get};

#define ZMK_EXT_POWER_INIT_PRIORITY 81

PM_DEVICE_DT_INST_DEFINE(0, ext_power_generic_pm_action);
DEVICE_DT_INST_DEFINE(0, ext_power_generic_init, PM_DEVICE_DT_INST_GET(0), &data, &config,
                      POST_KERNEL, ZMK_EXT_POWER_INIT_PRIORITY, &api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
