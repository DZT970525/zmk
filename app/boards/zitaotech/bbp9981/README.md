# BBP9981 on ZMK 0.4

Curve factory enablement follows A320 detection:0x3B off, other addresses on.
Stored/user curve settings override detection; disabled UI folds all parameters.


V1.10 default update: automatic touchpad mouse layer enabled, layer4,800ms.
Saved user choices remain. Mouse-button timer extension still defaults off.


V1.10: versioned release of the latest V1.09 behavior, with the unchanged
V1.09 recovery image. Normal upgrade preserves settings and needs no reset.


V1.09 default update: scroll-mode hold key is unassigned (-1). Stored user
assignments remain and can be cleared using Studio Cancel assignment.

V1.09 startup fix: P0.09 NFC pad is explicitly GPIO, both keyboard LED PWM
outputs retry after errors without changing saved on/off or brightness.

V1.09 settings update: factory backlight80, scroll hold-key unassigned,
runtime sleep enabled with factory Never, touch-start gap200ms (two drops).

V1.09: official release of the accepted V0.9 behavior. build-local.py builds the
normal image; build-reset.py builds the recovery image in a separate directory.
Recovery erases settings at APPLICATION/96 and returns to UF2 at /99 before
main() can load or write settings. A normal upgrade does not need recovery.


V0.9: RAISE/X (position29) now binds TP MODE SWITCH, command0. Enable the
mode-switch shortcut in Studio to use it. Other key bindings are preserved.
DPI behavior exposes Increase/Decrease trackpad DPI metadata. Studio and key
DPI changes light the pad even while idle, at the current DPI brightness for
the configured LED hold time (Caps Lock breathing still has priority).
BLE slots1..4 independently save pad DPI and pointer burst on/multiplier;
USB edits the selected BLE slot, like Sofle. Missing slots inherit legacy
settings. Flash writes debounce1s; curve/scroll settings remain shared.
Mouse-button automatic-layer extension uses the Sofle subsystem (off,200ms
by default,50..2000ms). Only actual presses extend an active automatic layer.
Bootmagic factory position is4, the central pad button. Saved settings and
keymaps override defaults; assign/reset the individual key instead of clearing
all settings. Hardware LED/Bootmagic/power-cycle testing remains pending.

V0.8: default layer names are BASE / LOWER / RAISE / RESERVE / MOUSE (0..4).
Four `status = "reserved"` keymap slots allow Studio Add Layer (+), for nine
total slots, matching Sofle's spare-slot scheme. All existing bindings remain.
Saved layer names/order may override defaults; use Studio's rename control
if needed. Adding layer slots does not clear saved keys or curve settings.

V0.7: Studio now shows the same four curve controls as Sofle TrackPoint, plus
the enable switch. Each control explains its effect. Defaults follow the
user's screenshot: min 0.56, max 1.70, exponent 1.27, vmax 4.50 counts/ms.
Internal defaults: ramp 419 ms, distance 853 counts, idle 60 ms, response 50%.
Existing saved settings are retained; Reset curve applies these new defaults.
The touch-start gate stays at 100 ms and discards two successful packets.

V0.6: new-touch sample gap is 100 ms. The first two successful packets are
still discarded. The Touchpad page in the updated local Studio exposes the
curve switch and all eight numeric curve parameters, with live updates,
validation, default reset and debounced device persistence. Curve-off gives
fixed gain 1 and retains the saved parameters. The feature is capability-gated
by CONFIG_ZITAOTECH_TRACKPAD_POINTER_CURVE; other boards default unsupported.
The curve reset pause (default 60 ms) is independent of the 100 ms touch gate.
Build version remains in build-version.conf, separate from defconfig.

V0.5: discard the first two successful raw packets at startup and each new
touch, before summing motion or applying pointer/scroll/direction processing.
Zero-motion packets count; I2C failures do not. A 0x3B batch drops only its
first two packets, not two batches. The gate resets gesture state and residuals.
New touch detection uses a 100 ms successful-sample gap
heuristic: MOTION is a packet interrupt, not a reliable contact/lift signal.
Touches separated by less than 100 ms of sample silence may be one gesture.

V0.4: paired X/Y pointer acceleration follows Sofle's velocity-curve shape:
gain 0.62..1.70, exponent 0.90, speed ceiling 4.5 raw counts/ms, speed EMA 0.35.
An additional same-direction stroke ramp reaches the ceiling after at least
350 ms and 120 raw counts. A pause over 60 ms or reversal/right-angle turn
restarts at low gain. Small alternating adjustments stay slow. Scroll and
direction modes bypass and reset the curve. Studio DPI/multiplier and speed
keys still multiply the result; fractional motion is retained.
These trial parameters are board-local in custom_driver/pointer_accel.h;
there is no new Studio acceleration slider in this version.
The version stamp lives in build-version.conf, passed by build-local.py as
EXTRA_CONF_FILE. The hardware defconfig contains no version override.

V0.3 A/B test: the 0x57 register reader performs two standalone address writes
before the burst read, matching Sofle's 0x37 transaction sequence. The 0x3B
sequence, HID format, polling intervals, keymap and pointer scale stay at V0.2.
This is a protocol experiment; report rate improvement has not been measured.

V0.2: MOTION IRQ still starts reads immediately. While MOTION remains active,
continue on the private queue after 1 ms (both variants), with 2 ms error retry.
No periodic reads while MOTION is inactive. The pointer path no longer uses a
three-sample integer average. Floating-point scale and fractional remainders
preserve small movements, including the slow key. USB HID interval is 1 ms;
sensor/I2C and BLE connection timing still bound actual host report rates.

Target: `bbp9981/nrf52840/zmk`. The factory keymap is byte-identical to Q10.
Five named layers and four reserved Studio layer slots are available.
Original 6x10 matrix pins, 42-position transform, I2C pins, flash partitions
and four PWM pins are preserved. Duplicate P0.10 GPIO LED removed (PWM owns it).

Touchpad uses the current Sofle input driver and indicator implementation,
adapted to BBP9981: default 0x57 protocol (7-byte packet, X=byte3, Y=byte1),
0x3B boot detection, active-low P0.02 MOTION, native pointer axes and scaling.
Private work queue, bounded packet drain and retry while MOTION is active.
Studio controls pointer speed/multiplier, scrolling and inversion, smooth
scrolling, Caps Lock scroll, direction mode/threshold, speed/mode key positions,
touch LED timeout, and automatic mouse layer (4, defaults on, 800 ms).
Original &DPI bindings still adjust the same setting as Studio.

Keyboard backlight uses the original BBP9981 two-channel driver, with Studio
brightness/on-off and per-layer effects. P0.09 and P1.04 are driven together;
P0.10 is the touchpad indicator. P0.15 holds the generic backlight state.
Saved Studio settings take priority over compiled defaults.
Shared features depend on the current local app and zmk-features tree.

## V1.11: Q10 V1.01 feature parity

- Default DPI 30 and enabled pointer curve: full-gain speed 6 counts/ms,
  exponent 1.48, minimum 0.66, maximum 1.35, for both sensor variants.
- Assignable fast-scroll hold key doubles both wheel axes and safely handles
  release or reassignment while held.
- Automatic mouse overlay over non-base layers is supported, factory OFF.
  Enabling the persistent Studio switch temporarily prioritizes MOUSE;
  timeout removes only the automatic overlay and preserves the prior layers.
- Backlight defaults to 30 seconds idle; Studio accepts whole seconds 1–6000
  or always on. The existing two-channel backlight and per-layer effects remain.
- Startup DPI restoration no longer cancels the USB touchpad LED pulse.
  Actual touch still overrides the pulse.
- Q10 host pairing/link tuning, USB line control, input/BLE scheduling and
  NVS lookup improvements apply. Exactly four Bluetooth host slots remain.
- Lowercase `bbp9981` USB/Bluetooth/Studio name, `bbp_pointing` settings namespace, packet formats/axes,
  P0.02 MOTION, matrix scan/wiring, I2C and LED pins are preserved.
  Q10 diode-less matrix timings are not copied to BBP hardware.

Normal upgrade keeps saved settings, so new defaults do not overwrite saved
DPI/curve/keymap choices. Reset firmware remains separate and returns to the
bootloader after a successful reset. Existing recovery images remain usable.

Validation: native packet/I2C, keymap/hardware parity, curve, fast-scroll,
USB LED, pairing/link, profile persistence, backlight, sleep and reset tests.
The shared mouse-overlay tests cover both layer 3 and BBP/Q10 layer 4.
Physical hardware validation is still required.

Hardware tests still required: all keys and chords, both sensor variants,
axes, scroll/direction mode, DPI keys, LEDs, USB/BLE and settings persistence.




## Low-frequency clock hardware

BBP9981 uses the external 32.768 kHz crystal (XTAL, 50 ppm), matching the legacy board. Do not copy Q20 internal-RC settings to this board.
