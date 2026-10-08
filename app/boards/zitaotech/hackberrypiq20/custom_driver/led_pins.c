/*
 * LED pad configuration for the three PWM driven LEDs.
 *
 * The stock QMK firmware configures these pads itself:
 *
 *     palSetPadMode(..., PAL_MODE_ALTERNATE_PWM | PAL_RP_PAD_DRIVE12 |
 *                        PAL_RP_GPIO_OE);
 *
 * i.e. PWM function, 12 mA drive strength *and* the pad's output driver
 * explicitly on.  Zephyr's pinctrl only sets the function select (and leaves
 * the output-enable override at whatever the pinmux state implies), which left
 * GP11 and GP25 dark even though the PWM slice was configured: those two pins
 * only light up when driven as plain GPIOs.
 *
 * This runs after the PWM driver has applied its pinctrl (PWM init priority
 * 50) and before the LED / ZMK backlight drivers take over, and re-applies the
 * QMK pad setup so the PWM output actually reaches the pin.
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <hardware/gpio.h>

/* Layer indicator (GP0), trackpad LED (GP11), keyboard backlight (GP25). */
static const uint32_t led_pins[] = {0, 11, 25};

static int led_pins_init(void) {
    for (size_t i = 0; i < ARRAY_SIZE(led_pins); i++) {
        const uint32_t pin = led_pins[i];

        gpio_init(pin);
        gpio_set_function(pin, GPIO_FUNC_PWM);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_set_oeover(pin, GPIO_OVERRIDE_NORMAL);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_SLOW);
    }

    return 0;
}

/* POST_KERNEL, after PWM_INIT_PRIORITY (50) but before the LED drivers (90). */
SYS_INIT(led_pins_init, POST_KERNEL, 60);
