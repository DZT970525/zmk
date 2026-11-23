#include <zmk/pointing_profiles.h>
/* SPDX-License-Identifier: MIT
 * The left central owns the saved curve; the right evaluates paired samples.
 * A dedicated relay event keeps the existing TrackPoint mirror ABI intact.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/split_central_status_changed.h>
#include <zitaotech/zmk/pointer_curve.h>
#include <zitaotech/zmk/trackpoint.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zitaotech_pointer_curve curve = {450, 90, 62, 170, true};
static uint8_t rise_rate = ZITAOTECH_POINTER_RISE_RATE_DEFAULT;
static uint8_t release_decay = ZITAOTECH_POINTER_RELEASE_DECAY_DEFAULT;
static struct k_spinlock curve_lock;

uint8_t zitaotech_pointer_rise_rate_get(void) {
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    uint8_t value = rise_rate;
    k_spin_unlock(&curve_lock, key);
    return value;
}
uint8_t zitaotech_pointer_release_decay_get(void) {
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    uint8_t value = release_decay;
    k_spin_unlock(&curve_lock, key);
    return value;
}

void zitaotech_pointer_curve_get(struct zitaotech_pointer_curve *out) {
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    *out = curve;
    k_spin_unlock(&curve_lock, key);
}

void zitaotech_pointer_curve_save(void) {
    struct zitaotech_pointer_curve c;
    zitaotech_pointer_curve_get(&c);
    const uint16_t values[] = {c.enabled, c.vmax, c.exponent, c.minimum, c.maximum};
    int rc = settings_save_one("zitaotech_tpcurve/config", values, sizeof(values));
    if (rc < 0) LOG_WRN("TrackPoint curve save failed: %d", rc);
    uint8_t rates[] = {zitaotech_pointer_rise_rate_get(), zitaotech_pointer_release_decay_get()};
    rc = settings_save_one("zitaotech_tpcurve/rates", rates, sizeof(rates));
    if (rc < 0) LOG_WRN("TrackPoint EMA rates save failed: %d", rc);
}
static void persist(struct k_work *work) {
    ARG_UNUSED(work);
    zitaotech_pointer_curve_save();
}
K_WORK_DELAYABLE_DEFINE(curve_persist_work, persist);

struct zitaotech_pointer_curve_mirror {
    uint16_t vmax, exponent, minimum, maximum;
    uint8_t enabled, source;
};
ZMK_EVENT_DECLARE(zitaotech_pointer_curve_mirror);
ZMK_EVENT_IMPL(zitaotech_pointer_curve_mirror);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_pointer_curve_mirror, tpc, source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_pointer_curve_mirror, tpc, source);

/* A separate relay event preserves the existing curve event's wire size. */
struct zitaotech_pointer_rates_mirror { uint8_t rise, decay, source; };
ZMK_EVENT_DECLARE(zitaotech_pointer_rates_mirror);
ZMK_EVENT_IMPL(zitaotech_pointer_rates_mirror);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_pointer_rates_mirror, tpd, source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_pointer_rates_mirror, tpd, source);
static void send_rates(void) {
    raise_zitaotech_pointer_rates_mirror((struct zitaotech_pointer_rates_mirror){
        .rise = zitaotech_pointer_rise_rate_get(),
        .decay = zitaotech_pointer_release_decay_get(),
        .source = ZMK_RELAY_EVENT_SOURCE_SELF});
}

static void send_curve(void) {
    struct zitaotech_pointer_curve c;
    zitaotech_pointer_curve_get(&c);
    raise_zitaotech_pointer_curve_mirror((struct zitaotech_pointer_curve_mirror){
        .vmax=c.vmax, .exponent=c.exponent, .minimum=c.minimum, .maximum=c.maximum,
        .enabled=c.enabled, .source=ZMK_RELAY_EVENT_SOURCE_SELF});
}

/* Send after the split GATT connection settles; retry so a just-connected
 * peripheral cannot permanently miss its central's saved configuration. */
static uint8_t sync_retries;
static void sync_curve(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(curve_sync_work, sync_curve);
static void sync_curve(struct k_work *work) {
    ARG_UNUSED(work);
    send_curve();
    send_rates();
    if (sync_retries > 0) {
        sync_retries--;
        k_work_reschedule(&curve_sync_work, K_MSEC(500));
    }
}
static void schedule_sync(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    sync_retries = 2;
    k_work_reschedule(&curve_sync_work, K_MSEC(150));
#endif
}

static int update_curve(const struct zitaotech_pointer_curve *c, bool notify) {
    if (!zitaotech_pointer_curve_valid(c)) return -ERANGE;
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    bool changed = curve.enabled != c->enabled || curve.vmax != c->vmax ||
        curve.exponent != c->exponent || curve.minimum != c->minimum || curve.maximum != c->maximum;
    curve = *c;
    k_spin_unlock(&curve_lock, key);
    if (changed) {
        k_work_reschedule(&curve_persist_work, K_MSEC(CONFIG_ZITAOTECH_TRACKPOINT_SAVE_DEBOUNCE_MS));
        if (notify) zitaotech_trackpoint_state_changed();
    }
    return 0;
}
static int update_rates(uint8_t rise, uint8_t decay, bool notify) {
    if (rise < 1 || rise > 100 || decay < 1 || decay > 100) return -ERANGE;
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    bool changed = rise_rate != rise || release_decay != decay;
    rise_rate = rise;
    release_decay = decay;
    k_spin_unlock(&curve_lock, key);
    if (changed) {
        k_work_reschedule(&curve_persist_work, K_MSEC(CONFIG_ZITAOTECH_TRACKPOINT_SAVE_DEBOUNCE_MS));
        if (notify) zitaotech_trackpoint_state_changed();
    }
    return 0;
}
static int set_rates(uint8_t rise, uint8_t decay) {
    if (zmk_pointing_profile_begin) zmk_pointing_profile_begin();
    int rc = update_rates(rise, decay, false);
    if (!rc) { send_rates(); schedule_sync(); }
    if (zmk_pointing_profile_end) zmk_pointing_profile_end();
    return rc;
}
int zitaotech_pointer_rise_rate_set(uint8_t value) {
    return set_rates(value, zitaotech_pointer_release_decay_get());
}
int zitaotech_pointer_release_decay_set(uint8_t value) {
    return set_rates(zitaotech_pointer_rise_rate_get(), value);
}
int zitaotech_pointer_curve_set(const struct zitaotech_pointer_curve *c) {
    if (zmk_pointing_profile_begin) zmk_pointing_profile_begin();
    int rc = update_curve(c, false); /* RPC response already contains fresh state. */
    if (rc == 0) { send_curve(); schedule_sync(); }
    { if (zmk_pointing_profile_end) zmk_pointing_profile_end(); return rc; }
}
void zitaotech_pointer_curve_reset(void) {
    struct zitaotech_pointer_curve c = ZITAOTECH_POINTER_CURVE_DEFAULT;
    zitaotech_pointer_curve_set(&c);
    set_rates(ZITAOTECH_POINTER_RISE_RATE_DEFAULT, ZITAOTECH_POINTER_RELEASE_DECAY_DEFAULT);
}

static int curve_listener(const zmk_event_t *eh) {
    struct zitaotech_pointer_rates_mirror *rates = as_zitaotech_pointer_rates_mirror(eh);
    if (rates && rates->source != ZMK_RELAY_EVENT_SOURCE_SELF) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        update_rates(rates->rise, rates->decay, true);
#endif
    }
    struct zitaotech_pointer_curve_mirror *ev = as_zitaotech_pointer_curve_mirror(eh);
    if (ev && ev->source != ZMK_RELAY_EVENT_SOURCE_SELF) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        struct zitaotech_pointer_curve c = {ev->vmax, ev->exponent, ev->minimum, ev->maximum, ev->enabled != 0};
        update_curve(&c, true);
#endif
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zmk_split_central_status_changed *connection = as_zmk_split_central_status_changed(eh);
    if (connection && connection->connected) schedule_sync();
#endif
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(zitaotech_pointer_curve_apply, curve_listener);
ZMK_SUBSCRIPTION(zitaotech_pointer_curve_apply, zitaotech_pointer_curve_mirror);
ZMK_SUBSCRIPTION(zitaotech_pointer_curve_apply, zitaotech_pointer_rates_mirror);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(zitaotech_pointer_curve_apply, zmk_split_central_status_changed);
#endif

static int curve_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    if (!strcmp(name, "rates")) {
        uint8_t rates[2];
        if (len != sizeof(rates)) return -EINVAL;
        int rc = read_cb(arg, rates, sizeof(rates));
        if (rc != sizeof(rates) || rates[0] < 1 || rates[0] > 100 || rates[1] < 1 || rates[1] > 100) return -EINVAL;
        k_spinlock_key_t key = k_spin_lock(&curve_lock);
        rise_rate = rates[0]; release_decay = rates[1];
        k_spin_unlock(&curve_lock, key);
        return 0;
    }
    if (strcmp(name, "config")) return -ENOENT;
    uint16_t values[5];
    if (len != sizeof(values)) return -EINVAL;
    int rc = read_cb(arg, values, sizeof(values));
    if (rc != sizeof(values)) return rc < 0 ? rc : -EINVAL;
    struct zitaotech_pointer_curve c = {values[1], values[2], values[3], values[4], values[0] != 0};
    if (values[0] > 1 || !zitaotech_pointer_curve_valid(&c)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&curve_lock);
    curve = c;
    k_spin_unlock(&curve_lock, key);
    return 0;
}
static int curve_settings_commit(void) { schedule_sync(); return 0; }
SETTINGS_STATIC_HANDLER_DEFINE(zitaotech_tpcurve, "zitaotech_tpcurve", NULL,
                               curve_settings_set, curve_settings_commit, NULL);
