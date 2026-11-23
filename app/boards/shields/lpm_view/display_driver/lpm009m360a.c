/*
 * Copyright (c) 2023 Taisheng WANG <wstrn66@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT jdi_lpm009m360a

#include "lpm009m360a.h"

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/display.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(display_lpm009m360a, CONFIG_DISPLAY_LOG_LEVEL);

#define LPM009M360A_RESET_TIME K_MSEC(1)
#define LPM009M360A_EXIT_SLEEP_TIME K_MSEC(1)

/*
 * The panel is always addressed through its native geometry: one gate line
 * carries 72 dots (each dot is made of a red, a green and a blue sub pixel)
 * and there are 144 gate lines.
 */
#define LPM009M360A_DOTS_PER_LINE 72
#define LPM009M360A_LINE_COUNT 144

/* Bytes transferred for one gate line in the supported data modes. */
#define LPM009M360A_LINE_BYTES_1BIT 9  /* 72 x 1 bit */
#define LPM009M360A_LINE_BYTES_3BIT 27 /* 72 x 3 bit */
#define LPM009M360A_LINE_BYTES_4BIT 36 /* 72 x 4 bit */

#define LPM009M360A_MAX_LINE_BYTES LPM009M360A_LINE_BYTES_4BIT

/* Mandatory number of SCLK cycles after the last data bit of a line. */
#define LPM009M360A_TRANSFER_CLOCKS 16

struct lpm009m360a_data {
    atomic_t hold_frame;
    /* Shadow of the panel pixel memory, one gate line after the other. */
    uint8_t buf[LPM009M360A_LINE_COUNT * LPM009M360A_MAX_LINE_BYTES];
    /* The shadow can only be trusted after it was written to the panel once. */
    bool synced;
};

struct lpm009m360a_config {
    struct spi_dt_spec bus;
    uint16_t height;
    uint16_t width;
    int rotation;
    int reverse;

    uint8_t color_mode[1];
};

static int lpm009m360a_transmit_hold(const struct device *dev, uint8_t cmd, uint8_t arg,
                                     const uint8_t *tx_data, size_t tx_count) {
    const struct lpm009m360a_config *config = dev->config;
    struct spi_buf tx_buf = {.buf = &cmd, .len = 1};
    struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};
    int ret;

    ret = spi_write_dt(&config->bus, &tx_bufs);
    tx_buf.buf = &arg;
    ret = spi_write_dt(&config->bus, &tx_bufs);
    if (ret < 0) {
        return ret;
    }

    if (tx_data != NULL) {
        tx_buf.buf = (void *)tx_data;
        tx_buf.len = tx_count;
        ret = spi_write_dt(&config->bus, &tx_bufs);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

static int lpm009m360a_transmit(const struct device *dev, uint8_t cmd, uint8_t arg,
                                const uint8_t *tx_data, size_t tx_count) {
    const struct lpm009m360a_config *config = dev->config;
    int ret;

    ret = lpm009m360a_transmit_hold(dev, cmd, arg, tx_data, tx_count);
    spi_release_dt(&config->bus);
    return ret;
}

static int lpm009m360a_end_transfer(const struct device *dev, uint8_t cmd) {
    const struct lpm009m360a_config *config = dev->config;

    /*
     * No-update commands keep SCLK running long enough for the panel to latch
     * the data that has just been shifted in.
     */
    for (uint8_t i = 0; i < (LPM009M360A_TRANSFER_CLOCKS + 15) / 16; i++) {
        lpm009m360a_transmit_hold(dev, cmd, 0, NULL, 0);
    }
    spi_release_dt(&config->bus);

    return 0;
}

/*
 * Send one gate line: the mode/address prefix and the line data are put into a
 * single SPI transaction instead of one transaction per byte group.
 */
static int lpm009m360a_write_line(const struct device *dev, uint8_t cmd, uint8_t gate,
                                  const uint8_t *line, size_t len) {
    const struct lpm009m360a_config *config = dev->config;
    uint8_t tx[2 + LPM009M360A_MAX_LINE_BYTES];
    struct spi_buf tx_buf = {.buf = tx, .len = len + 2};
    struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

    tx[0] = cmd;
    tx[1] = gate;
    memcpy(&tx[2], line, len);

    return spi_write_dt(&config->bus, &tx_bufs);
}

static int lpm009m360a_exit_sleep(const struct device *dev) {
    LOG_DBG("Exiting sleep mode");
    k_sleep(LPM009M360A_EXIT_SLEEP_TIME);
    return 0;
}

static int lpm009m360a_sleep(const struct device *dev) {
    LOG_DBG("Sleeping display, but not really");
    return 0;
}

static int lpm009m360a_reset_display(const struct device *dev) {
    struct lpm009m360a_data *data = dev->data;
    if (atomic_get(&data->hold_frame)) return 0;

    LOG_DBG("Resetting display");
    lpm009m360a_transmit(dev, LPM009M360A_CMD_ALL_CLEAR, 0, NULL, 0);

    /* The panel memory is black now, so the shadow starts empty and has to be
     * written completely with the next frame. */
    memset(data->buf, 0x00, sizeof(data->buf));
    data->synced = false;

    k_sleep(LPM009M360A_RESET_TIME);

    return 0;
}

static int lpm009m360a_blanking_on(const struct device *dev) { return lpm009m360a_sleep(dev); }

static int lpm009m360a_blanking_off(const struct device *dev) {
    return lpm009m360a_exit_sleep(dev);
}

static int lpm009m360a_read(const struct device *dev, const uint16_t x, const uint16_t y,
                            const struct display_buffer_descriptor *desc, void *buf) {
    return -ENOTSUP;
}

static inline uint8_t lpm009m360a_bits_per_dot(uint8_t color_mode) {
    switch (color_mode) {
    case LPM009M360A_DATA_MODE_1BIT:
        return 1;
    case LPM009M360A_DATA_MODE_4BIT:
        return 4;
    default:
        return 3;
    }
}

static inline bool lpm009m360a_is_color(const struct lpm009m360a_config *config) {
    return lpm009m360a_bits_per_dot(config->color_mode[0]) > 1;
}

static inline size_t lpm009m360a_line_bytes(uint8_t color_mode) {
    return (size_t)LPM009M360A_DOTS_PER_LINE * lpm009m360a_bits_per_dot(color_mode) / 8;
}

/*
 * Write every gate line of the shadow to the panel. Used once after the reset,
 * because until then the content of the panel memory is unknown: the power up
 * state of the panel is not defined and the all clear command may have been
 * missed if the panel was not powered yet.
 */
static int lpm009m360a_sync_panel(const struct device *dev, uint8_t cmd) {
    const struct lpm009m360a_config *config = dev->config;
    struct lpm009m360a_data *data = dev->data;
    const size_t len = lpm009m360a_line_bytes(config->color_mode[0]);
    int ret = 0;

    for (uint16_t line = 0; line < LPM009M360A_LINE_COUNT; line++) {
        ret = lpm009m360a_write_line(dev, cmd, line + 1, &data->buf[line * len], len);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

/*
 * The pixel memory of one dot is transferred as red, green and blue (plus a
 * dummy bit in 4 bit data mode), red first and MSB first.
 */
static inline void lpm009m360a_put_dot(uint8_t *line, uint16_t dot, uint8_t bits_per_dot,
                                       uint8_t rgb) {
    uint32_t start = (uint32_t)dot * bits_per_dot;

    for (uint8_t i = 0; i < bits_per_dot; i++) {
        uint32_t pos = start + i;
        bool on = (i < 3) && (((rgb >> (2 - i)) & 0x1) != 0);

        if (on) {
            line[pos >> 3] |= BIT(7 - (pos & 0x7));
        } else {
            line[pos >> 3] &= (uint8_t)~BIT(7 - (pos & 0x7));
        }
    }
}

/* Pick the closest of the 8 colors the panel is able to display. */
static inline uint8_t lpm009m360a_rgb565_to_rgb111(uint16_t pixel) {
    uint8_t red = (pixel >> 11) & 0x1f;
    uint8_t green = (pixel >> 5) & 0x3f;
    uint8_t blue = pixel & 0x1f;

    return ((red & 0x10) ? 0x4 : 0x0) | ((green & 0x20) ? 0x2 : 0x0) |
           ((blue & 0x10) ? 0x1 : 0x0);
}

static int lpm009m360a_write_mono(const struct device *dev, uint8_t cmd,
                                  const struct display_buffer_descriptor *desc, const void *buf,
                                  uint16_t x, uint16_t y, bool *wrote) {
    const struct lpm009m360a_config *config = dev->config;
    struct lpm009m360a_data *data = dev->data;
    const uint8_t *source_buf8 = (uint8_t *)buf;
    const size_t len = lpm009m360a_line_bytes(config->color_mode[0]);
    int ret = 0;

    if (config->rotation == 0) {
        for (uint8_t i = 0; i < desc->height; i++) {
            for (uint8_t j = 0; j < desc->width; j++) {
                data->buf[(y + i) * len + (x / 8 + j)] = source_buf8[i * 9 + j];
            }
        }
        for (uint8_t i = 0; i < desc->height; i++) {
            ret = lpm009m360a_transmit_hold(dev, cmd, i + y + 1,
                                            (uint8_t *)&data->buf[(y + i) * len], len);
            *wrote = true;
        }
    } else if (config->rotation == 1) {
        for (uint8_t i = 0; i < (desc->height) / 8; i++) {
            for (uint8_t j = 0; j < desc->width; j++) {
                data->buf[(LPM009M360A_LINE_COUNT - 1 - x - j) * len + y / 8 + i] =
                    source_buf8[i * desc->width + j];
            }
        }
        for (uint8_t i = 0; i < desc->width; i++) {
            ret = lpm009m360a_transmit_hold(
                dev, cmd, LPM009M360A_LINE_COUNT - x - i,
                (uint8_t *)&data->buf[(LPM009M360A_LINE_COUNT - 1 - x - i) * len], len);
            *wrote = true;
        }
    }

    return ret;
}

static int lpm009m360a_write_color(const struct device *dev, uint8_t cmd,
                                   const struct display_buffer_descriptor *desc, const void *buf,
                                   uint16_t x, uint16_t y, bool *wrote) {
    const struct lpm009m360a_config *config = dev->config;
    struct lpm009m360a_data *data = dev->data;
    const uint16_t *source_buf16 = (uint16_t *)buf;
    const uint8_t bits = lpm009m360a_bits_per_dot(config->color_mode[0]);
    const size_t len = lpm009m360a_line_bytes(config->color_mode[0]);
    int ret = 0;

    if (config->rotation == 0) {
        /* One gate line holds one line of the framebuffer. */
        for (uint16_t i = 0; i < desc->height; i++) {
            uint8_t *line = &data->buf[(y + i) * len];
            uint8_t old[LPM009M360A_MAX_LINE_BYTES];

            memcpy(old, line, len);
            for (uint16_t j = 0; j < desc->width; j++) {
                lpm009m360a_put_dot(line, x + j, bits,
                                    lpm009m360a_rgb565_to_rgb111(source_buf16[i * desc->width + j]));
            }
            /* The panel already shows the shadow, so unchanged lines are skipped. */
            if (memcmp(old, line, len) == 0) {
                continue;
            }

            ret = lpm009m360a_write_line(dev, cmd, y + i + 1, line, len);
            if (ret < 0) {
                return ret;
            }
            *wrote = true;
        }
    } else if (config->rotation == 1) {
        /*
         * The panel is used rotated by 90 degrees: one column of the
         * framebuffer is one gate line (addressed from the other end of the
         * panel) and one row of the framebuffer is one dot of that line.
         */
        for (uint16_t i = 0; i < desc->width; i++) {
            uint8_t *line = &data->buf[(LPM009M360A_LINE_COUNT - 1 - x - i) * len];
            uint8_t old[LPM009M360A_MAX_LINE_BYTES];

            memcpy(old, line, len);
            for (uint16_t j = 0; j < desc->height; j++) {
                lpm009m360a_put_dot(line, y + j, bits,
                                    lpm009m360a_rgb565_to_rgb111(source_buf16[j * desc->width + i]));
            }
            /* The panel already shows the shadow, so unchanged lines are skipped. */
            if (memcmp(old, line, len) == 0) {
                continue;
            }

            ret = lpm009m360a_write_line(dev, cmd, LPM009M360A_LINE_COUNT - x - i, line, len);
            if (ret < 0) {
                return ret;
            }
            *wrote = true;
        }
    }

    return ret;
}

static int lpm009m360a_write(const struct device *dev, const uint16_t x, const uint16_t y,
                             const struct display_buffer_descriptor *desc, const void *buf) {
    const struct lpm009m360a_config *config = dev->config;
    struct lpm009m360a_data *data = dev->data;
    const uint8_t cmd = LPM009M360A_CMD_UPDATE | (config->color_mode[0] << 2);
    if (atomic_get(&data->hold_frame)) return 0;
    bool wrote = false;
    int ret;

    if ((x + desc->width > config->width) || (y + desc->height > config->height)) {
        LOG_ERR("Write outside of the display area");
        return -EINVAL;
    }

    if (lpm009m360a_is_color(config)) {
        ret = lpm009m360a_write_color(dev, cmd, desc, buf, x, y, &wrote);
    } else {
        ret = lpm009m360a_write_mono(dev, cmd, desc, buf, x, y, &wrote);
    }

    if (ret < 0) {
        return ret;
    }

    if (!data->synced) {
        /* first frame after the reset: the whole panel has to be written once,
         * after that it is enough to send the gate lines that changed */
        data->synced = true;
        ret = lpm009m360a_sync_panel(dev, cmd);
        if (ret < 0) {
            return ret;
        }
        wrote = true;
    }

    if (!wrote) {
        /* The panel keeps the last image, there is nothing to latch. */
        return 0;
    }

    return lpm009m360a_end_transfer(dev, LPM009M360A_CMD_NO_UPDATE);
}

static void *lpm009m360a_get_framebuffer(const struct device *dev) { return NULL; }

static int lpm009m360a_set_brightness(const struct device *dev, const uint8_t brightness) {
    return -ENOTSUP;
}

static int lpm009m360a_set_contrast(const struct device *dev, const uint8_t contrast) {
    return -ENOTSUP;
}

static void lpm009m360a_get_capabilities(const struct device *dev,
                                         struct display_capabilities *capabilities) {
    const struct lpm009m360a_config *config = dev->config;

    memset(capabilities, 0, sizeof(struct display_capabilities));
    capabilities->x_resolution = config->width;
    capabilities->y_resolution = config->height;

    if (lpm009m360a_is_color(config)) {
        capabilities->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
        capabilities->current_pixel_format = PIXEL_FORMAT_RGB_565;
    } else {
        capabilities->supported_pixel_formats = PIXEL_FORMAT_MONO01 | PIXEL_FORMAT_MONO10;
        if (config->reverse) {
            capabilities->current_pixel_format = PIXEL_FORMAT_MONO01;
        } else {
            capabilities->current_pixel_format = PIXEL_FORMAT_MONO10;
        }
        if (config->rotation == 0) {
            capabilities->screen_info = SCREEN_INFO_X_ALIGNMENT_WIDTH | SCREEN_INFO_MONO_MSB_FIRST;
        } else if (config->rotation == 1) {
            capabilities->screen_info = SCREEN_INFO_MONO_VTILED | SCREEN_INFO_MONO_MSB_FIRST;
        }
    }
    capabilities->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int lpm009m360a_set_pixel_format(const struct device *dev,
                                        const enum display_pixel_format pixel_format) {

    LOG_ERR("Pixel format change not implemented");

    return -ENOTSUP;
}

static int lpm009m360a_set_orientation(const struct device *dev,
                                       const enum display_orientation orientation) {
    if (orientation == DISPLAY_ORIENTATION_NORMAL) {
        return 0;
    }

    LOG_ERR("Changing display orientation not implemented");

    return -ENOTSUP;
}

static int lpm009m360a_init(const struct device *dev) {
    const struct lpm009m360a_config *config = dev->config;
    int ret;

    LOG_INF("initializing in %s mode", lpm009m360a_is_color(config) ? "8 color" : "monochrome");

    ret = lpm009m360a_reset_display(dev);
    if (ret) {
        LOG_ERR("Couldn't reset display");
        return ret;
    }
    LOG_INF("initialized");
    return 0;
}

#ifdef CONFIG_PM_DEVICE
static int lpm009m360a_pm_action(const struct device *dev, enum pm_device_action action) {
    int ret = 0;

    switch (action) {
    case PM_DEVICE_ACTION_RESUME:
        LOG_INF("resume");
        lpm009m360a_exit_sleep(dev);
        break;
    case PM_DEVICE_ACTION_SUSPEND:
        LOG_INF("suspend");
        lpm009m360a_sleep(dev);
        break;
    case PM_DEVICE_ACTION_TURN_OFF:
        LOG_INF("turn off");
        break;
    case PM_DEVICE_ACTION_TURN_ON:
        lpm009m360a_init(dev);
        LOG_INF("turn on");
        break;
    default:
        ret = -ENOTSUP;
        break;
    }

    return ret;
}
#endif /* CONFIG_PM_DEVICE */

static const struct display_driver_api lpm009m360a_api = {
    .blanking_on = lpm009m360a_blanking_on,
    .blanking_off = lpm009m360a_blanking_off,
    .write = lpm009m360a_write,
    .read = lpm009m360a_read,
    .get_framebuffer = lpm009m360a_get_framebuffer,
    .set_brightness = lpm009m360a_set_brightness,
    .set_contrast = lpm009m360a_set_contrast,
    .get_capabilities = lpm009m360a_get_capabilities,
    .set_pixel_format = lpm009m360a_set_pixel_format,
    .set_orientation = lpm009m360a_set_orientation,
};

void zmk_memory_lcd_hold_frame(const struct device *dev) {
    if (!device_is_ready(dev) || dev->api != &lpm009m360a_api) return;
    struct lpm009m360a_data *data = dev->data;
    const struct lpm009m360a_config *config = dev->config;
    atomic_set(&data->hold_frame, 1);
    /* The final write already sent its latch clocks and released SPI. Keep
     * active-high SCS low while the CPU transitions, without clearing pixels. */
    if (config->bus.config.cs.gpio.port) {
        int rc = gpio_pin_configure_dt(&config->bus.config.cs.gpio,
                                      GPIO_OUTPUT_INACTIVE | GPIO_PULL_DOWN);
        if (rc < 0) LOG_WRN("Could not idle memory LCD CS: %d", rc);
    }
}

#define LPM009M360A_INIT(inst)                                                                     \
                                                                                                   \
    BUILD_ASSERT((DT_INST_PROP(inst, rotation) == 0 && DT_INST_PROP(inst, width) == 72 &&          \
                  DT_INST_PROP(inst, height) == 144) ||                                            \
                     (DT_INST_PROP(inst, rotation) == 1 && DT_INST_PROP(inst, width) == 144 &&     \
                      DT_INST_PROP(inst, height) == 72),                                           \
                 "LPM009M360A is a 72x144 panel, width/height must match the rotation");           \
                                                                                                   \
    const static struct lpm009m360a_config lpm009m360a_config_##inst = {                           \
        .bus = SPI_DT_SPEC_INST_GET(                                                               \
            inst, SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_HOLD_ON_CS | SPI_LOCK_ON, 0),         \
        .width = DT_INST_PROP(inst, width),                                                        \
        .height = DT_INST_PROP(inst, height),                                                      \
        .color_mode = DT_INST_PROP(inst, color_mode),                                              \
        .rotation = DT_INST_PROP(inst, rotation),                                                  \
        .reverse = DT_INST_PROP(inst, reverse),                                                    \
    };                                                                                             \
    static struct lpm009m360a_data lpm009m360a_data_##inst = {0};                                  \
                                                                                                   \
    PM_DEVICE_DT_INST_DEFINE(inst, lpm009m360a_pm_action);                                         \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(inst, lpm009m360a_init, PM_DEVICE_DT_INST_GET(inst),                     \
                          &lpm009m360a_data_##inst, &lpm009m360a_config_##inst, POST_KERNEL,       \
                          CONFIG_DISPLAY_INIT_PRIORITY, &lpm009m360a_api);

DT_INST_FOREACH_STATUS_OKAY(LPM009M360A_INIT)
