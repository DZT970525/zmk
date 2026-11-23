# KEYPOINT V1.08 pointing parity

Retains the 56-key board map, original wiring and matching shields. The
KEYPOINT_M receiver button remains position 0; ordinary KEYPOINT keeps its
first Escape/Tab hold-tap key.

The paired right/left pointer pipeline carries 1/16 counts until final left
DPI/burst scaling, with 64-bit intermediate multiplication. Each of four BLE
slots owns touchpad and TrackPoint DPI, their pointer beast flags/multipliers,
and the complete TrackPoint velocity curve. USB and receiver output share the
selected BLE slot. `keypoint_pointing/0` through `/3` store validated records;
missing records inherit the legacy settings after startup settings load.
Writes are debounced by one second; profile changes reset speed/fractional
history. Receiver builds have no profile manager and forward final HID only.

Shared display core keeps the final Memory LCD bootloader frame; shared Studio
RPC preserves the USB reconnect fix. Existing image slots and telemetry remain
unchanged. Both halves must be updated together without resetting settings.
Host tests cover actual C arithmetic and profile persistence/selection; real
USB/BLE/receiver operation remains to be verified on this keyboard family.
