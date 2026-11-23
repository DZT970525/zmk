/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zitaotech/zmk/trackpad_curve.h>
LOG_MODULE_REGISTER(trackpad_curve, CONFIG_ZMK_LOG_LEVEL);
static struct zitaotech_trackpad_curve curve = ZITAOTECH_TRACKPAD_CURVE_DEFAULT;
static struct k_spinlock curve_lock;
static bool device_default_enabled = ZITAOTECH_TRACKPAD_CURVE_DEFAULT_ENABLED;
static bool curve_configured;

void zitaotech_trackpad_curve_set_device_default(bool enabled) {
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    device_default_enabled = enabled;
    if (!curve_configured) curve.enabled = enabled;
    k_spin_unlock(&curve_lock, key);
}


bool zitaotech_trackpad_curve_supported(void) {
    return IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_POINTER_CURVE);
}
void zitaotech_trackpad_curve_get(struct zitaotech_trackpad_curve *out) {
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    *out = curve;
    k_spin_unlock(&curve_lock, key);
}
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_POINTER_CURVE)
static void persist_curve(struct k_work *work) {
    struct zitaotech_trackpad_curve c;
    zitaotech_trackpad_curve_get(&c);
    uint32_t values[] = {1, c.enabled, c.minimum, c.maximum, c.exponent, c.vmax,
                         c.ramp_ms, c.ramp_counts, c.idle_ms, c.smoothing};
    int rc = settings_save_one("zitaotech_padcurve/config", values, sizeof(values));
    if (rc < 0) LOG_ERR("Touchpad curve save failed: %d", rc);
}
K_WORK_DELAYABLE_DEFINE(curve_save_work, persist_curve);
static int load_curve(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "config")) return -ENOENT;
    uint32_t v[10];
    if (len != sizeof(v)) return -EINVAL;
    int rc = read_cb(cb_arg, v, sizeof(v));
    if (rc < 0) return rc;
    if (rc != (int)sizeof(v) || v[0] != 1 || v[1] > 1) return -EINVAL;
    struct zitaotech_trackpad_curve c = {v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]};
    if (!zitaotech_trackpad_curve_valid(&c)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    curve = c;
    curve_configured = true;
    k_spin_unlock(&curve_lock, key);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(zitaotech_padcurve, "zitaotech_padcurve", NULL, load_curve, NULL, NULL);
#endif
int zitaotech_trackpad_curve_set(const struct zitaotech_trackpad_curve *value) {
    if (!zitaotech_trackpad_curve_supported()) return -ENOTSUP;
    if (!zitaotech_trackpad_curve_valid(value)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    curve = *value;
    curve_configured = true;
    k_spin_unlock(&curve_lock, key);
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_POINTER_CURVE)
    k_work_reschedule(&curve_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#endif
    return 0;
}
void zitaotech_trackpad_curve_reset(void) {
    struct zitaotech_trackpad_curve defaults = ZITAOTECH_TRACKPAD_CURVE_DEFAULT;
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    defaults.enabled = device_default_enabled;
    k_spin_unlock(&curve_lock, key);
    (void)zitaotech_trackpad_curve_set(&defaults);
}
