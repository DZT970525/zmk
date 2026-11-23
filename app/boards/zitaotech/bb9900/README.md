# BB9900 / ZMK 0.4

Target: `bb9900/nrf52840/zmk`.
Ported from the old BB9900 hardware definition, with BBQ20 V0.4 features.
Original 42-key transform, 7 row / 6 column pins, I2C and flash partitions retained.
Column outputs use active-high open-source (inactive high impedance), with
5 us input settling / 20 us between outputs, as in BBQ20 V0.4.
Touchpad: fixed 0x3B, bounded polling every 8 ms, BBQ20 V0.4 axis convention.
No motion IRQ is assumed. Verify axis orientation on the physical BB9900.

PWM pins retain BB9900 wiring: generic backlight P0.11, touchpad LED P0.10,
layer indicator P0.03, keyboard backlight P0.09. Separate keyboard fade driver
is retained; Studio layer effects control P0.03. Duplicate P0.10 GPIO LED removed.

Five default layers: BASE / LOWER / RAISE / RESERVE / MOUSE, plus four
reserved Studio layer slots. LOWER/RAISE tap sticky (3000 ms), hold momentary
(450 ms). RAISE X uses TP MODE SWITCH; enable the mode-switch key in Studio.
Automatic mouse layer 4 defaults off. Includes Studio touchpad/backlight,
runtime macros/combos/tap dance, matrix tester, BLE management, default layer,
OS detection and settings RPC from the existing ZMK 0.4 modules.
BBQ20 and BB9900 share the same physical key order and default key bindings.
Saved Studio settings override firmware defaults; flashing does not erase them.

Build and host checks do not establish hardware compatibility. Verify matrix
chords, USB/BLE, touchpad axes, three lighting channels and saved settings.
