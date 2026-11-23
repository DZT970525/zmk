/* Single-piece BBP9981 always enables its host link. No connection policy here. */
#include <stdbool.h>
#include <errno.h>
#include <zmk/ble.h>
bool zmk_ble_host_is_enabled(void) { return true; }
int zmk_ble_host_set_enabled(bool enabled) { (void)enabled; return -ENOTSUP; }
