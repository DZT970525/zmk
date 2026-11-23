/* Host environment for the production C matcher, replay and release code. */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
/* Avoid interactive CRT assertion dialogs on Windows test failures. */
#undef assert
#define assert(condition)                                                                          \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "ASSERT %s:%d: %s\n", __FILE__, __LINE__, #condition);                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define ZMK_KEYMAP_LEN 32
#define CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO 16
#define CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS 16
#define CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS 4
#define CONFIG_ZMK_RUNTIME_COMBO_DEFAULT_TIMEOUT_MS 40
#define CONFIG_ZMK_RUNTIME_COMBO_DEFAULT_REQUIRE_PRIOR_IDLE_MS 0
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_EV_EVENT_HANDLED 1
#define ZMK_EV_EVENT_CAPTURED 2
#define ZMK_COMBOS_LEN 0
#define ZMK_VIRTUAL_KEY_POSITION_COMBO(n) (1000 + (n))
#define BIT(n) (1u << (n))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define IS_ENABLED(x) 0
#define LOG_WRN(...) ((void)0)
enum zmk_runtime_combo_slow_release_override {
    ZMK_RUNTIME_COMBO_SLOW_RELEASE_INHERIT,
    ZMK_RUNTIME_COMBO_SLOW_RELEASE_ON,
    ZMK_RUNTIME_COMBO_SLOW_RELEASE_OFF
};
struct zmk_behavior_binding {
    const char *behavior_dev;
    uint32_t param1, param2;
};
struct zmk_behavior_binding_event {
    uint32_t position;
    int64_t timestamp;
};
struct zmk_position_state_changed {
    uint8_t source;
    uint32_t position;
    bool state;
    int64_t timestamp;
};
struct zmk_position_state_changed_event {
    struct zmk_position_state_changed data;
};
struct zmk_runtime_combo_config {
    bool enabled;
    uint8_t key_position_len;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    uint16_t timeout_ms, require_prior_idle_ms;
    enum zmk_runtime_combo_slow_release_override slow_release_override;
    int32_t key_positions[16];
};
struct zmk_runtime_combo_global_settings {
    uint16_t timeout_ms;
    bool slow_release;
    uint32_t max_combo;
    uint16_t require_prior_idle_ms;
};
struct k_work {
    int unused;
};
struct k_work_delayable {
    int64_t deadline;
};
static struct zmk_runtime_combo_config configs[16];
static struct zmk_runtime_combo_global_settings settings = {.timeout_ms = 40};
static struct zmk_runtime_combo_global_settings runtime_combo_global_cache;
static bool runtime_combo_global_cache_valid;
static int64_t now_ms;
static int raw_down, raw_up, combo_down, combo_up, recursion;
static uint32_t last_press_value, last_release_value;
static int balance[16];
static bool trace;
static bool physical[32];
static int position_state_down(struct zmk_position_state_changed *);
static int position_state_up(struct zmk_position_state_changed *);
static void combo_timeout_handler(struct k_work *);
static void store_last_tapped(int64_t);
static int dispatch(struct zmk_position_state_changed_event *ev, bool after);
#define ZMK_EVENT_RELEASE(ev) dispatch(&(ev), true)
#define ZMK_EVENT_RAISE(ev) dispatch(&(ev), false)
static int64_t k_uptime_get(void) { return now_ms; }
static int k_work_cancel_delayable(struct k_work_delayable *work) {
    work->deadline = 0;
    return 0;
}
static int k_work_schedule(struct k_work_delayable *work, int64_t delay) {
    if (work->deadline)
        return 0;
    work->deadline = now_ms + delay;
    return 1;
}
static uint8_t zmk_keymap_highest_layer_active(void) { return 0; }
static struct zmk_position_state_changed_event
copy_raised_zmk_position_state_changed(const struct zmk_position_state_changed *data) {
    return (struct zmk_position_state_changed_event){.data = *data};
}
static int runtime_combo_cache_read(uint32_t i, struct zmk_runtime_combo_config *out) {
    if (i >= 16 || !configs[i].enabled)
        return -ENOENT;
    *out = configs[i];
    return 0;
}
static int zmk_runtime_combo_read_global_settings(struct zmk_runtime_combo_global_settings *out) {
    *out = settings;
    return 0;
}
static int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event ev, bool down) {
    if (down)
        last_press_value = binding->param1;
    else
        last_release_value = binding->param1;
    int id = ev.position - 1000;
    assert(id >= 0 && id < 16);
    balance[id] += down ? 1 : -1;
    if (down)
        combo_down++;
    else
        combo_up++;
    if (trace)
        printf("  combo %d %s at %lld balance %d\n", id, down ? "down" : "up",
               (long long)ev.timestamp, balance[id]);
    return 0;
}

/* INSERT_PRODUCTION_STATE */
/* INSERT_PRODUCTION_CORE */

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp)
        last_tapped_timestamp = timestamp;
}
static int dispatch(struct zmk_position_state_changed_event *ev, bool after) {
    assert(++recursion < 32);
    if (trace)
        printf("  %s %u %s at %lld\n", after ? "release" : "raise", ev->data.position,
               ev->data.state ? "down" : "up", (long long)ev->data.timestamp);
    int ret =
        after ? ZMK_EV_EVENT_BUBBLE
              : (ev->data.state ? position_state_down(&ev->data) : position_state_up(&ev->data));
    if (ret == ZMK_EV_EVENT_BUBBLE) {
        if (ev->data.state) {
            raw_down++;
            store_last_tapped(ev->data.timestamp);
        } else
            raw_up++;
    }
    recursion--;
    return ret;
}
static void advance(int64_t dt) {
    now_ms += dt;
    for (int i = 0; timeout_task.deadline && timeout_task.deadline <= now_ms; i++) {
        assert(i < 32);
        timeout_task.deadline = 0;
        combo_timeout_handler(NULL);
    }
}
static void key(int p, bool down, int dt) {
    advance(dt);
    physical[p] = down;
    struct zmk_position_state_changed_event ev = {
        .data = {.source = 255, .position = p, .state = down, .timestamp = now_ms}};
    dispatch(&ev, false);
}
static void config(int id, int a, int b, int c, int d) {
    configs[id] = (struct zmk_runtime_combo_config){
        .enabled = true, .key_position_len = 2, .key_positions = {a, b, c, d}};
    if (c >= 0)
        configs[id].key_position_len++;
    if (d >= 0)
        configs[id].key_position_len++;
}
static void reset(void) {
    memset(configs, 0, sizeof(configs));
    memset(active_combos, 0, sizeof(active_combos));
    memset(pending_keys, 0, sizeof(pending_keys));
    memset(balance, 0, sizeof(balance));
    memset(physical, 0, sizeof(physical));
    pending_count = active_combo_count = 0;
    fully_pressed_combo_idx = -1;
    timeout_task_timeout_at = timeout_task.deadline = 0;
    last_tapped_timestamp = last_combo_timestamp = INT32_MIN;
    now_ms = 1000;
    for (int p = 0; p < 32; p++) {
        struct zmk_position_state_changed ev = {.position = p, .timestamp = INT64_MAX};
        position_state_up(&ev);
    }
    settings = (struct zmk_runtime_combo_global_settings){.timeout_ms = 40};
    raw_down = raw_up = combo_down = combo_up = recursion = 0;
    runtime_combo_global_cache_valid = false;
}
static void release_all(void) {
    for (int p = 0; p < 32; p++)
        if (physical[p])
            key(p, false, 1);
    advance(1000);
}
static void dump(void) {
    printf("active=%u pending=%u combo_down=%d combo_up=%d raw_down=%d raw_up=%d\n",
           active_combo_count, pending_count, combo_down, combo_up, raw_down, raw_up);
    for (int i = 0; i < active_combo_count; i++) {
        printf(" active slot %d combo=%u positions:", i, active_combos[i].combo_idx);
        for (int j = 0; j < active_combos[i].key_positions_pressed_count; j++)
            printf(" %u", active_combos[i].key_positions_pressed[j].data.position);
        puts("");
    }
}
static uint32_t randstate = 1234;
static uint32_t rng(void) {
    randstate = 1664525u * randstate + 1013904223u;
    return randstate;
}

static void check_empty(void) {
    assert(active_combo_count == 0 && pending_count == 0);
    for (int i = 0; i < 16; i++)
        assert(balance[i] == 0);
}
static void inject_replay(int position, int64_t timestamp) {
    struct zmk_position_state_changed_event ev = {
        .data = {.source = 255, .position = position, .state = true, .timestamp = timestamp}};
    dispatch(&ev, false);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *mode = argv[1];
    reset();
    if (!strcmp(mode, "normal")) {
        for (int i = 0; i < 8; i++)
            config(i, i * 2, i * 2 + 1, -1, -1);
        for (int n = 0; n < 100000; n++) {
            int a = (n % 8) * 2;
            key(a, true, 100);
            key(a + 1, true, 4);
            key(a, false, 8);
            key(a + 1, false, 3);
        }
        check_empty();
        assert(combo_down == 100000 && combo_up == 100000 && raw_down == 0 && raw_up == 0);
    } else if (!strcmp(mode, "lost")) {
        for (int i = 0; i < 8; i++)
            config(i, i * 2, i * 2 + 1, -1, -1);
        for (int i = 0; i < 4; i++) {
            key(i * 2, true, 100);
            key(i * 2 + 1, true, 2);
            key(i * 2, false, 2);
            physical[i * 2 + 1] = false; /* Drop only this final release. */
        }
        advance(3600000);
        check_empty();
        assert(combo_down == 4 && combo_up == 4);
        for (int i = 0; i < 10; i++) {
            key(8, true, 100);
            key(9, true, 3);
            key(8, false, 3);
            key(9, false, 3);
        }
        check_empty();
        assert(combo_down == 14 && combo_up == 14 && raw_down == 0 && raw_up == 0);
    } else if (!strcmp(mode, "minimal")) {
        config(0, 0, 1, -1, -1);
        config(1, 0, 3, 5, 7);
        key(7, true, 1);
        key(0, true, 2);
        key(0, false, 2);
        assert(pending_count == 0);
        key(0, true, 2);
        advance(100);
        key(0, false, 1);
        key(7, false, 1);
        advance(1000);
        check_empty();
        assert(combo_down == 0 && combo_up == 0 && raw_down == 3 && raw_up == 3);
    } else if (!strcmp(mode, "minimal_full")) {
        for (int i = 0; i < 4; i++) {
            int b = i * 6;
            config(i * 2, b, b + 1, -1, -1);
            config(i * 2 + 1, b, b + 3, b + 4, b + 5);
        }
        config(8, 28, 29, -1, -1);
        for (int i = 0; i < 4; i++) {
            int b = i * 6;
            key(b + 5, true, 1);
            key(b, true, 2);
            key(b, false, 2);
            key(b, true, 2);
            advance(100);
            key(b, false, 1);
            key(b + 5, false, 1);
            advance(100);
        }
        check_empty();
        assert(combo_down == 0 && combo_up == 0 && raw_down == 12 && raw_up == 12);
        advance(3600000);
        for (int i = 0; i < 10; i++) {
            key(28, true, 100);
            key(29, true, 3);
            key(28, false, 3);
            key(29, false, 3);
        }
        check_empty();
        assert(combo_down == 10 && combo_up == 10);
    } else if (!strcmp(mode, "tails")) {
        config(0, 0, 1, -1, -1);
        key(0, true, 1);
        key(1, true, 2);
        int64_t original = now_ms;
        key(0, false, 2);
        check_empty();
        inject_replay(1, original);
        inject_replay(1, original - 1);
        assert(pending_count == 0 && raw_down == 0 && combo_down == 1 && combo_up == 1);
        key(1, false, 2);
        assert(raw_up == 0); /* Delayed tail is swallowed. */
        key(0, true, 100);
        key(1, true, 2);
        key(0, false, 2);
        physical[1] = false;
        key(1, true, 100);
        key(0, true, 2);
        key(1, false, 2);
        key(0, false, 2);
        check_empty();
        assert(combo_down == 3 && combo_up == 3 && raw_down == 0 && raw_up == 0);
    } else if (!strcmp(mode, "slow")) {
        settings.slow_release = true;
        config(0, 0, 1, -1, -1);
        key(0, true, 1);
        key(1, true, 2);
        advance(3600000);
        assert(active_combo_count == 1 && combo_down == 1 && combo_up == 0);
        key(0, false, 2);
        advance(3600000);
        assert(active_combo_count == 1 && combo_up == 0);
        key(1, false, 2);
        check_empty();
        assert(combo_up == 1 && raw_down == 0 && raw_up == 0);
    } else if (!strcmp(mode, "missing_all")) {
        config(0, 0, 1, -1, -1);
        key(0, true, 1);
        int64_t original = now_ms;
        key(1, true, 2);
        inject_replay(0, original);
        inject_replay(0, original - 1);
        assert(active_combo_count == 1 && combo_down == 1 && combo_up == 0);
        physical[0] = physical[1] = false; /* Both releases omitted upstream. */
        key(0, true, 100);
        key(1, true, 2);
        key(0, false, 2);
        key(1, false, 2);
        check_empty();
        assert(combo_down == 2 && combo_up == 2 && raw_down == 0 && raw_up == 0);
    } else if (!strcmp(mode, "snapshot")) {
        config(0, 0, 1, -1, -1);
        configs[0].behavior.param1 = 100;
        key(0, true, 1);
        key(1, true, 2);
        assert(last_press_value == 100);
        config(0, 0, 1, 2, 3);
        configs[0].behavior.param1 = 999;
        settings.slow_release = true;
        runtime_combo_global_cache_valid = false;
        key(0, false, 2);
        assert(combo_up == 1 && last_release_value == 100 && active_combo_count == 0);
        key(1, false, 2);
        check_empty();
        assert(raw_up == 0);
    } else if (!strcmp(mode, "release_timestamps")) {
        config(0, 0, 1, -1, -1);
        key(0, true, 1);
        int64_t original = now_ms;
        key(1, true, 2);
        struct zmk_position_state_changed_event old_up = {
            .data = {.source = 255, .position = 0, .state = false, .timestamp = original - 1}};
        dispatch(&old_up, false);
        assert(active_combo_count == 1 && combo_up == 0);
        key(0, false, 2);
        key(1, false, 2);
        check_empty();
        assert(combo_up == 1);
        reset();
        config(0, 0, 1, -1, -1);
        key(0, true, 1);
        key(1, true, 0);
        key(0, false, 0);
        key(1, false, 0);
        check_empty();
        assert(combo_down == 1 && combo_up == 1 && raw_down == 0 && raw_up == 0);
    } else if (!strcmp(mode, "capacity")) {
        settings.slow_release = true;
        for (int i = 0; i < 4; i++) {
            config(i, i * 2, i * 2 + 1, -1, -1);
            key(i * 2, true, 100);
            key(i * 2 + 1, true, 2);
        }
        assert(active_combo_count == 4);
        config(4, 8, 9, -1, -1);
        config(5, 8, 9, 10, -1);
        key(8, true, 100);
        key(9, true, 2);
        advance(100);
        advance(100);
        assert(raw_down == 2 && combo_down == 4 && active_combo_count == 4);
        key(8, false, 2);
        key(9, false, 2);
        assert(raw_up == 2);
        release_all();
        check_empty();
        assert(combo_down == 4 && combo_up == 4);
    } else if (!strcmp(mode, "fuzz") || !strcmp(mode, "pairs")) {
        for (int seed = 1; seed <= 10000; seed++) {
            reset();
            randstate = seed;
            config(0, 0, 1, -1, -1);
            config(1, 1, 2, -1, -1);
            config(2, 0, 1, 2, -1);
            config(3, 2, 3, 4, 5);
            config(4, 3, 4, -1, -1);
            config(5, 5, 6, -1, -1);
            config(6, 6, 7, -1, -1);
            config(7, 0, 3, 5, 7);
            if (!strcmp(mode, "pairs")) {
                config(2, 0, 2, -1, -1);
                config(3, 2, 3, -1, -1);
                config(7, 0, 7, -1, -1);
            }
            for (int n = 0; n < 1000; n++) {
                int p = (rng() >> 16) % 10, dt = (rng() >> 16) % 80;
                key(p, !physical[p], dt);
            }
            release_all();
            if (active_combo_count || pending_count) {
                printf("FAIL seed=%d\n", seed);
                dump();
                return 1;
            }
            for (int i = 0; i < 16; i++)
                if (balance[i]) {
                    printf("FAIL balance seed=%d combo=%d\n", seed, i);
                    dump();
                    return 2;
                }
        }
    } else {
        fprintf(stderr, "Unknown case %s\n", mode);
        return 2;
    }
    printf("PASS %s\n", mode);
    return 0;
}
