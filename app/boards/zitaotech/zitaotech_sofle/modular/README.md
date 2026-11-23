# SOFLE modular pointing test

Build in the existing WSL toolchain: `python3 app/boards/zitaotech/zitaotech_sofle/build-modular.py left` (or `right`).
Flash both halves. Receiver firmware is unchanged. This is opt-in; build-release.py remains the stable build.

At boot each half probes its existing I2C slot: touchpad 0x37/0x3b, TrackPoint 0x15. Only the present sensor driver installs its interrupt handler or starts its work queue. Module replacement requires rebooting. The slot retains its original SDA/SCL, motion IRQ and PWM LED pin.

Four split input IDs: right TrackPoint 0, left touchpad 1, left TrackPoint 2, right touchpad 3. The left has four independent processing pipelines, including fractional state. Generated additional per-hand settings/RPC implementations use the same maintained module sources but separate symbols, relay event IDs, profile locks and flash keys; protobuf layouts are unchanged. Right-side module status is relayed every two seconds. Disconnection clears the cached right type.

Defaults: left and right scroll position 61, regardless of module type. Existing flash assignments remain; reset module settings to adopt changed defaults. Touchpad modules use trackpad_led with arrow blank feedback relayed back to the right slot when needed. Left touchpad LED uses PWM1 channel 0/P0.07 and left TrackPoint uses channel 1/P0.16. On the right connector, touchpad LED uses PWM1 channel 1/P0.16; TrackPoint LED retains PWM1 channel 0/P0.04. Local touchpad DPI events also trigger a visible brightness preview. The other LED worker remains inactive. DPI behavior bindings select physical left/right slots in this test build.

Studio topology subsystem: sofle__pointing_modules, request byte 1, response [version=1,leftKind,rightKind]. Kind 0 unavailable, 1 touchpad, 2 TrackPoint. Additional RPCs: sofle__right_touchpad and sofle__left_trackpoint. Original RPCs control left touchpad and right TrackPoint.

Hardware tests required: all four combinations, boot probe reliability, I2C IRQ wiring, scrolling positions, both LED types, arrow blank feedback, wireless reconnect, independent settings after reboot and host-profile changes. Compiling and host tests do not verify these physical behaviors.



2026-10-19 fast-scroll parity: V1.11-FASTSCROLL-TEST uses shared mode index4 for an optional, initially unassigned fast-scroll key. It adds a held x2 factor to wheel output (both axes and precision modes) without activating scroll by itself or changing pointer speed. Existing mode0-3 numbers, settings and per-hand module detection remain unchanged; Studio's shared per-hand pages expose assignment/clear with the capability flag. SOFLE retains66 keys, position61 scroll defaults, its original I2C/MOTION/LED pins, image caches and2048-byte sensor stacks. KEYPOINT-only alternate SDA probing is not included in SOFLE. Test both halves together before publishing.
