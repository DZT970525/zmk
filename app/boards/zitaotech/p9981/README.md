# P9981 (nRF52840)

Target: `p9981/nrf52840/zmk`. Firmware version: `V1.01`.

This port uses the 35-key, four-layer layout and hardware wiring from
`C:/Users/Dai/zmk/app/boards/arm/p9981`. The device name remains `p9981`.

## Hardware

- 4 × 10, column-to-row matrix; the last row uses columns 0, 2, 4, 6 and 8.
- Rows: P1.10, P0.06, P0.07, P1.13. Columns: P0.08, P0.13, P1.09,
  P0.11, P1.11, P0.29, P0.31, P0.30, P0.28, P0.03.
- Internal low-frequency RC oscillator with 500 ppm clock accuracy and
  Nordic RC calibration; no external 32.768 kHz crystal is required.
- PWM1/P0.09 drives the keyboard backlight and selected layer effects.
  PWM0/P0.10 retains the old board's backlight control output. Both NFC pins
  are configured as GPIO; the old duplicate GPIO LED on P0.10 is removed.
- Battery ADC channel 2, divider 2 MΩ / 2.82 MΩ.
- Existing flash layout is preserved: application at `0x1000`, settings at
  `0xD4000`, UF2 bootloader at `0xF4000`.
- No touchpad, I²C pointing sensor, automatic mouse layer or external-power
  GPIO. The old UPPER-layer external-power toggle had no device to control;
  that one binding is now `none`. All other stock bindings are preserved.

## Connection and Studio

The board selects the same host connection sources and effective Bluetooth
settings as Q20 V1.06: shared Q20 `ble_reconnect.c`, pinned upstream HOG and
compatibility code, 1M PHY, advertising retry, +8 dBm and four host profiles.
The same USB CDC DTR/session handling is used for Studio reconnects.

Studio supports keymap/layer editing, macros (32 slots), combos, runtime
Tap Dance, default layer, BLE management, USB OS detection, device/version
information, matrix testing, Bootmagic, sleep and backlight/layer effects.
The shared small-keyboard behaviors include Layer Tap, LM, Key Toggle,
Caps Word, Key Repeat and mouse-key actions. There is no touchpad subsystem
or Vial-specific second HID interface.

Stock brightness is 100%, idle backlight timeout 30 seconds, deep sleep off
until enabled in Studio. Existing saved brightness/on-off settings take
precedence over this factory default; upgrades do not overwrite them.
Bootmagic defaults to key position 32 (the bottom-row 3U Space key,
matrix row 3 / column 4, P1.13 / P1.11): hold Space while powering on to
enter the existing UF2 bootloader. A Bootmagic key previously saved in
Studio takes precedence; upgrades do not replace that saved choice.

## Reset firmware

The separate `reset.conf` build (`V1.01-reset`) erases the complete NVS
settings partition at `0xD4000` (size `0x20000`), including pairing,
keymaps and saved Studio settings. After successful erasure it immediately
returns to the existing UF2 bootloader, where normal P9981 V1.01 firmware
must be copied back. The bootloader partition is not erased. If erasure
or setting the bootloader mode fails, it stops without starting the normal
application. Its Bootmagic key is disabled so holding Space cannot skip
the erase. The normal build remains V1.01 and never erases settings.

## Build and validation

Use the repository's configured Zephyr 4.1/ZMK module environment:

```sh
west build -b p9981/nrf52840/zmk app -- -DEXTRA_CONF_FILE=boards/zitaotech/p9981/build-version.conf
```

The local controlled build uses `tmp/p9981-build/run.ps1` (2 GB/2 CPU WSL,
at most two compile jobs, WSL shut down afterward). It creates a fresh
directory or explicitly resumes a verified P9981 cache, checks the final
configuration, memory, UF2 family and payload,
and leaves normal settings/bootloader partitions outside the UF2 payload.
`tmp/p9981-build/audit_board.py` compares the source and generated hardware
definition with the old P9981. Hardware operation still requires testing
on a physical P9981; compilation is not a physical-device test.
