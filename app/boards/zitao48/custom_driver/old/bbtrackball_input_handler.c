/*
 * bbtrackball_input_handler.c - Handle BB trackball sensor data
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>

LOG_MODULE_REGISTER(bbtrackball_input_handler, LOG_LEVEL_INF);

/* ==== GPIO Pin Definitions ==== */
#define TRACKBALL_GPIO_NODE DT_NODELABEL(gpio0)
#define LEFT_GPIO_PIN 24
#define RIGHT_GPIO_PIN 20
#define UP_GPIO_PIN 17
#define DOWN_GPIO_PIN 8

/* ==== Trackball Movement Config ==== */
#define base_move_pixels 2
#define exponential_base 1.2f /* 从 1.2f 降低到 1.1f，减缓加速度曲线 */
#define speed_scale 75.0f     /* 从 100 降低到 50，整体加速度减半 */

/* ==== Direction Struct ==== */
typedef struct {
    const struct device *gpio_dev;
    int pins[2];
    int current_actions[2];
    int last_actions[2];
    double move_multiply;
    uint32_t current_action_times[2];
    uint32_t last_action_times[2];
} Direction;

static Direction x_direction;
static Direction y_direction;
static int x_move, y_move;

/* ==== Direction Init ==== */
static void Direction_init(Direction *dir, int pin1, int pin2) {
    dir->gpio_dev = DEVICE_DT_GET(TRACKBALL_GPIO_NODE);
    dir->pins[0] = pin1;
    dir->pins[1] = pin2;

    gpio_pin_configure(dir->gpio_dev, pin1, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(dir->gpio_dev, pin2, GPIO_INPUT | GPIO_PULL_UP);

    /* 初始化 last_actions，避免第一次读到随机值 */
    for (int i = 0; i < 2; i++) {
        dir->last_actions[i] = gpio_pin_get(dir->gpio_dev, dir->pins[i]);
        dir->last_action_times[i] = k_uptime_get_32();
    }
}

/* ==== Read Movement (平滑加速度+上限) ==== */
static int Direction_read_action(Direction *dir) {
    for (int i = 0; i < 2; ++i) {
        dir->current_actions[i] = gpio_pin_get(dir->gpio_dev, dir->pins[i]);
        dir->current_action_times[i] = k_uptime_get_32();

        if (dir->current_actions[i] != dir->last_actions[i]) {
            uint32_t delta = dir->current_action_times[i] - dir->last_action_times[i];
            if (delta == 0)
                delta = 1; /* 避免除零 */

            /* === 使用 powf 平滑加速度 === */
            float speed_factor = speed_scale / (float)delta;
            dir->move_multiply = powf(exponential_base, speed_factor);

            /* 保存状态 */
            dir->last_actions[i] = dir->current_actions[i];
            dir->last_action_times[i] = dir->current_action_times[i];

            /* 返回方向 */
            if (i == 0) {
                return (-1) * base_move_pixels * dir->move_multiply;
            } else {
                return base_move_pixels * dir->move_multiply;
            }
        }
    }
    return 0;
}

/* ==== Trackball Thread ==== */
static void bbtrackball_thread_main(void) {
    while (1) {
        x_move = Direction_read_action(&x_direction);
        y_move = Direction_read_action(&y_direction);
        LOG_DBG("x_move is %d", x_move);
        LOG_DBG("y_move is %d", y_move);
        LOG_INF("NIUBI");
        if (x_move || y_move) {
            zmk_hid_mouse_scroll_set(0, 0);
            zmk_hid_mouse_movement_set(0, 0);
            zmk_hid_mouse_movement_update(x_move, y_move);
            zmk_endpoints_send_mouse_report();
        }

        k_sleep(K_MSEC(5)); /* 提高轮询频率 */
    }
}

K_THREAD_STACK_DEFINE(bbtrackball_thread_stack, 1024);
static struct k_thread bbtrackball_thread_data;

/* ==== Driver Init ==== */
static int bbtrackball_input_init(void) {
    LOG_INF("Initializing BBtrackball input handler...");

    Direction_init(&x_direction, LEFT_GPIO_PIN, RIGHT_GPIO_PIN);
    Direction_init(&y_direction, UP_GPIO_PIN, DOWN_GPIO_PIN);

    k_thread_create(&bbtrackball_thread_data, bbtrackball_thread_stack,
                    K_THREAD_STACK_SIZEOF(bbtrackball_thread_stack),
                    (k_thread_entry_t)bbtrackball_thread_main, NULL, NULL, NULL, K_PRIO_PREEMPT(0),
                    0, K_NO_WAIT);

    LOG_INF("BBtrackball input handler thread started.");
    return 0;
}

/* 注册为应用初始化 */
SYS_INIT(bbtrackball_input_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
