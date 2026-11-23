# SOFLE V1.08 unified 66-key firmware

Canonical source has been merged into this ZMK checkout. The folder retains its test suffix to preserve the validated build paths; this is the source used for the V1.08 release.

- Keyboard board IDs: sofle_dongle_left/nrf52840/zmk and sofle_dongle_right/nrf52840/zmk.
- Shields: lpm_view + left_bbtrackpad / right_trackpoint.
- EXTRA_CONF_FILE: configs/left.conf + configs/unified-test.conf + configs/unified-left.conf (left), configs/right.conf + configs/unified-test.conf (right).
- Left extra overlay: sofle-dongle-left.overlay. Keymap: sofle_dongle.keymap.
- Macintosch source: ../Macintosch. Its pairing protocol remains compatible.
- Layout:66 positions; no receiver placeholder at0. Space59/62; default Bootmagic60/61. Layer1 B position45 toggles local/receiver output only when permission is enabled. Both press/release are consumed, including saved Studio keymaps.
- Includes paired TrackPoint fractional transport, four BLE pointing profiles (USB shares current slot),10 left /20 right custom images, USB Studio reconnect and Memory LCD bootloader frame retention.
- No reset is needed when upgrading V1.07. Existing customized bindings and bonds remain.

The previous 67-key sofle_dongle board manifest is archived as board.yml.pre-unified to avoid duplicate board IDs. Historical sources are retained separately; do not use their legacy build.sh for this66-key release.
