# Q20 with Q10 feature parity

Target: `zitaotech_q20/nrf52840/zmk`. USB/Bluetooth/Studio name:
`ZitaoTech_q20`.

The factory keymap contains the same five named layers and four spare Studio
slots as Q10. Each populated layer is exactly Q10 positions 2–41, retaining all
40 remaining bindings. Bootmagic moves from position 4 to 2; assignable hold
positions move from 27/37/38 to 25/35/36. The original Q20 7x10 matrix,
40-position transform, I2C SDA P0.23 / SCL P0.25 and
active-low P0.02 MOTION are retained.

Physical position 2 is a centered 1U touchpad at x=4.5U, matching Q10/BBP9981
after removing their two shoulder positions. The other Q20 key geometry remains.

Pointer/scroll/direction processing, automatic mouse layer, curve, fast-scroll,
USB LED behavior, backlight effects/timeouts, per-host settings, Bluetooth host
tuning, runtime macros/combos and Studio controls match the current Q10 source.
Factory DPI is 30; curve enabled, 6 / 1.48 / 0.66 / 1.35. Smooth scrolling and
non-base automatic mouse overlay default OFF. Automatic mouse layer 4 defaults
ON/800ms. Four Bluetooth hosts; backlight 80, idle-off 30 seconds, configurable
1–6000 seconds or always on; sleep defaults Never.

Q20's address probing is preserved. Per the user's orientation correction,
the original X/Y outputs are swapped and the resulting X axis is inverted:
0x3B reads register 0x82 and maps X=byte2/Y=byte1; 0x37 reads register 0x0A
and maps X=byte1/Y=-byte3.
Physical orientation still needs hardware confirmation.
The GPIO LED definition sharing P0.10 with PWM was removed so the touchpad
PWM owns that pin. Q20's layer-indicator node and two keyboard-backlight
channels remain on their original pins. The existing Vial matrix interface
remains available alongside Studio matrix testing.

Old saved keymaps/settings can override defaults. The previous Q20 firmware
used a different DPI grid and DPI behavior command enum. To evaluate these new
factory settings on an already configured keyboard, restore the keymap and
touchpad defaults in Studio; do not assume old overrides match the new defaults.
No global settings or Bluetooth bonds are automatically erased by this build.

## Recovery image

`build-reset.py` builds a separate `Q20-V1.0-RESET` image with startup settings
erasure and bootmagic key -1. Successful erasure at APPLICATION priority 96 is
followed by a checked bootloader request and cold reboot at priority 99.
Erase/boot-mode failure stops normal application startup. It clears saved
settings and Bluetooth bonds; after returning to the UF2 drive, flash the
normal Q20 firmware. `package-reset.py` validates and exports the recovery UF2.

Build with `python3 build-local.py` in the configured WSL toolchain. Tests in
this folder exercise packet axes, I2C, exact layer remapping, pointer curve,
scrolling, LED startup, profiles, pairing/link and timeout persistence.
Physical key scanning, sensor orientation and host behavior still need testing.
