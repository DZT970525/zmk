#pragma once
#include <zephyr/sys/reboot.h>
#if IS_ENABLED(CONFIG_ZMK_DISPLAY)
void zmk_bootloader_feedback_reboot(int type);
#else
static inline void zmk_bootloader_feedback_reboot(int type) { sys_reboot(type); }
#endif
