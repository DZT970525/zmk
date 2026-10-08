# DESIGN: zmk-feature-os-detection

Detect the host OS (Windows / macOS / Linux / iOS / Android / unknown) over
USB and BLE, raise a ZMK event, and expose state + per-BLE-profile manual
override through a Custom Studio RPC subsystem + Web UI. MIT licensed. Must
never reference QMK (GPL-2.0) source.

It can also optionally auto-switch a keymap layer per detected OS
(`CONFIG_ZMK_OS_DETECTION_LAYER_AUTO_SWITCH`, default `n`), but that overlaps
with [zmk-feature-default-layer](https://github.com/cormoran/zmk-feature-default-layer)
and consumers should be pointed there instead - see "Layer auto-switch"
below. The Kconfig defaults to off and the listener code is compiled out
entirely unless a consumer explicitly opts in.

`ZMK_OS_IOS` was split out from `ZMK_OS_MACOS` as its own value on
2026-07-05 after a real iPhone USB capture showed a genuine, reproducible
difference from real macOS (`SET_FEATURE(DEVICE_REMOTE_WAKEUP)` after
enumeration) despite an otherwise identical descriptor-read pattern - see
`docs/fingerprints.md`. Before that capture this module (like the original
task brief) treated "macOS/iOS" as one bucket because USB alone generally
can't tell Apple's desktop and mobile USB stacks apart; that turned out not
to be true for this specific signal. Note BLE still can't distinguish iOS
from macOS - the split only applies to USB.

`ZMK_OS_ANDROID` was added the same day after a real Android BLE capture
showed its GATT read pattern differs from desktop Linux's (no GAP
Appearance read, unlike Windows/Linux) - see `docs/fingerprints.md`. This
is BLE-only: over USB, Android's kernel-level enumeration is identical to
desktop Linux's and is deliberately still reported as `ZMK_OS_LINUX` there
(no separate value needed for that transport).

Built from `cormoran/zmk-module-template-with-custom-studio-rpc`
(`main+custom-studio-protocol`). Depends on `cormoran/zmk`
(`main+custom-studio-protocol`, pinned at `618f0832` as of writing — same
Zephyr `v4.1.0+zmk-fixes` pin as upstream ZMK, legacy `CONFIG_USB_DEVICE_STACK`
confirmed in `app/src/usb.c`) and `cormoran/zmk-feature-custom-settings`.

## Naming

- Author namespace: `cormoran`. Module/feature name: `os_detection` /
  `os-detection`.
- Custom Studio subsystem identifier: `cormoran__os_detection`.
- Proto package: `cormoran.os_detection`. Proto path:
  `proto/cormoran/os-detection/os_detection.proto`.
- Top Kconfig: `CONFIG_ZMK_OS_DETECTION` (gates everything below).

## Public C API (`include/cormoran/os-detection/os_detection.h`)

```c
enum zmk_os { ZMK_OS_UNKNOWN = 0, ZMK_OS_WINDOWS, ZMK_OS_MACOS, ZMK_OS_LINUX, ZMK_OS_IOS, ZMK_OS_ANDROID };

enum zmk_os zmk_os_detection_current(void); // effective OS for active endpoint

struct zmk_os_changed { enum zmk_os os; };
ZMK_EVENT_DECLARE(zmk_os_changed);
```

`ZMK_OS_UNKNOWN == 0` matters: it doubles as "no override" sentinel for
settings and as the nanopb zero-value default.

## Kconfig tree

```
config ZMK_OS_DETECTION
    bool "Enable OS detection"

if ZMK_OS_DETECTION

config ZMK_OS_DETECTION_STUDIO_RPC
    bool "Enable OS detection custom Studio RPC"
    depends on ZMK_STUDIO

config ZMK_OS_DETECTION_USB
    bool "Detect host OS via USB enumeration fingerprint"
    depends on ZMK_USB
    select USB_DEVICE_BOS

config ZMK_OS_DETECTION_USB_SETTLE_MS
    int "Debounce (ms) after last SETUP before finalizing the USB guess"
    depends on ZMK_OS_DETECTION_USB
    default 200

config ZMK_OS_DETECTION_BLE
    bool "Detect host OS via BLE GATT access fingerprint"
    depends on ZMK_BLE
    select BT_GATT_AUTHORIZATION_CUSTOM

config ZMK_OS_DETECTION_BLE_GATT_CLIENT_PROBE
    bool "Opt-in: discover ANCS/AMS/Device Name as a GATT client post-pairing"
    depends on ZMK_OS_DETECTION_BLE
    select BT_GATT_CLIENT
    default n

config ZMK_OS_DETECTION_LAYER_AUTO_SWITCH
    bool "Auto-activate a keymap layer based on the detected OS"
    default n
    help
      Overlaps with zmk-feature-default-layer, which is recommended
      instead. Off by default; see "Layer auto-switch" below.

if ZMK_OS_DETECTION_LAYER_AUTO_SWITCH

config ZMK_OS_DETECTION_LAYER_WINDOWS
    int "Layer to auto-activate for Windows (-1 disables)"
    default -1

config ZMK_OS_DETECTION_LAYER_MACOS
    int "Layer to auto-activate for macOS (-1 disables)"
    default -1

config ZMK_OS_DETECTION_LAYER_LINUX
    int "Layer to auto-activate for Linux (-1 disables)"
    default -1

config ZMK_OS_DETECTION_LAYER_IOS
    int "Layer to auto-activate for iOS (-1 disables)"
    default -1

config ZMK_OS_DETECTION_LAYER_ANDROID
    int "Layer to auto-activate for Android over BLE (-1 disables)"
    default -1

config ZMK_OS_DETECTION_LAYER_UNKNOWN
    int "Layer to auto-activate when OS is unknown (-1 disables)"
    default -1

endif # ZMK_OS_DETECTION_LAYER_AUTO_SWITCH

config ZMK_OS_DETECTION_TEST_INJECT
    bool "Test-only: inject recorded fingerprints instead of real hardware"
    default n
    help
      Hidden option enabled only by tests/test/*.conf. Feeds recorded SETUP
      sequences / BLE stat snapshots into the classifiers at boot so the
      pure classifier logic can be unit tested on native_sim, which has no
      USB device controller and no BLE.

endif
```

Nothing here forces `default y` for USB/BLE — every sub-feature is opt-in via
the consuming keyboard's `.conf`, matching the template's convention (see
`ZMK_TEMPLATE_FEATURE` having no forced default).

`CONFIG_ZMK_OS_DETECTION_BLE` must compile out completely when
`CONFIG_ZMK_BLE=n` (native_sim's `tests/studio/native_sim.conf` sets
`CONFIG_ZMK_BLE=n`) — enforced by `depends on ZMK_BLE`.

## File layout

```
include/cormoran/os-detection/os_detection.h   # public enum + event + getter
src/os_detection_core.c        # state machine, classifiers, layer listener
src/os_detection_usb.c         # linker-wrap hook, SETUP aggregation (USB only)
src/os_detection_ble.c         # GATT authorization/MTU/conn hooks (BLE only)
src/os_detection_settings.c    # zmk-feature-custom-settings registration (BLE only, needs ZMK_CUSTOM_SETTINGS)
src/studio/os_detection_handler.c  # Custom Studio RPC handler
proto/cormoran/os-detection/os_detection.proto
proto/cormoran/os-detection/os_detection.options
docs/fingerprints.md            # captured USB/BLE fingerprint data + rules
web/src/App.tsx                 # state table + BLE override UI
tests/test/...                  # native_sim unit tests for classifiers (TEST_INJECT)
tests/studio/...                # native_sim RPC registration + handler tests
tests/zmk-config/...             # real xiao_ble build-test artifacts
```

## Core state machine (`os_detection_core.c`)

Two transports (USB, BLE), each with two sources of truth:

- `detected`: last auto-classification (or `ZMK_OS_UNKNOWN` before settling).
- `override`: BLE only, per-profile, from custom settings
  (`ZMK_OS_UNKNOWN` == "no override" == AUTO).

Effective value per transport: `override != ZMK_OS_UNKNOWN ? override :
detected` (BLE); USB has no override, effective == detected.

`zmk_os_detection_current()` picks the transport matching
`zmk_usb_is_hid_ready()` / active BLE connection (USB wins if both are somehow
active, since ZMK itself prioritizes whichever endpoint HID reports go to —
mirror `zmk_endpoints` selection logic rather than re-deriving it). Whenever
the *effective* value for the *currently active* endpoint changes, raise
`zmk_os_changed` exactly once (dedupe on unchanged value to avoid event
spam from repeated classifier ticks).

## Layer auto-switch (opt-in, not recommended)

Gated entirely behind `CONFIG_ZMK_OS_DETECTION_LAYER_AUTO_SWITCH`
(`#if IS_ENABLED(...)` around the whole block in `os_detection_core.c`, and
the per-OS `ZMK_OS_DETECTION_LAYER_*` int configs live inside
`if ZMK_OS_DETECTION_LAYER_AUTO_SWITCH` in Kconfig) - default `n`, so the
listener doesn't exist in the build unless a consumer explicitly opts in.

This exists because the original task brief asked for it, but it overlaps
with [zmk-feature-default-layer](https://github.com/cormoran/zmk-feature-default-layer),
a dedicated module for per-host default layer selection with more criteria
than just OS. Point consumers there first; keep this only for the narrow
case of wanting layer switching driven solely by this module's OS
detection, without an extra module dependency.

When enabled: a `ZMK_LISTENER` on `zmk_os_changed` that deactivates the
previously-selected `ZMK_OS_DETECTION_LAYER_*` layer (if not -1) and
activates the new one, using `zmk_keymap_layer_activate` /
`_deactivate`. Guard every layer number against `-1` and against
`>= ZMK_KEYMAP_LAYERS_LEN`.

Pure classifiers (hardware-independent, unit-testable):

```c
enum zmk_os zmk_os_classify_usb(const struct usb_fp_stats *stats);
enum zmk_os zmk_os_classify_ble(const struct ble_fp_stats *stats);
```

`usb_fp_stats` / `ble_fp_stats` are plain structs of counters (see below);
classifiers have zero Zephyr/driver dependencies so they link and run
unmodified under `ZMK_OS_DETECTION_TEST_INJECT` on native_sim.

## USB detection (`os_detection_usb.c`)

- `CMakeLists.txt`, guarded by `if(CONFIG_ZMK_OS_DETECTION_USB)`:
  - `if(NOT CONFIG_USB_DEVICE_STACK)` → `message(FATAL_ERROR ...)` explaining
    this hook only works with the legacy USB device stack (risk called out
    in README — breaks if ZMK ever migrates to `device_next`).
  - `zephyr_ld_options(-Wl,--wrap=usb_handle_bos)`.
- **BOS capability registration** (`os_detection_usb_bos_init()`,
  `SYS_INIT(..., POST_KERNEL, 0)`): `CONFIG_ZMK_OS_DETECTION_USB` selecting
  `USB_DEVICE_BOS` bumps `bcdUSB` to 2.01, which is what makes a host
  request `GET_DESCRIPTOR(BOS)` at all — but Zephyr's legacy stack leaves
  the BOS header's `wTotalLength=0` until something calls
  `usb_bos_register_cap()`. Nothing else in ZMK does, so this module
  registers a minimal, always-truthful USB 2.0 Extension capability itself
  (fixed 2026-07-05 — see `docs/windows-usb-enumeration-issue.md`; a
  spec-invalid empty BOS made real Windows abort enumeration entirely).
  Must run before ZMK's own `usb_enable()`
  (`APPLICATION`/`CONFIG_ZMK_USB_INIT_PRIORITY`) — `POST_KERNEL` always
  precedes `APPLICATION` regardless of priority number, so ordering doesn't
  depend on a priority-number race.
- Wrap function observes every SETUP packet by calling
  `zmk_os_detection_observe_setup(setup)` before delegating to
  `__real_usb_handle_bos`, fully transparent (return value unchanged).
- Aggregated per-enumeration-cycle stats (`struct usb_fp_stats`): count of
  `GET_DESCRIPTOR(String)` requests bucketed by `wLength`, whether
  `GET_DESCRIPTOR(BOS)` was seen (+ its wLength), and request order index.
  `SET_ADDRESS` observation resets the aggregation (new enumeration cycle).
  Subscribe to `zmk_usb_conn_state_changed` and reset on disconnect too.
- 200ms (`CONFIG_ZMK_OS_DETECTION_USB_SETTLE_MS`) debounce timer from the
  last SETUP packet before calling the classifier and publishing a result —
  avoids classifying mid-enumeration.
- `docs/fingerprints.md` records real capture data. Only a Linux host
  fingerprint can be captured directly in this environment (this sandbox's
  physical rig is itself a Linux USB host attached to the target board via
  the existing J-Link/pyusb setup — see `skills/develop-zmk-module` in the
  workspace). Windows and macOS fingerprints require physically plugging the
  board into those OSes with usbmon/USBPcap running, which is outside what
  this environment can do; the general tendencies from the task brief are
  recorded as **unverified placeholders** pending real capture, clearly
  labeled, and the classifier is written so new thresholds can be dropped in
  by editing one table.
- `ZMK_OS_DETECTION_TEST_INJECT`: a test-only init hook that feeds one of
  several recorded `struct usb_setup_packet` sequences into
  `zmk_os_detection_observe_setup()` at boot, so `tests/test/` can assert the
  classifier's output via log snapshot without a real USB DC.

## BLE detection (`os_detection_ble.c`)

- Signals (weighted-evidence scoring into one `enum zmk_os` guess):
  - A: `bt_gatt_authorization_cb_register()` — which characteristics
    (HIDS Report Map/Info, Report Reference, DIS PnP ID, GAP Appearance) are
    read, and in what order. Callback always authorizes; observation only.
  - B: `bt_gatt_cb_register()` → `att_mtu_updated`.
  - C: `bt_conn_cb_register()` → initial connection interval + PHY/DataLen
    update transitions.
  - D (opt-in, `ZMK_OS_DETECTION_BLE_GATT_CLIENT_PROBE`, default off):
    post-`security_changed`, discover ANCS/AMS (exposed by iPhone/iPad
    pairing with accessories, not by macOS - maps to `ZMK_OS_IOS`, not
    `ZMK_OS_MACOS`) and GAP Device Name (0x2A00) prefix as a GATT client.
  - Like USB, exact thresholds need real-device logs (task calls this out
    explicitly: "まず実機ログで呼び出され方を確認してから指紋テーブルを作る").
    This environment has no way to pair the board with real Windows/macOS/
    Linux BLE hosts either (no Bluetooth radio exposed to the sandbox) — same
    placeholder-table approach as USB, documented in `docs/fingerprints.md`.
- Persistence via `zmk-feature-custom-settings`, two `INT32` array settings
  registered once each with `ZMK_CUSTOM_SETTING_ARRAY_DEFINE` (one element per
  BLE profile, `_max_count`/`_default_size` == `ZMK_BLE_PROFILE_COUNT`). This
  replaces the old per-element `ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE` +
  `#if` ladder, removed by that module's P3 rework in favour of one descriptor
  owning a single contiguous backing buffer. Individual profiles are still
  reached with `zmk_custom_setting_find_array_element(...)`. The module
  `select`s `ZMK_CUSTOM_SETTINGS_ARRAY` (see `Kconfig`) so the array feature
  is force-enabled when custom settings are present.
  - `ble_detected[i]` (`INT32`, default `ZMK_OS_UNKNOWN`) — last auto-guess.
  - `ble_override[i]` (`INT32`, default `0` == `ZMK_OS_UNKNOWN` == AUTO).
  - Use `ZMK_CUSTOM_SETTING_NO_CONSTRAINT`, **not**
    `ZMK_CUSTOM_SETTING_RANGE_INT32` — the latter is a known compile failure
    (nested compound literal in a `STRUCT_SECTION_ITERABLE` static
    initializer, C11 6.6p9, arm-zephyr-eabi-gcc). Validate the 0-3 range in
    the RPC handler instead.
  - On (re)connect: adopt the cached `ble_detected` immediately (no guessing
    delay for known devices), then re-run detection in the background and
    persist+raise-changed only on a differing result.
  - `ble_override` is written by the RPC handler via
    `zmk_custom_setting_write_array_by_key(..., ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST)`
    and re-evaluates the effective OS + raises `zmk_os_changed` immediately.

## Custom Studio RPC

Proto (nanopb, no 64-bit fields):

```proto
syntax = "proto3";
package cormoran.os_detection;

enum Os { OS_UNSPECIFIED = 0; OS_UNKNOWN = 1; OS_WINDOWS = 2; OS_MACOS = 3; OS_LINUX = 4; OS_IOS = 5; OS_ANDROID = 6; }

message GetStateRequest {}
message SetBleOverrideRequest { uint32 profile_index = 1; Os os = 2; }

message UsbState { bool connected = 1; Os detected = 2; }
message BleProfileState {
    uint32 index = 1; bool bonded = 2; bool connected = 3;
    Os detected = 4; Os override = 5; Os effective = 6;
}
message StateResponse {
    UsbState usb = 1;
    repeated BleProfileState ble_profiles = 2; // max_count = CONFIG_ZMK_BLE_PROFILE_COUNT (5)
    uint32 active_profile_index = 3;
    Os current_effective = 4;
}
message SetBleOverrideResponse { BleProfileState profile = 1; }
message ErrorResponse { string message = 1; }

message Request {
    oneof request_type { GetStateRequest get_state = 1; SetBleOverrideRequest set_ble_override = 2; }
}
message Response {
    oneof response_type { ErrorResponse error = 1; StateResponse state = 2; SetBleOverrideResponse set_ble_override = 3; }
}
```

Note: `cormoran_os_detection_Os` (nanopb generated name) is a *separate* type
from the firmware's own `enum zmk_os` — the handler translates between them
explicitly (`OS_UNSPECIFIED` only appears in proto to give oneof-less enums a
zero value; firmware-side `ZMK_OS_UNKNOWN` maps to proto `OS_UNKNOWN`, never
`OS_UNSPECIFIED`, which is reserved for "field not set").

Handler (`src/studio/os_detection_handler.c`) follows `template_handler.c`'s
shape: `ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__os_detection, ...)` +
`ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER`. `set_ble_override` validates
`profile_index < CONFIG_ZMK_BLE_PROFILE_COUNT` and `os` is one of the 4 valid
values before writing.

## Web UI

`web/src/App.tsx`: `zmkApp.findSubsystem("cormoran__os_detection")`, warning
banner when absent (existing template pattern). Connected view: USB card
(connected + detected OS), BLE profile table (index, bonded, connected,
detected, override, effective; highlight `active_profile_index`), per-row
`<select>` AUTO/WINDOWS/MACOS/LINUX → `set_ble_override` RPC → optimistic row
update from the response. Manual refresh button + `setInterval` poll (a few
seconds) while connected, cleared on unmount/disconnect.

## Test plan

- `tests/test/`: `ZMK_OS_DETECTION_TEST_INJECT=y` native_sim build injecting
  recorded Windows/macOS/Linux USB SETUP sequences and BLE stat snapshots at
  boot; assert classifier output via the existing `events.patterns` +
  `*.snapshot` log-scrape mechanism.
- `tests/studio/`: native_sim + `CONFIG_ZMK_STUDIO=y` (BLE stays off here,
  so only USB + RPC + settings paths are exercised); assert
  `cormoran__os_detection` appears in the boot subsystem list, and drive
  `get_state`/`set_ble_override` through the handler directly.
- `tests/zmk-config/`: real `xiao_ble`/`tester_xiao` build matrix (mirrors the
  template's 4-artifact pattern) asserting `CONFIG_ZMK_OS_DETECTION=y`,
  `CONFIG_USB_DEVICE_BOS=y`, `CONFIG_BT_GATT_AUTHORIZATION_CUSTOM=y`, and
  custom-settings integration in `.config`.
- `web/test/`: `createConnectedMockZMKApp` — state table renders, override
  dropdown sends the right payload, "subsystem not found" warning path.
- Real hardware: this sandbox can flash the physical XIAO board over SWD
  (see `skills/develop-zmk-module`) and it IS itself a real Linux USB host
  for the board (used today for `pyusb` Studio RPC), so a genuine Linux USB
  enumeration capture is possible here. Real Windows/macOS/BLE-host
  validation is out of reach in this sandbox and is documented as a manual
  step in README for the human maintainer to run before trusting the
  Windows/macOS thresholds.

## Known limitations (→ README)

Same list as the task brief, updated after real captures: wLength
fingerprints are OS-version-dependent; ChromeOS and Android (verified,
2026-07-05) enumerate identically to Linux over USB and are reported as
`ZMK_OS_LINUX`, no separate value; iOS *is* distinguished from macOS now
(verified, 2026-07-05 - see `ZMK_OS_IOS` above), but only by one narrow
signal (`SET_FEATURE(DEVICE_REMOTE_WAKEUP)`) that could plausibly not hold
across every macOS/iOS version; KVM/switches suppress
re-enumeration; `--wrap=usb_handle_bos` requires the legacy USB stack and
breaks if ZMK migrates to `device_next` (hard CMake error, not silent);
BLE detection is heuristic — manual per-profile override is the primary UX,
auto-detect is a first guess; Custom Studio RPC is unofficial and needs the
`cormoran/zmk` fork; detection only runs on the split central.
