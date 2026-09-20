#define DT_DRV_COMPAT avago_a320

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/input/input.h>
#include <zephyr/pm/device.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(A320, CONFIG_SENSOR_LOG_LEVEL);

/* 驱动内部使用的结构体定义 */
struct a320_data {
    uint16_t x_position;
    uint16_t y_position;
};

struct a320_config {
    struct i2c_dt_spec bus;
};

/* 采样函数（当前未做任何采集动作） */
static int a320_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    ARG_UNUSED(dev);
    ARG_UNUSED(chan);
    return 0;
}

/* 获取通道数据（dx, dy） */
static int a320_channel_get(const struct device *dev, enum sensor_channel chan,
                            struct sensor_value *val) {
    const struct a320_config *cfg = dev->config;
    uint8_t buf[A320_BURST_SIZE] = {0};

    /* 触发读取：先写寄存器地址 0x0A */
    uint8_t reg = 0x0A;
    int ret = i2c_write_dt(&cfg->bus, &reg, 1);
    if (ret < 0) {
        LOG_ERR("Failed to write register address 0x0A: %d", ret);
        return ret;
    }

    /* 再从 0x0A 连续读取 7 字节数据 */
    ret = i2c_burst_read_dt(&cfg->bus, 0x0A, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Failed to read from 0x0A: %d", ret);
        returx ret;
    }

    /* 提取 dx, dy */
    val->val1 = (int8_t)buf[1];
    val->val2 = (int8_t)buf[3];

    LOG_DBG("dx = %d, dy = %d", val->val1, val->val2);
    return 0;
}

static const struct sensor_driver_api a320_driver_api = {
    .sample_fetch = a320_sample_fetch,
};

/* 初始化函数 */
static int a320_init(const struct device *dev) {
    const struct a320_config *cfg = dev->config;

    if (!device_is_ready(cfg->bus.bus)) {
        LOG_ERR("I2C bus %s is not ready!", cfg->bus.bus->name);
        return -EINVAL;
    }

    LOG_DBG("A320 Init done, Ready to read data.");
    return 0;
}

/* 设备实例化宏 */
#define A320_DEFINE(inst)                                                                          \
    struct a320_data a320_data_##inst;                                                             \
    static const struct a320_config a320_cfg_##inst = {                                            \
        .bus = I2C_DT_SPEC_INST_GET(inst),                                                         \
        .irq_gpio = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                           \
        .evt_type = DT_PROP(DT_DRV_INST(n), evt_type),                                             \
        .x_input_code = DT_PROP(DT_DRV_INST(n), x_input_code),                                     \
        .y_input_code = DT_PROP(DT_DRV_INST(n), y_input_code),                                     \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, a320_init, NULL, &a320_data_##inst, &a320_cfg_##inst, POST_KERNEL, \
                          60, &a320_driver_api);

DT_INST_FOREACH_STATUS_OKAY(A320_DEFINE)

#define GET_A320_DEV(node_id) DEVICE_DT_GET(node_id),

static const struct device *a320_devs[] = {DT_FOREACH_STATUS_OKAY(avago_a320, GET_A320_DEV)};

static int a320_set_interrupt(const struct device *dev, const bool en) {
    const struct avago_config *config = dev->config;
    int ret = gpio_pin_interrupt_configure_dt(&config->irq_gpio,
                                              en ? GPIO_INT_LEVEL_ACTIVE : GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_ERR("can't set interrupt");
    }
    return ret;
}

static int a320_report_data(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    uint8_t buf[PMW3610_BURST_SIZE];

    if (unlikely(!data->ready)) {
        LOG_WRN("Device is not initialized yet");
        return -EBUSY;
    }

    static int64_t dx = 0;
    static int64_t dy = 0;

#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0
    static int64_t last_smp_time = 0;
    static int64_t last_rpt_time = 0;
    int64_t now = k_uptime_get();
#endif

    int err = pmw3610_read(dev, PMW3610_REG_MOTION_BURST, buf, PMW3610_BURST_SIZE);
    if (err) {
        return err;
    }
    // LOG_HEXDUMP_DBG(buf, PMW3610_BURST_SIZE, "buf");

// 12-bit two's complement value to int16_t
// adapted from https://stackoverflow.com/questions/70802306/convert-a-12-bit-signed-number-in-c
#define TOINT16(val, bits) (((struct { int16_t value : bits; }){val}).value)

    int16_t x = TOINT16((buf[PMW3610_X_L_POS] + ((buf[PMW3610_XY_H_POS] & 0xF0) << 4)), 12);
    int16_t y = TOINT16((buf[PMW3610_Y_L_POS] + ((buf[PMW3610_XY_H_POS] & 0x0F) << 8)), 12);
    LOG_DBG("x/y: %d/%d", x, y);

#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0
    // purge accumulated delta, if last sampled had not been reported on last report tick
    if (now - last_smp_time >= CONFIG_PMW3610_REPORT_INTERVAL_MIN) {
        dx = 0;
        dy = 0;
    }
    last_smp_time = now;
#endif

    // accumulate delta until report in next iteration
    dx += x;
    dy += y;

#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0
    // strict to report inerval
    if (now - last_rpt_time < CONFIG_PMW3610_REPORT_INTERVAL_MIN) {
        return 0;
    }
#endif

    // fetch report value
    int16_t rx = (int16_t)CLAMP(dx, INT16_MIN, INT16_MAX);
    int16_t ry = (int16_t)CLAMP(dy, INT16_MIN, INT16_MAX);
    bool have_x = rx != 0;
    bool have_y = ry != 0;

    if (have_x || have_y) {
#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0
        last_rpt_time = now;
#endif
        dx = 0;
        dy = 0;
        if (have_x) {
            input_report(dev, config->evt_type, config->x_input_code, rx, !have_y, K_NO_WAIT);
        }
        if (have_y) {
            input_report(dev, config->evt_type, config->y_input_code, ry, true, K_NO_WAIT);
        }
    }

    return err;
}

static void a320_gpio_callback(const struct device *gpiob, struct gpio_callback *cb,
                               uint32_t pins) {
    struct avago_data *data = CONTAINER_OF(cb, struct avago_data, irq_gpio_cb);
    const struct device *dev = data->dev;
    a320_set_interrupt(dev, false);
    k_work_submit(&data->trigger_work);
}

static void a320_work_callback(struct k_work *work) {
    struct avago_data *data = CONTAINER_OF(work, struct avago_data, trigger_work);
    const struct device *dev = data->dev;
    a320_report_data(dev);
    a320_set_interrupt(dev, true);
}

static int a320_init_irq(const struct device *dev) {
    int err;
    struct avago_data *data = dev->data;
    const struct avago_config *config = dev->config;

    // check readiness of irq gpio pin
    if (!device_is_ready(config->irq_gpio.port)) {
        LOG_ERR("IRQ GPIO device not ready");
        return -ENODEV;
    }

    // init the irq pin
    err = gpio_pin_configure_dt(&config->irq_gpio, GPIO_INPUT);
    if (err) {
        LOG_ERR("Cannot configure IRQ GPIO");
        return err;
    }

    // setup and add the irq callback associated
    gpio_init_callback(&data->irq_gpio_cb, a320_gpio_callback, BIT(config->irq_gpio.pin));

    err = gpio_add_callback(config->irq_gpio.port, &data->irq_gpio_cb);
    if (err) {
        LOG_ERR("Cannot add IRQ GPIO callback");
    }

    return err;
}


static int a320_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    ARG_UNUSED(dev);
    ARG_UNUSED(chan);
    return 0;
}

static const struct sensor_driver_api a320_driver_api = {
    .sample_fetch = a320_sample_fetch,
};

static int a320_init(const struct device *dev) {
    const struct a320_config *cfg = dev->config;

    if (!device_is_ready(cfg->bus.bus)) {
        LOG_ERR("I2C bus %s is not ready!", cfg->bus.bus->name);
        return -EINVAL;
    }

    LOG_DBG("A320 Init done, Ready to read data.");
    return 0;
}


static int a320_init(const struct device *dev) {
    struct avago_data *data = dev->data;
    const struct avago_config *config = dev->config;
    int err;

	if (!spi_is_ready_dt(&config->bus.bus)) {
		LOG_ERR("%s is not ready", config->spi.bus->name);
		return -ENODEV;
	}

    // init device pointer
    data->dev = dev;


    // init trigger handler work
    k_work_init(&data->trigger_work, pmw3610_work_callback);

    // init irq routine
    err = pmw3610_init_irq(dev);
    if (err) {
        return err;
    }

    k_work_init_delayable(&data->init_work, pmw3610_async_init);

    k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));

    return err;
}