#pragma once
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>

struct a320_data {
    struct gpio_callback gpio_cb;
    const struct device *dev;
    uint8_t btn;
    struct k_work work;
};
struct a320_dev_config {
    struct i2c_dt_spec i2c;
    struct gpio_dt_spec rdy_gpio;
};
struct a320_config {
    struct i2c_dt_spec i2c;
    struct k_mutex polling_mutex;
#if DT_INST_NODE_HAS_PROP(0, nrst_gpios)
    struct gpio_dt_spec nrst_gpio;
#endif
#if DT_INST_NODE_HAS_PROP(0, motion_gpios)
    struct gpio_dt_spec motion_gpio;
#endif
#if DT_INST_NODE_HAS_PROP(0, orient_gpios)
    struct gpio_dt_spec orient_gpio;
#endif
#if DT_INST_NODE_HAS_PROP(0, shutdown_gpios)
    struct gpio_dt_spec shutdown_gpio;
#endif
};

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

bool tp_is_touched(void);

#ifdef __cplusplus
}
#endif
/* Detection */
#define BIT_MOTION_MOT (1 << 7)
#define BIT_MOTION_OVF (1 << 4)
