# BBQ20 — ZMK 0.4 / Zephyr 4.1

Target: `bbq20/nrf52840/zmk`. Original source: `C:/Users/Dai/zmk/app/boards/arm/bbq20`.

## Hardware

Preserves the original 8x7 matrix, 42 physical keys,
I2C SDA P0.22 / SCL P1.00 and flash partitions (application 0x1000,
settings 0xD4000, bootloader 0xF4000). The reserve and mouse layers are transparent.
V0.3: touchpad PWM is P0.10 (Sofle activity/DPI/direction feedback).
Keyboard PWM is P0.09, driven by the Q20 layer-effect implementation: layer 0
follows brightness, 1 blinks, 2 breathes, 3 fast blinks; Studio can override
each layer effect. PWM0/P0.07 stores ZMK backlight state; the effect driver
mirrors that brightness/on/off/idle state onto the real keyboard LED.
The 0x3B packet X and Y signs are both reversed from V0.2.
The old GPIO LED on P0.10 was removed because PWM owns that pin.

V0.2: Matrix columns use active-high open-source output (nRF D0S1):
the selected column sources high, inactive columns disconnect. Rows retain
active-high inputs with pull-down. This avoids output contention when two
switches on the same electrical row connect different columns. Scan settling
is 5 us before input reads and 20 us between columns. Matrix pins, transform,
key bindings and debounce remain unchanged. Hardware rollover still needs
validation; diode-less matrices can ghost with multi-row rectangle chords.

BBQ20 has only the 0x3B controller (register 0x82, three-byte packets).
There is no IRQ pin, address probe, or 0x37 fallback. Sensor data is polled
every 8 ms on a private queue, at most eight packets per invocation.
Only nonzero movement generates input; an empty read produces no mouse report.
The pointer, scroll and direction processing reuses the Sofle implementation.
Polling needs hardware validation for battery consumption and feel.

## Studio

Uses the existing `zitaotech__trackpad` subsystem. The existing Studio
selects its Touchpad page by subsystem capability; no frontend patch is
needed. Driver label: `Touchpad`, without Left_hand/Right_hand prefixes.
No split-module discovery subsystem is registered.

Runtime controls: pointer speed, fast/slow multipliers,
scroll speed/inversion/burst/smooth scrolling, Caps Lock scrolling,
direction mode and swipe threshold, touch time, assignable mode keys,
automatic mouse layer, and LED settings. Settings persist through the
existing trackpad module. Automatic mouse layer defaults OFF; enabling
it selects transparent layer 4, which can be edited in Studio.
Default physical positions (zero based): scroll 2, direction 27,
fast 37, slow 38. All can be reassigned in Studio.

Also enables keymap editing, 32 runtime macros, runtime combos/tap dance,
BLE management, OS detection, default layer, matrix tester, bootmagic,
device information, keyboard backlight and layer LED effects.
Bootmagic defaults to position 2. Keyboard lighting uses ZMK backlight
directly so Studio brightness/on/off/idle settings affect the real PWM.

## Build

In a configured ZMK/Zephyr environment:
`west build -p always -s app -d build-bbq20 -b bbq20/nrf52840/zmk`.
This workspace additionally supplies `build-local.py` for its WSL SDK
and cached dependency modules. Run it with `python3` under Ubuntu WSL.

## Validation boundary

Compilation and generated configuration can be checked without hardware.
USB/BLE connection, all 42 keys, the physical 0x3B touchpad, direction,
scrolling, LEDs and persistence still require a physical BBQ20 test.
No firmware is automatically flashed by the build script.


V0.4: Default layers are BASE(0), LOWER(1), RAISE(2), RESERVE(3), MOUSE(4).
Layer 2 physical X (position 29) is `&TP 0` / TP MODE SWITCH. Enable the
mode-switch key in Studio's Touchpad page for that behavior to toggle modes.
Four reserved keymap slots permit Studio's Add Layer operation (up to nine
total layer slots). Automatic mouse layer is now 4, default OFF.
Previously saved Studio bindings, layer names/order and mouse-layer settings
override firmware defaults: update the individual binding/names/mouse-layer
settings in Studio, or reset keymap defaults only if that is intended.
Flashing this firmware does not clear saved settings.

LOWER/RAISE thumb keys on layers 0..2 use old bbp9981's `sl_mo` behavior:
hold-preferred, 450 ms tapping term, hold=&mo, tap=&sl. Sticky release timeout
is 3000 ms. The first normal key consumes the sticky layer; a held layer
returns on release. Studio-saved keys override these firmware defaults.

LOWER/RAISE thumb keys on layers 0..2 use old bbp9981's `sl_mo` behavior:
hold-preferred, 450 ms tapping term, hold=&mo, tap=&sl. Sticky release timeout
is 3000 ms. The first normal key consumes the sticky layer; a held layer
returns on release. Studio-saved keys override these firmware defaults.
