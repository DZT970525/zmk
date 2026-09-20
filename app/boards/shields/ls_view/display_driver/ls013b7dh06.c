/*
 * Copyright (c) 2026 ZitaoTech <zitao.tech@foxmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT sharp_ls013b7dh06

#include "ls013b7dh06.h"

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(display_ls013b7dh06, CONFIG_DISPLAY_LOG_LEVEL);

#define LS013B7DH06_WIDTH 128U
#define LS013B7DH06_HEIGHT 128U
#define LS013B7DH06_BYTES_PER_LINE 48U
#define LS013B7DH06_FRAMEBUFFER_SIZE (LS013B7DH06_BYTES_PER_LINE * LS013B7DH06_HEIGHT)

#define LS013B7DH06_RESET_TIME K_MSEC(1)
#define LS013B7DH06_EXIT_SLEEP_TIME K_MSEC(1)

/*
 * LS013B7DH06 serial protocol values when SPI is configured LSB-first.
 *
 * Reference refresh sequence:
 *
 *   WRITE_CMD
 *   LINE_ADDRESS
 *   48 bytes RGB111 data
 *   0x00 dummy
 *   LINE_ADDRESS
 *   48 bytes RGB111 data
 *   0x00 dummy
 *   ...
 *   final 0x00 dummy
 */
#define LS013B7DH06_WRITE_CMD 0x01U
#define LS013B7DH06_CLEAR_CMD 0x04U
#define LS013B7DH06_DUMMY 0x00U

struct ls013b7dh06_data {
    /*
     * The display itself is RGB111:
     *
     * 128 pixels × 3 bits = 384 bits = 48 bytes per line.
     *
     * Zephyr still provides MONO01/MONO10 input. The driver expands each
     * monochrome pixel to:
     *
     * black = RGB111 000
     * white = RGB111 111
     */
    uint8_t buf[LS013B7DH06_FRAMEBUFFER_SIZE];
};

struct ls013b7dh06_config {
    struct spi_dt_spec bus;
    uint16_t height;
    uint16_t width;
    int rotation;
    int reverse;

    /*
     * Kept only so the existing overlay remains compatible:
     *
     * color_mode = [02];
     *
     * LS013B7DH06 always receives RGB111 data, so this property is not used
     * to select a 16-byte monochrome transfer mode.
     */
    uint8_t color_mode[1];
};

/*
 * Send one or more buffers while keeping CS asserted and the SPI bus locked.
 *
 * SPI_HOLD_ON_CS and SPI_LOCK_ON are configured in SPI_DT_SPEC_INST_GET().
 */
static int ls013b7dh06_spi_write_hold(const struct device *dev, struct spi_buf *buffers,
                                      size_t count) {
    const struct ls013b7dh06_config *config = dev->config;
    const struct spi_buf_set set = {
        .buffers = buffers,
        .count = count,
    };

    return spi_write_dt(&config->bus, &set);
}

static void ls013b7dh06_spi_release(const struct device *dev) {
    const struct ls013b7dh06_config *config = dev->config;

    spi_release_dt(&config->bus);
}

/*
 * Clear command sequence from the reference implementation:
 *
 *   CS active
 *   CLEAR_CMD
 *   0x00
 *   CS inactive
 */
static int ls013b7dh06_clear_display(const struct device *dev) {
    uint8_t clear_cmd = LS013B7DH06_CLEAR_CMD;
    uint8_t dummy = LS013B7DH06_DUMMY;

    struct spi_buf buffers[] = {
        {
            .buf = &clear_cmd,
            .len = 1U,
        },
        {
            .buf = &dummy,
            .len = 1U,
        },
    };

    int ret = ls013b7dh06_spi_write_hold(dev, buffers, ARRAY_SIZE(buffers));

    ls013b7dh06_spi_release(dev);

    if (ret < 0) {
        LOG_ERR("Display clear failed: %d", ret);
        return ret;
    }

    k_sleep(LS013B7DH06_RESET_TIME);

    return 0;
}

/*
 * Set one physical RGB111 pixel in the 48-byte display line.
 *
 * This follows the packing used by the reference mlcd_DrawPixel():
 *
 * pixel 0: byte 0 bits 0..2
 * pixel 1: byte 0 bits 3..5
 * pixel 2: byte 0 bits 6..7 and byte 1 bit 0
 * ...
 *
 * SPI is LSB-first, exactly like:
 *
 *   hspi1.Init.FirstBit = SPI_FIRSTBIT_LSB;
 */
static void ls013b7dh06_set_rgb111_pixel(uint8_t *line, uint16_t x, bool white) {
    uint16_t first_bit = (uint16_t)(x * 3U);

    for (uint8_t component = 0U; component < 3U; component++) {
        uint16_t bit_index = (uint16_t)(first_bit + component);
        uint16_t byte_index = bit_index >> 3;
        uint8_t bit_mask = BIT(bit_index & 0x07U);

        if (white) {
            line[byte_index] |= bit_mask;
        } else {
            line[byte_index] &= (uint8_t)~bit_mask;
        }
    }
}

/*
 * Convert the Zephyr monochrome source bit to physical white/black.
 *
 * reverse = 1:
 *     PIXEL_FORMAT_MONO01
 *     source 0 = black
 *     source 1 = white
 *
 * reverse = 0:
 *     PIXEL_FORMAT_MONO10
 *     source 0 = white
 *     source 1 = black
 */
static bool ls013b7dh06_source_bit_is_white(const struct ls013b7dh06_config *config,
                                            bool source_bit) {
    return config->reverse ? source_bit : !source_bit;
}

/*
 * Start one update transaction.
 *
 * The write command must be sent only once. It must not be repeated for every
 * line.
 */
static int ls013b7dh06_begin_update(const struct device *dev) {
    uint8_t command = LS013B7DH06_WRITE_CMD;
    struct spi_buf buffer = {
        .buf = &command,
        .len = 1U,
    };

    return ls013b7dh06_spi_write_hold(dev, &buffer, 1U);
}

/*
 * Send one line inside the current transaction:
 *
 *   line address
 *   48 bytes RGB111
 *   0x00 dummy
 *
 * The line address is sent directly. Because the SPI controller is configured
 * with SPI_TRANSFER_LSB, no software bit reverse is needed.
 */
static int ls013b7dh06_send_line(const struct device *dev, uint8_t line_address,
                                 const uint8_t *line_data) {
    uint8_t address = line_address;
    uint8_t dummy = LS013B7DH06_DUMMY;

    struct spi_buf buffers[] = {
        {
            .buf = &address,
            .len = 1U,
        },
        {
            .buf = (void *)line_data,
            .len = LS013B7DH06_BYTES_PER_LINE,
        },
        {
            .buf = &dummy,
            .len = 1U,
        },
    };

    return ls013b7dh06_spi_write_hold(dev, buffers, ARRAY_SIZE(buffers));
}

/*
 * Finish one update transaction:
 *
 *   final 0x00 dummy
 *   CS inactive
 */
static int ls013b7dh06_end_update(const struct device *dev) {
    uint8_t dummy = LS013B7DH06_DUMMY;
    struct spi_buf buffer = {
        .buf = &dummy,
        .len = 1U,
    };

    int ret = ls013b7dh06_spi_write_hold(dev, &buffer, 1U);

    ls013b7dh06_spi_release(dev);

    return ret;
}

static int ls013b7dh06_abort_update(const struct device *dev, int error) {
    ls013b7dh06_spi_release(dev);
    return error;
}

static int ls013b7dh06_blanking_on(const struct device *dev) {
    ARG_UNUSED(dev);

    LOG_DBG("Blanking on is not implemented");

    return 0;
}

static int ls013b7dh06_blanking_off(const struct device *dev) {
    ARG_UNUSED(dev);

    LOG_DBG("Blanking off");

    k_sleep(LS013B7DH06_EXIT_SLEEP_TIME);

    return 0;
}

static int ls013b7dh06_read(const struct device *dev, const uint16_t x, const uint16_t y,
                            const struct display_buffer_descriptor *desc, void *buf) {
    ARG_UNUSED(dev);
    ARG_UNUSED(x);
    ARG_UNUSED(y);
    ARG_UNUSED(desc);
    ARG_UNUSED(buf);

    return -ENOTSUP;
}

static int ls013b7dh06_validate_write(const struct device *dev, uint16_t x, uint16_t y,
                                      const struct display_buffer_descriptor *desc,
                                      const void *buf) {
    const struct ls013b7dh06_config *config = dev->config;
    size_t required_size;

    if ((desc == NULL) || (buf == NULL)) {
        return -EINVAL;
    }

    if ((desc->width == 0U) || (desc->height == 0U)) {
        return -EINVAL;
    }

    if (((uint32_t)x + desc->width > config->width) ||
        ((uint32_t)y + desc->height > config->height)) {
        LOG_ERR("Write outside display: x=%u y=%u w=%u h=%u", x, y, desc->width, desc->height);
        return -EINVAL;
    }

    if (desc->pitch < desc->width) {
        LOG_ERR("Pitch %u is smaller than width %u", desc->pitch, desc->width);
        return -EINVAL;
    }

    if (config->rotation == 0) {
        /*
         * Horizontal monochrome buffer.
         */
        if (((x & 0x07U) != 0U) || ((desc->width & 0x07U) != 0U) || ((desc->pitch & 0x07U) != 0U)) {
            LOG_ERR("Horizontal writes must be aligned to 8 pixels");
            return -EINVAL;
        }

        required_size = ((size_t)desc->pitch / 8U) * (size_t)desc->height;
    } else if (config->rotation == 1) {
        /*
         * Zephyr SCREEN_INFO_MONO_VTILED:
         * one source byte represents eight vertical pixels.
         */
        if (((y & 0x07U) != 0U) || ((desc->height & 0x07U) != 0U)) {
            LOG_ERR("Vertical-tiled writes must be aligned to 8 pixels");
            return -EINVAL;
        }

        required_size = (size_t)desc->pitch * ((size_t)desc->height / 8U);
    } else {
        LOG_ERR("Unsupported rotation: %d", config->rotation);
        return -ENOTSUP;
    }

    if (desc->buf_size < required_size) {
        LOG_ERR("Input buffer too small: %u, required: %u", (unsigned int)desc->buf_size,
                (unsigned int)required_size);
        return -EINVAL;
    }

    return 0;
}

static int ls013b7dh06_write_rotation_0(const struct device *dev, uint16_t x, uint16_t y,
                                        const struct display_buffer_descriptor *desc,
                                        const uint8_t *source) {
    const struct ls013b7dh06_config *config = dev->config;
    struct ls013b7dh06_data *data = dev->data;
    size_t source_stride = (size_t)desc->pitch / 8U;
    int ret;

    /*
     * Convert the requested area from Zephyr 1-bit horizontal layout to the
     * display's packed RGB111 framebuffer.
     */
    for (uint16_t row = 0U; row < desc->height; row++) {
        uint16_t physical_y = (uint16_t)(y + row);
        uint8_t *display_line = &data->buf[(size_t)physical_y * LS013B7DH06_BYTES_PER_LINE];

        for (uint16_t column = 0U; column < desc->width; column++) {
            size_t source_offset = (size_t)row * source_stride + (column >> 3);

            uint8_t source_byte = source[source_offset];
            bool source_bit = (source_byte & BIT(7U - (column & 0x07U))) != 0U;

            bool white = ls013b7dh06_source_bit_is_white(config, source_bit);

            ls013b7dh06_set_rgb111_pixel(display_line, (uint16_t)(x + column), white);
        }
    }

    ret = ls013b7dh06_begin_update(dev);
    if (ret < 0) {
        return ls013b7dh06_abort_update(dev, ret);
    }

    /*
     * Send only the modified physical lines. The command is already active
     * and is not repeated here.
     */
    for (uint16_t row = 0U; row < desc->height; row++) {
        uint16_t line_index = (uint16_t)(y + row);
        uint8_t line_address = (uint8_t)(line_index + 1U);

        ret = ls013b7dh06_send_line(dev, line_address,
                                    &data->buf[(size_t)line_index * LS013B7DH06_BYTES_PER_LINE]);

        if (ret < 0) {
            return ls013b7dh06_abort_update(dev, ret);
        }
    }

    return ls013b7dh06_end_update(dev);
}

static int ls013b7dh06_write_rotation_1(const struct device *dev, uint16_t x, uint16_t y,
                                        const struct display_buffer_descriptor *desc,
                                        const uint8_t *source) {
    const struct ls013b7dh06_config *config = dev->config;
    struct ls013b7dh06_data *data = dev->data;
    int ret;

    /*
     * Preserve the rotation behavior of the original LPM009M360A-derived
     * driver:
     *
     * source columns become physical display lines in reverse X order.
     */
    for (uint16_t tile_row = 0U; tile_row < (desc->height / 8U); tile_row++) {

        for (uint16_t column = 0U; column < desc->width; column++) {

            size_t source_offset = (size_t)tile_row * desc->pitch + column;

            uint8_t source_byte = source[source_offset];

            uint16_t physical_line = (uint16_t)(config->width - 1U - x - column);

            uint8_t *display_line = &data->buf[(size_t)physical_line * LS013B7DH06_BYTES_PER_LINE];

            /*
             * One source byte contains eight vertical pixels.
             */
            for (uint8_t bit = 0U; bit < 8U; bit++) {
                bool source_bit = (source_byte & BIT(7U - bit)) != 0U;

                bool white = ls013b7dh06_source_bit_is_white(config, source_bit);

                uint16_t physical_x = (uint16_t)(y + tile_row * 8U + bit);

                ls013b7dh06_set_rgb111_pixel(display_line, physical_x, white);
            }
        }
    }

    ret = ls013b7dh06_begin_update(dev);
    if (ret < 0) {
        return ls013b7dh06_abort_update(dev, ret);
    }

    /*
     * Send each modified physical line, matching the reference transaction:
     *
     * address + 48 bytes + dummy
     */
    for (uint16_t column = 0U; column < desc->width; column++) {

        uint16_t line_index = (uint16_t)(config->width - 1U - x - column);

        uint8_t line_address = (uint8_t)(line_index + 1U);

        ret = ls013b7dh06_send_line(dev, line_address,
                                    &data->buf[(size_t)line_index * LS013B7DH06_BYTES_PER_LINE]);

        if (ret < 0) {
            return ls013b7dh06_abort_update(dev, ret);
        }
    }

    return ls013b7dh06_end_update(dev);
}

static int ls013b7dh06_write(const struct device *dev, const uint16_t x, const uint16_t y,
                             const struct display_buffer_descriptor *desc, const void *buf) {
    int ret = ls013b7dh06_validate_write(dev, x, y, desc, buf);

    if (ret < 0) {
        return ret;
    }

    if (((const struct ls013b7dh06_config *)dev->config)->rotation == 0) {
        return ls013b7dh06_write_rotation_0(dev, x, y, desc, (const uint8_t *)buf);
    }

    return ls013b7dh06_write_rotation_1(dev, x, y, desc, (const uint8_t *)buf);
}

static void *ls013b7dh06_get_framebuffer(const struct device *dev) {
    ARG_UNUSED(dev);

    return NULL;
}

static int ls013b7dh06_set_brightness(const struct device *dev, const uint8_t brightness) {
    ARG_UNUSED(dev);
    ARG_UNUSED(brightness);

    return -ENOTSUP;
}

static int ls013b7dh06_set_contrast(const struct device *dev, const uint8_t contrast) {
    ARG_UNUSED(dev);
    ARG_UNUSED(contrast);

    return -ENOTSUP;
}

static void ls013b7dh06_get_capabilities(const struct device *dev,
                                         struct display_capabilities *capabilities) {
    const struct ls013b7dh06_config *config = dev->config;

    memset(capabilities, 0, sizeof(*capabilities));

    capabilities->x_resolution = config->width;
    capabilities->y_resolution = config->height;

    /*
     * Applications still supply a monochrome image. Conversion to RGB111 is
     * performed internally.
     */
    capabilities->supported_pixel_formats = PIXEL_FORMAT_MONO01 | PIXEL_FORMAT_MONO10;

    capabilities->current_pixel_format =
        config->reverse ? PIXEL_FORMAT_MONO01 : PIXEL_FORMAT_MONO10;

    if (config->rotation == 0) {
        capabilities->screen_info = SCREEN_INFO_X_ALIGNMENT_WIDTH | SCREEN_INFO_MONO_MSB_FIRST;
    } else {
        capabilities->screen_info = SCREEN_INFO_MONO_VTILED | SCREEN_INFO_MONO_MSB_FIRST;
    }

    capabilities->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int ls013b7dh06_set_pixel_format(const struct device *dev,
                                        const enum display_pixel_format pixel_format) {
    const struct ls013b7dh06_config *config = dev->config;

    enum display_pixel_format configured_format =
        config->reverse ? PIXEL_FORMAT_MONO01 : PIXEL_FORMAT_MONO10;

    if (pixel_format == configured_format) {
        return 0;
    }

    LOG_ERR("Pixel format is fixed by the reverse property");

    return -ENOTSUP;
}

static int ls013b7dh06_set_orientation(const struct device *dev,
                                       const enum display_orientation orientation) {
    ARG_UNUSED(dev);

    if (orientation == DISPLAY_ORIENTATION_NORMAL) {
        return 0;
    }

    return -ENOTSUP;
}

static int ls013b7dh06_init(const struct device *dev) {
    const struct ls013b7dh06_config *config = dev->config;
    struct ls013b7dh06_data *data = dev->data;
    int ret;

    LOG_INF("Initializing LS013B7DH06");

    if (!spi_is_ready_dt(&config->bus)) {
        LOG_ERR("SPI bus is not ready");
        return -ENODEV;
    }

    if ((config->width != LS013B7DH06_WIDTH) || (config->height != LS013B7DH06_HEIGHT)) {
        LOG_ERR("LS013B7DH06 requires 128x128 resolution");
        return -EINVAL;
    }

    if ((config->rotation != 0) && (config->rotation != 1)) {
        LOG_ERR("Unsupported rotation: %d", config->rotation);
        return -EINVAL;
    }

    /*
     * Reference code initializes the RGB111 framebuffer to 0xFF, which is
     * white for all pixels.
     */
    memset(data->buf, 0xFF, sizeof(data->buf));

    ret = ls013b7dh06_clear_display(dev);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("LS013B7DH06 initialized");

    return 0;
}

#ifdef CONFIG_PM_DEVICE
static int ls013b7dh06_pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_RESUME:
        return ls013b7dh06_blanking_off(dev);

    case PM_DEVICE_ACTION_SUSPEND:
        return ls013b7dh06_blanking_on(dev);

    case PM_DEVICE_ACTION_TURN_ON:
        return ls013b7dh06_init(dev);

    case PM_DEVICE_ACTION_TURN_OFF:
        return 0;

    default:
        return -ENOTSUP;
    }
}
#endif

static const struct display_driver_api ls013b7dh06_api = {
    .blanking_on = ls013b7dh06_blanking_on,
    .blanking_off = ls013b7dh06_blanking_off,
    .write = ls013b7dh06_write,
    .read = ls013b7dh06_read,
    .get_framebuffer = ls013b7dh06_get_framebuffer,
    .set_brightness = ls013b7dh06_set_brightness,
    .set_contrast = ls013b7dh06_set_contrast,
    .get_capabilities = ls013b7dh06_get_capabilities,
    .set_pixel_format = ls013b7dh06_set_pixel_format,
    .set_orientation = ls013b7dh06_set_orientation,
};

#define LS013B7DH06_INIT(inst)                                                                     \
    static const struct ls013b7dh06_config ls013b7dh06_config_##inst = {                           \
        .bus = SPI_DT_SPEC_INST_GET(inst,                                                          \
                                    SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_LSB |      \
                                        SPI_HOLD_ON_CS | SPI_LOCK_ON,                              \
                                    0),                                                            \
        .width = DT_INST_PROP(inst, width),                                                        \
        .height = DT_INST_PROP(inst, height),                                                      \
        .color_mode = DT_INST_PROP(inst, color_mode),                                              \
        .rotation = DT_INST_PROP(inst, rotation),                                                  \
        .reverse = DT_INST_PROP(inst, reverse),                                                    \
    };                                                                                             \
                                                                                                   \
    static struct ls013b7dh06_data ls013b7dh06_data_##inst;                                        \
                                                                                                   \
    PM_DEVICE_DT_INST_DEFINE(inst, ls013b7dh06_pm_action);                                         \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(inst, ls013b7dh06_init, PM_DEVICE_DT_INST_GET(inst),                     \
                          &ls013b7dh06_data_##inst, &ls013b7dh06_config_##inst, POST_KERNEL,       \
                          CONFIG_DISPLAY_INIT_PRIORITY, &ls013b7dh06_api);

DT_INST_FOREACH_STATUS_OKAY(LS013B7DH06_INIT)
