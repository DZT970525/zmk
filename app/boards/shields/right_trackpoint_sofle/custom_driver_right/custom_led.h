#pragma once

#include <stdint.h>

/**
 * @brief Get the last non-zero TrackPoint DPI indicator brightness
 *
 * This value is NOT cleared when LED fades to 0.
 */
uint8_t custom_led_get_last_valid_brightness(void);
