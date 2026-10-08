# Device Info Module — Design

## Goal

Expose device diagnostic information via a custom ZMK Studio RPC subsystem, presented in a web UI, to help end-users troubleshoot their keyboard firmware.

---

## Information to Expose

### 1. Build Info

Answers: *"Which firmware is this keyboard running?"*

| Field | Source | Notes |
|-------|--------|-------|
| `zmk_version` | `APP_BUILD_VERSION` (from `app_version.h`) | `git describe --dirty` of ZMK source; includes `-dirty` suffix if uncommitted changes |
| `zmk_version_dirty` | derived from `zmk_version` | `true` if `zmk_version` contains `dirty` |
| `zmk_config_version` | CMake-injected `ZMK_CONFIG_BUILD_VERSION` | `git describe --dirty` of the zmk-config directory; see CMake note below |
| `zmk_config_version_dirty` | derived | `true` if `zmk_config_version` contains `dirty` |
| `module_version` | CMake-injected `MODULE_BUILD_VERSION` | `git describe --dirty` of this module, embedded at build time |
| `module_version_dirty` | derived | `true` if `module_version` contains `dirty` |
| `zephyr_version` | `KERNEL_VERSION_STRING` (from `version.h`) | e.g. `"4.1.0"` |
| `build_timestamp` | CMake-injected `BUILD_TIMESTAMP` | ISO-8601 string, e.g. `"2025-05-01T12:34:56"` |
| `board` | `CONFIG_BOARD` Kconfig string | e.g. `"xiao_ble"` |
| `build_hash` | GNU build-id read from `.note.gnu.build-id` | SHA1 of the linked image (hex); uniquely identifies the ELF/binary; empty if unavailable |

**zmk-config version injection:**
ZMK's build system exposes the config directory as the `ZMK_CONFIG` CMake variable (set by `west zmk-build` / `west build -d ... -- -DZMK_CONFIG=...`). The module's CMakeLists.txt runs `git -C ${ZMK_CONFIG} describe --dirty --always --tags` at configure time and injects the result as the `ZMK_CONFIG_BUILD_VERSION` compile definition. If `ZMK_CONFIG` is not set or is not a git repo, the field is left empty.

**Rationale for additional fields:**
- `zmk_config_version`: The user's zmk-config is the most common source of bugs; knowing its exact commit instantly identifies whether a known-bad config is in use.
- `zephyr_version`: Many issues are Zephyr-version-specific (API changes, HAL bugs).
- `build_timestamp`: Confirms which build is running even when the git hash alone is ambiguous.
- `board`: Allows the support person to instantly know the hardware target without asking.
- `build_hash`: The git hashes describe *source* revisions but do not uniquely identify the *binary* (a "dirty" tree, different toolchain, or config change all produce the same describe output). The GNU build-id is a SHA1 of the actual linked image, so it uniquely names the ELF — letting you pick the matching artifact (with symbols) for crash/error analysis.

**build_hash injection (GNU build-id):**
Zephyr disables build-id by default (`--build-id=none` in the linker `base` property). The module's `CMakeLists.txt` re-enables it with `zephyr_link_libraries(-Wl,--build-id=sha1)`; because module CMake is processed *after* Zephyr appends its `base` flags on the final link line, and `ld` honors the last `--build-id`, the module's flag wins. A `ROM_SECTIONS` linker snippet (`src/build_id.ld`) pins the resulting `.note.gnu.build-id` into the flash (ROMABLE) region with `__build_id_note_start` / `__build_id_note_end` boundary symbols — otherwise the orphan note can be assigned address 0. The handler parses the ELF note at runtime and hex-encodes the descriptor. The boundary symbols always exist, so builds without build-id emission (e.g. the note range is empty) simply report an empty hash. Works on the nRF52840 / Adafruit UF2 bootloader: the note is ordinary flash rodata inside the app image, so it is included in the `.uf2` and is directly memory-mapped for reading.

---

### 2. Hardware Info

Answers: *"What MCU is this, and what happened before I connected?"*

| Field | Source | Notes |
|-------|--------|-------|
| `device_id` | `hwinfo_get_device_id()` | Hardware unique ID bytes (hex-encoded in proto as string) |
| `reset_cause` | `hwinfo_get_reset_cause()` | Bitmask of reset reasons (POR, watchdog, brownout, software, etc.) |
| `flash_size_kb` | `CONFIG_FLASH_SIZE` | Total flash in KB (compile-time) |
| `sram_size_kb` | `CONFIG_SRAM_SIZE` | Total SRAM in KB (compile-time) |

**Additional proposal — `reset_cause`:**
This is highly valuable for troubleshooting spontaneous reboots. A watchdog reset suggests a firmware hang; a brownout reset suggests a power supply problem.

---

### 3. Zephyr Device Status

Answers: *"Are all the expected hardware drivers initializing correctly?"*

Use `z_device_get_all_static()` to iterate all statically-declared devices and report `device_is_ready()` for each. This gives a complete picture of what initialized successfully.

| Field | Source |
|-------|--------|
| `name` | `dev->name` |
| `ready` | `device_is_ready(dev)` |

> **Note:** This list can be long (~30–50 entries on a typical ZMK build). The web UI should filter/group them usefully (e.g., collapse passing devices, highlight failing ones).

---

### 4. ZMK Hardware Configuration

Answers: *"How is this keyboard wired up in software?"*

All fields are compile-time Kconfig/DT values, so this section never changes at runtime.

| Field | Source | Notes |
|-------|--------|-------|
| `kscan_compatible` | `DT_PROP(DT_CHOSEN(zmk_kscan), compatible)` | e.g. `"zmk,kscan-gpio-matrix"` or `"zmk,kscan-gpio-direct"` |
| `ble_enabled` | `IS_ENABLED(CONFIG_ZMK_BLE)` | |
| `ble_profile_count` | `CONFIG_BT_MAX_PAIRED` | Number of BLE slots |
| `usb_enabled` | `IS_ENABLED(CONFIG_ZMK_USB)` | |
| `split_enabled` | `IS_ENABLED(CONFIG_ZMK_SPLIT)` | |
| `split_role` | `IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)` | `"central"`, `"peripheral"`, or `"none"` |
| `display_enabled` | `IS_ENABLED(CONFIG_ZMK_DISPLAY)` | |
| `rgb_underglow_enabled` | `IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)` | |
| `backlight_enabled` | `IS_ENABLED(CONFIG_ZMK_BACKLIGHT)` | |
| `battery_level_enabled` | `IS_ENABLED(CONFIG_ZMK_BATTERY_LEVEL_FETCHING)` | |

**Additional proposals:**
- `split_role`: Split keyboards are a common source of confusion — knowing which half is which, at a glance, is very helpful.
- `kscan_compatible`: Tells you immediately whether direct-pin or matrix wiring is in use, which affects ghosting and noise troubleshooting.

---

### 5. Runtime Status

Answers: *"When did the keyboard last reboot?"*

| Field | Source | Notes |
|-------|--------|-------|
| `uptime_ms` | `k_uptime_get()` | ms since last boot; confirms whether device rebooted recently |

---

## Protobuf Schema

File: `proto/zmk/device_info/device_info.proto`

```proto
syntax = "proto3";
package zmk.device_info;

message GetDeviceInfoRequest {}

message BuildInfo {
    string zmk_version         = 1;  // git describe of ZMK (may include "-dirty")
    bool   zmk_dirty           = 2;
    string zmk_config_version  = 3;  // git describe of zmk-config dir (empty if unavailable)
    bool   zmk_config_dirty    = 4;
    string module_version      = 5;  // git describe of this module
    bool   module_dirty        = 6;
    string zephyr_version      = 7;  // e.g. "4.1.0"
    string build_timestamp     = 8;  // ISO-8601 build time
    string board               = 9;  // CONFIG_BOARD string
    string build_hash          = 10; // GNU build-id (SHA1 of image, hex)
}

message HardwareInfo {
    string device_id     = 1;  // hex-encoded unique ID
    uint32 reset_cause   = 2;  // bitmask; see hwinfo.h RESET_* flags
    uint32 flash_size_kb = 3;
    uint32 sram_size_kb  = 4;
}

message ZephyrDevice {
    string name  = 1;
    bool   ready = 2;
}

message ZmkConfig {
    string kscan_compatible      = 1;
    bool   ble_enabled           = 2;
    uint32 ble_profile_count     = 3;
    bool   usb_enabled           = 4;
    bool   split_enabled         = 5;
    string split_role            = 6;  // "central", "peripheral", or "none"
    bool   display_enabled       = 7;
    bool   rgb_underglow_enabled = 8;
    bool   backlight_enabled     = 9;
    bool   battery_level_enabled = 10;
}

message RuntimeStatus {
    uint64 uptime_ms = 1;
}

message DeviceInfoResponse {
    BuildInfo     build          = 1;
    HardwareInfo  hardware       = 2;
    repeated ZephyrDevice zephyr_devices = 3;
    ZmkConfig     zmk_config     = 4;
    RuntimeStatus runtime        = 5;
}

message ErrorResponse { string message = 1; }

message Request {
    oneof request_type {
        GetDeviceInfoRequest get_device_info = 1;
    }
}

message Response {
    oneof response_type {
        ErrorResponse      error       = 1;
        DeviceInfoResponse device_info = 2;
    }
}
```

---

## Firmware Implementation Plan

1. **CMake**: At configure time, run `git describe --dirty --always --tags` in:
   - This module's source dir → `MODULE_BUILD_VERSION`
   - `${ZMK_CONFIG}` (if set and is a git repo) → `ZMK_CONFIG_BUILD_VERSION`
   - Also inject `BUILD_TIMESTAMP` (current UTC time as ISO-8601).
2. **Proto**: Define `proto/zmk/device_info/device_info.proto` (+ `.options` for string max sizes).
3. **Handler** (`src/studio/device_info_handler.c`):
   - Collect build info from `version.h` / `app_version.h` + CMake-injected defines.
   - Call `hwinfo_get_device_id()` / `hwinfo_get_reset_cause()`, guarded by `CONFIG_HWINFO`.
   - Iterate `z_device_get_all_static()` for device list.
   - Fill ZMK config from `IS_ENABLED()` / DT macros.
   - Fill runtime status from `k_uptime_get()`.
4. **Kconfig** (`Kconfig`): Add `CONFIG_ZMK_DEVICE_INFO` and `CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC`.
5. **CMakeLists.txt**: Wire up new proto and source files.

---

## Web UI Plan

- Single `DeviceInfoPanel` component that calls the RPC on load.
- Sections collapsible: Build, Hardware, Devices, ZMK Config, Runtime.
- "Build" section: highlight dirty flags visually (yellow warning if any repo was dirty at build).
- "Devices" section: green/red badge per device, failing devices shown first.
- "Reset cause" rendered as human-readable labels (e.g., "Power-On Reset", "Watchdog").
- "Copy to clipboard" button that serializes everything to JSON (useful for bug reports).

---

## Open Questions / Future Work

- **zmk-config version when ZMK_CONFIG is unset**: If the user builds without `-DZMK_CONFIG=...` (e.g., in-tree builds), the field will be empty. Could fall back to `APPLICATION_SOURCE_DIR`.
- **Settings/NVS health**: Could report whether ZMK settings storage initialized successfully. Needs access to settings subsystem internals.
- **Split peripheral status**: Central-side could report whether each peripheral is connected and its RSSI. Requires cross-half RPC, out of scope for v1.
