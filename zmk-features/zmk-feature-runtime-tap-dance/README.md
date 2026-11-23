# Runtime Tap Dance

This ZMK module provides 16 runtime-configurable slots by default. `&rtd 0`
selects slot 0. Enable `CONFIG_ZMK_RUNTIME_TAP_DANCE=y` and
`CONFIG_ZMK_RUNTIME_TAP_DANCE_STUDIO_RPC=y`, include
`<behaviors/runtime_tap_dance.dtsi>`, and edit slots in ZitaoTech Studio.

The V1 editor exposes On tap, On hold, On double tap and On tap + hold.
Tapping term ranges from 50 to 2000 milliseconds (default 200). Releasing the
first tap starts the inter-tap window. The second press starts its hold window.
The final tap release resolves immediately; a single tap waits until the
deadline measured from the last press, without restarting the term on release.
When both double actions are unassigned, a single tap resolves on release.
Unassigned holds use the matching tap action;
unassigned double taps do nothing. Slots start disabled and empty.

Preferences apply while a press is undecided:

- `tap-preferred` (default): other keys do not decide hold early.
- `hold-preferred`: another key press immediately selects hold.
- `balanced`: a complete other-key press/release selects hold.
- In all preferences, reaching the tapping term while pressed selects hold.

Other position/keycode events are buffered in order until the decision, as with
ZMK hold-tap. The bounded queue resolves instead of dropping input on overflow.
Active dances snapshot their configuration, so editing/deleting a slot cannot
change an already-held action's release. Nested runtime Tap Dance actions are
rejected. Up to eight decided dances can remain held concurrently.

## Storage and Studio

Custom subsystem `zitaotech_runtime_tap_dance` is secured. Request V1 uses
`[1, operation, slot]` with operations 0 read, 2 delete and 4 commit.
Save starts with `[1,1,slot,crc32:u32LE]`, followed by ordered chunks
`[1,3,slot,offset:u8,data]` of at most 26 data bytes, then commit. Requests fit
the native 30-byte custom RPC limit. CRC32 uses IEEE; incomplete, expired,
out-of-order or corrupt uploads cannot publish changes. Uploads expire after
30 seconds without a chunk. The complete configuration contains
110 bytes: enabled, tap-count, preference (0 hold/1 balanced/2 tap), reserved 0,
term u16 LE, NUL-terminated UTF-8 name[24], eight alternating tap/hold bindings
(u16 behavior local ID, u32 param1, u32 param2, all LE). `0xffff` is unassigned.
Response is `[1, result:i32LE, capacity:u8, config:110]`.

Each explicit save writes a versioned 111-byte record at `runtime_td/<slot>`.
RAM is published only after successful durable save. Delete saves the disabled
default configuration, preserving the slot ID. Settings reset clears the slots.
Configuration and storage stay on LEFT in KEYPOINT_M receiver mode; neither
RIGHT nor the Macintosh receiver needs the module.

Run `python3 tests/host_test.py` to exercise the production state machine with
deterministic time and event mocks. Real Bluetooth/USB hardware still needs
verification after flashing.
