#define DT_DRV_COMPAT avago_a320

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>

#include "a320.h"

LOG_MODULE_REGISTER(A320, CONFIG_SENSOR_LOG_LEVEL);

static int a320_sample_fetch(const struct device *dev, enum sensor_channel chan) { return 0; }

static int a320_channel_get(const struct device *dev, enum sensor_channel chan,
                            struct sensor_value *val) {
    const struct a320_config *cfg = dev->config;
    uint8_t buf[7] = {0};

    // 仅写入寄存器地址 0x0A，不写入任何数据（用于触发）
    uint8_t reg = 0x0A;
    int ret = i2c_write_dt(&cfg->bus, &reg, 1);
    if (ret < 0) {
        LOG_ERR("Failed to write register address 0x0A: %d", ret);
        return ret;
    }

    // 随后从 0x0A 读取 3 字节数据
    ret = i2c_burst_read_dt(&cfg->bus, 0x0A, buf, 7);
    if (ret < 0) {
        LOG_ERR("Failed to read 3 bytes from 0x0A: %d", ret);
        return ret;
    }

    val->val1 = (int8_t)buf[1];
    val->val2 = (int8_t)buf[3];

    LOG_DBG("Read val1 (dx) = %d, val2 (dy) = %d", val->val1, val->val2);
    return 0;
}

static const struct sensor_driver_api a320_driver_api = {
    .sample_fetch = a320_sample_fetch,
    .channel_get = a320_channel_get,
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

#define A320_DEFINE(inst)                                                                          \
    struct a320_data a3200_data_##inst;                                                            \
    static const struct a320_config a3200_cfg_##inst = {.bus = I2C_DT_SPEC_INST_GET(inst)};        \
    DEVICE_DT_INST_DEFINE(inst, a320_init, NULL, &a3200_data_##inst, &a3200_cfg_##inst,            \
                          POST_KERNEL, 60, &a320_driver_api);

DT_INST_FOREACH_STATUS_OKAY(A320_DEFINE)
