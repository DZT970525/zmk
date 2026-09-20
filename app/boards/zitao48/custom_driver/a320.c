#define DT_DRV_COMPAT avago_a320

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
    uint8_t buf[7] = {0};

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
        return ret;
    }

    /* 提取 dx, dy */
    val->val1 = (int8_t)buf[1];
    val->val2 = (int8_t)buf[3];

    LOG_DBG("dx = %d, dy = %d", val->val1, val->val2);
    return 0;
}

static const struct sensor_driver_api a320_driver_api = {
    .sample_fetch = a320_sample_fetch,
    .channel_get = a320_channel_get,
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
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, a320_init, NULL, &a320_data_##inst, &a320_cfg_##inst, POST_KERNEL, \
                          60, &a320_driver_api);

DT_INST_FOREACH_STATUS_OKAY(A320_DEFINE)
