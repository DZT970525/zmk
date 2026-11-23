# KEYPOINT standalone firmware

KEYPOINT keeps the KEYPOINT_M screen, pointing, keymap and mouse-layer features, but has no Macintosh/Dongle route behavior, relay GATT service, topology RPC or peer-selection workflow.

Build left with board zitaotech_keypoint_left/nrf52840/zmk, shields lpm_view;left_bbtrackpad_keypoint, EXTRA_CONF_FILE=keypoint-left.conf and EXTRA_DTC_OVERLAY_FILE=keypoint-left.overlay from this directory.
Build right with board zitaotech_keypoint_right/nrf52840/zmk, shields lpm_view;right_trackpoint_keypoint and EXTRA_CONF_FILE=keypoint-right.conf.

The left screen uses the independent ZITAOTECH_KEYPOINT_SCREEN feature. Shared rendering and transport improvements remain shared with KEYPOINT_M. USB/BLE host telemetry is direct to KEYPOINT. The Windows sender recognizes paired KEYPOINT devices.

Custom board processors are grouped in custom_driver. The Caps Lock scroll decision continues to use the host HID indicator; no local double-tap detection is added.

Default mouse-button layer extension is disabled, 200ms, configurable 50–2000ms. Layer 1 C uses SCREEN MODE SWITCH. The former connection-mode position is transparent.

Independent KEYPOINT has 56 physical keys and transform positions, unlike KEYPOINT_M's 57-key layout. All five factory layers must have exactly 56 bindings, starting with Esc/Tab on QWERTY, without a leading Bootloader placeholder. Pointer-mode/scroll position defaults are 49 for the right thumb action and 0 for Esc/Tab. Use left_bbtrackpad_keypoint for this standalone build. Do not copy KEYPOINT_M binding rows without removing its extra physical position.

V1.05 (2026-10-14) parity audit: shared right-picture storage supports20 custom Flash images with one reusable left cache; right uses32 NVS sectors in its existing128KB partition. Manual Studio synchronization and readback input/display lock apply to standalone split as well. Mouse-layer extension is the same source as KEYPOINT_M, retaining off/200ms defaults. The left arrow event handler compiles via ZITAOTECH_KEYPOINT_SCREEN. Left runtime macro count is32; right input and BLE priorities match the current M firmware. Mac receiver boot splash/telemetry forwarding are receiver-only and are omitted. Version stamped into left device info: V1.05. Actual build validation must check56 bindings on every layer and no relay/peer-selection features.
