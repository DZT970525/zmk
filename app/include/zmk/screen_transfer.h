#pragma once
#include <stdbool.h>
#include <zephyr/sys/util.h>
#include <stddef.h>
#include <stdint.h>
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_SHIELD_LPM_VIEW)
bool zitaotech_screen_transfer_active(void);
int zitaotech_image_receive(const void *data, size_t len);
void zitaotech_image_ack_receive(const void *data, size_t len);
int zitaotech_image_link_send(const void *data, size_t len);
int zitaotech_image_link_mtu(void);
int zitaotech_image_link_ack(const void *data, size_t len);
#else
static inline bool zitaotech_screen_transfer_active(void) { return false; }
#endif

