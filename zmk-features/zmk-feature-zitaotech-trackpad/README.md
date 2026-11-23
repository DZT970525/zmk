# zmk-feature-zitaotech-trackpad

Runtime-adjustable settings for ZitaoTech trackpads, exposed to ZitaoTech
Studio's **Trackpad** tab through a custom Studio RPC subsystem
(`zitaotech__trackpad`).

What the web UI can change:

| Setting | Range | Notes |
| --- | --- | --- |
| Pointer speed (DPI) | 10 steps, 10..100 (`CONFIG_ZITAOTECH_TRACKPAD_DPI_*`) | same curve the `&DPI TP_INC/TP_DEC` keymap bindings drive |
| Scroll speed | 25..200 % | multiplier applied on top of the driver's scroll scale |
| Scroll direction | invert X and/or Y | applied on top of the board's compile-time signs |

Everything is persisted with ZMK's `settings` subsystem (`ztp/dpi`,
`ztp/speed`, `ztp/invert`), so it survives reboots.

The board driver is the only consumer: on Q10, `a320.c` reads the values
through `<zitaotech/zmk/trackpad.h>` while building pointer/scroll reports.
Enable it per board:

```conf
CONFIG_ZITAOTECH_TRACKPAD_CONTROL=y
CONFIG_ZITAOTECH_TRACKPAD_CONTROL_STUDIO_RPC=y
```
# Optional pointer acceleration curve

`CONFIG_ZITAOTECH_TRACKPAD_POINTER_CURVE` enables the paired-sample curve API
for a driver that calls `zitaotech_trackpad_curve_get()` (currently BBP9981).
The `zitaotech__trackpad` State reports `pointer_curve_supported` and the full
curve; `set_pointer_curve` validates the complete value before changing RAM.
Gains, exponent and full-gain speed use hundredths. Time fields are milliseconds;
distance is raw sensor counts; velocity response is percent. Defaults are
enabled, 56/170 gain, 127 exponent, 450 velocity, 419 ms ramp, 853 counts,
60 ms idle and 50% response. Disabled means fixed gain 1, retaining all values.
Updates are saved to `zitaotech_padcurve/config` after the configured settings
save debounce. Global touchpad reset restores curve defaults. Old firmware and
drivers without the capability hide the Studio curve controls.
