# SOFLE_M / SOFLE_DONGLE

V1.04 parity update (2026-10-16): paired right/left TrackPoint transport uses
1/16 counts until final DPI/burst scaling. The left owns four BLE-slot profiles
for both pointer DPIs, beast flags/multipliers and the TrackPoint curve. USB
and Macintosh relay output share the currently selected BLE slot. Profile
switches reset fractional/speed history; the receiver forwards final HID
reports without scaling again. Profiles initialize from existing global
settings, persist after a one-second debounce, and survive ordinary updates.
The local profile manager is compiled for left/right only, not the receiver
or settings-reset images. Memory-LCD final-frame hold and USB RPC reconnect
fixes use the shared display/Studio core. Build all three normal targets with
`build.sh left`, `build.sh right`, `build.sh receiver`; its version overrides
are stored in this board folder. Update both halves together; no reset needed
when updating an existing 67-key SOFLE_M. The receiver build retains the prior
splash/fade, telemetry, title and battery display behavior.

SOFLE with Macintosh receiver. All three endpoints use a 67-key map.
Key 0 is the receiver button; existing SOFLE key positions shift by one.
Encoder switches: 7 and 8. Layer 1 X: 43, C: 44, B: 46.
Right mode thumb: 62. Escape: 1.
SOFLE row/column GPIOs are preserved; column 0 belongs only to the receiver.

Left shield: left_bbtrackpad_sofle_dongle.
Right shield: right_trackpoint_sofle_dongle.
Receiver shield: st7789_display.

Run build.sh left, right, or receiver under the existing WSL toolchain.
The shared ZMK_KEYPOINT_* protocol options are intentionally retained.
V1.00-test is a validation build, not a published release.

Board-specific sensor and pointing sources are local in custom_driver_left,
custom_driver_right and custom_driver. Shared ZMK core changes are included as
shared-core-sofle-support.patch for review/reproduction, and are already applied
to this workspace. The two shield folders live in app/boards/shields as required
by Zephyr shield discovery.

When changing from standalone SOFLE (66 keys), back up Studio configuration first.
Saved key bindings from the 66-key variant retain their original numeric indices;
if old custom bindings remain, restore the default keymap in Studio before
verifying the new 67-key map. This normal firmware does not erase settings.

Board-specific sensor and pointing sources are local in custom_driver_left,
custom_driver_right and custom_driver. Shared ZMK core changes are included as
shared-core-sofle-support.patch for review/reproduction, and are already applied
to this workspace. The two shield folders live in app/boards/shields as required
by Zephyr shield discovery.

When changing from standalone SOFLE (66 keys), back up Studio configuration first.
Saved key bindings from the 66-key variant retain their original numeric indices;
if old custom bindings remain, restore the default keymap in Studio before
verifying the new 67-key map. This normal firmware does not erase settings.

Reset builds: run build-reset.sh left/right/receiver. They erase the full 128KB settings partition and disable BLE, split, sleep and Studio. Wait for the completion message, then install the matching normal firmware. Left/right feedback is cyan/black; receiver feedback is black/white with its physical bootloader button enabled.

SOFLE_M left V1.01 (2026-10-14): ten69x53 custom area pictures, per-slot enable/order/readback. Existing two images retained;128KB settings backend. Only left needs update; existing right and Macintosh receiver remain compatible.

V0.07 reconnect comparison (2026-10-23): the receiver uses a continuous passive scan (96/96 units, 60 ms) while disconnected, instead of 100 ms per 250 ms. Scanning stops before connection creation and stays off while connected. Existing bonds, approval and encryption remain unchanged. Hardware reconnect timing is not yet verified.
Pair prompts now follow the keyboard approval response instead of a pre-encryption address lookup. A genuinely unapproved keyboard still returns pending approval and requires Space.
Disconnected scanning is revalidated every two seconds (EALREADY retains the running scan); failures retry after one second. No polling or scan restart occurs during connection establishment or while connected. Screensaver-related loss of scanning remains a hardware reproduction hypothesis.

V0.09 (2026-10-23): remove receiver idle image mode. Keep the status frame and radio/USB running, fade backlight to the saved sleep brightness (fresh default 10%), and fade back to active brightness on input. Existing timeout/brightness config persists. Only boot image slot 0 is loaded and writable; old slot 1 data is left untouched. Inactivity and wake do not tear down/recreate widgets. Firmware behavior and host logic tests pass; real hardware verification is pending.

2026-10-23 release-guard test (not published): paired left firmware sends serialized keyboard/consumer/mouse-button state snapshots every status tick; no relative deltas are replayed and snapshot reception does not wake display sleep. Receiver enables a two-second relay lease only after seeing the new keyboard snapshot type, batches receive processing, and requests all-release/disconnect on lease expiration. Legacy peers do not activate the lease. Manual screen RPC op11 exposes approximate queue counters/max residence, connection parameters and config-save duration/count; queue residence starts at receiver arrival, not the TrackPoint sensor. Hardware stuck-key/lag reproduction remains unverified.

2026-10-23 reconnect phase test: reset report/control handles, nonce, pending product and handshake/read flags on each connection result and disconnect. Security-triggered approval polling requires the new offer write ACK; READY/subscribe/hello callbacks additionally require the active handshake phase. Actual-handler host regression simulates 100 connection cycles and confirms prior code fails. This targets keyboard-only power cycling with receiver left on; physical reconnect acceptance is pending.
