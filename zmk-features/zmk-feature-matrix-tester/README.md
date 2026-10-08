# zmk-feature-matrix-tester

Reusable read-only Studio matrix tester for any ZMK keyboard with physical layouts.
Add this directory to ZMK_EXTRA_MODULES, or include it as a west project containing
zephyr/module.yml. Enable CONFIG_ZMK_MATRIX_TESTER=y with CONFIG_ZMK_STUDIO=y
on a standalone board or split central. Peripheral firmware does not need it:
normal split key events reach the central. Maximum 256 key positions.

Studio subsystem: zmk__matrix_tester. Request command 1 reads/renews a session,
2 begins, 3 clears tested history (held keys remain tested), 4 stops. Response
contains key count and LSB-first pressed/tested bitmaps indexed by physical-layout
position. Get geometry via standard Studio getPhysicalLayouts. No keyboard-specific
JSON, hardware row numbers or HID/Vial interface is required.

History and state live in RAM only. A 5-second heartbeat lease prevents background
sessions collecting indefinite history. Browser polls serially every 100ms while
this page is active; fast taps stay recorded even between polls. The firmware
continues normal typing/pointing; Studio's focused test area prevents browser key
shortcuts where possible. No Flash writes or periodic firmware worker.

GNU ld --wrap observes event_manager_raise before combo/hold-tap capture and calls
the original dispatcher unchanged. This module works without modifying ZMK core.
The build must use GNU-compatible linker wrapping and normal position events.
Sensor rotation and pointer movement are not matrix keys. A disconnected split
peripheral can retain a held position until ZMK emits its release/reset events.

This module uses the custom Studio RPC extension (`zmk/studio/custom.h`) already
used by the other zmk-feature modules in this tree; stock firmware without that
extension needs the same extension before this module can be enabled.

Example local setup: ZMK_EXTRA_MODULES=/absolute/path/zmk-feature-matrix-tester
plus CONFIG_ZMK_STUDIO=y and CONFIG_ZMK_MATRIX_TESTER=y in the keyboard config.
In this workspace app/CMakeLists.txt registers the vendored module automatically.
For host logic tests run python3 tests/test_matrix_tester.py on a machine with gcc.
The tests compile the actual observer/session handler with ASan/UBSan. A real
Sofle left build verifies generated nanopb integration, the registered subsystem,
and linker wrapping; runtime input/browser verification still requires flashing.

Encoder testing: reports up to 16 configured sensor indices. ROTATION-channel
nonzero events record separate CW/CCW tested bits and LE16 event counters. Both
positive/negative degree values and legacy val2-only ticks are supported. Studio
shows a 300ms directional activity flash and persistent tested colour, including
rotation events between polls. Clear resets direction history without resetting
counters; Begin resets both. Encoders with no geometry metadata have separate
numbered direction cards; encoder push switches remain normal matrix positions.
