/* SPDX-License-Identifier: MIT */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>
extern int zmk_settings_reset_result(void);
static int reset_to_bootloader(void) {
    int result = zmk_settings_reset_result();
    /* Do not start normal application tasks or write settings on erase failure. */
    if (result < 0) { for (;;) k_sleep(K_FOREVER); }
    result = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    if (result < 0) { for (;;) k_sleep(K_FOREVER); }
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}
/* Reset settings runs at APPLICATION/96 with bootmagic enabled. */
SYS_INIT(reset_to_bootloader, APPLICATION, 99);
