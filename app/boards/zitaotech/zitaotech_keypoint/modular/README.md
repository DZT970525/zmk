# KEYPOINT interchangeable pointing modules

Build both halves with `python3 app/boards/zitaotech/zitaotech_keypoint/build-modular.py left` (or `right`) in the existing WSL toolchain.

This opt-in configuration retains KEYPOINT identity, the original 56-position transform, Bootmagic, host profiles and receiver telemetry. It reuses the maintained SOFLE modular implementation, supplying KEYPOINT module-specific bus and IRQ wiring (see R2 below). No SOFLE layout or extra receiver key is introduced.

LEDs: left touchpad PWM1/P1.09; left TrackPoint PWM2/P0.07; right touchpad PWM1 channel 1/P0.16; right TrackPoint PWM1 channel 0/P0.06. The old right generic GPIO LED is disabled because it also claimed P0.16. Only the detected module's LED worker starts. DPI edits update brightness and touchpad arrows retain blank feedback.

KEYPOINT module contacts are on scan row 5, columns 6/7; swapped modules normalize encoder/pointer push positions without changing the two small buttons or the logical layout. Scroll defaults to position 49 on either module; direction/pointer mode key defaults to unassigned. Existing saved assignments remain. Module swapping requires power-off and restart.

Studio uses the same capability-based module metadata RPC as SOFLE, so left/right pages and two identical module types are supported without name-based UI selection. Internal RPC identifiers retain the shared implementation names.

The shared image implementation retains ten Flash slots with one RAM cache, independent upload staging, integrity checks and newest-upload preview. Right-side twenty-slot image support is retained. No pointing-driver thread stacks are reduced or shared.

Both halves must be flashed together. The receiver is unchanged. Compilation and host tests do not establish physical wiring, radio behavior, LED or image playback correctness; test these on hardware before publishing.

R2: KEYPOINT automatically selects alternate wiring before TWIM initialization (priority45, GPIO40/TWIM50). Left TP SDA0.08/SCL0.04/MOTION0.26; right pad SDA0.07/SCL1.08/MOTION0.14. Original left pad SDA0.26/MOTION0.08 and right TP SDA0.14/MOTION0.07 remain supported. Clock is unchanged. GPIO probe uses open-drain with input enabled, bounded clock stretching and rejects a permanently-low SDA. Dynamic pinctrl retains selected default and sleep states. Change modules with power off.
