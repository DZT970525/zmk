# ZMK Feature: Device Info

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-feature-device-info/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-device-info/actions/workflows/zmk-module.yml)

A ZMK module that exposes keyboard diagnostic information via the **unofficial** custom ZMK Studio RPC protocol. The information is displayed in a web UI and is intended to help end-users troubleshoot their keyboard firmware.

## What information is shown

| Section | Contents |
|---------|----------|
| **Build** | ZMK git hash, zmk-config git hash, this module's git hash, Zephyr version, build timestamp, board name, and a **build hash**. Each version shows a "dirty" warning if the source had uncommitted changes at build time. |
| **Hardware** | MCU hardware unique ID, last reset cause (Power-On / Watchdog / Brownout / etc.), flash and SRAM size. |
| **ZMK Configuration** | KScan driver type, BLE/USB/split/display/RGB/backlight flags. |
| **Runtime** | Uptime since last boot. |
| **Zephyr Devices** | All statically-declared Zephyr devices and whether each initialized successfully. Failing devices are shown first. |

A **Copy JSON** button lets users paste the full diagnostic data into a bug report.

## Module User Guide

### 1. Add to `config/west.yml`

This module requires a patched ZMK with custom Studio RPC support.

```yml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-feature-device-info
      remote: cormoran
      revision: main
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol
      import:
        file: app/west.yml
```

### 2. Enable in `config/<shield>.conf`

```conf
CONFIG_ZMK_DEVICE_INFO=y

# Enable the web UI RPC (requires ZMK Studio)
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC=y

# Optional: allow reading device info without unlocking Studio first.
# By default a Studio unlock is required before any device info can be read.
# CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC_REQUIRE_UNLOCK=n
```

### 3. Open the web UI

Connect your keyboard via USB and open:
`https://cormoran.github.io/zmk-feature-device-info/`

Or run locally: see [web/README.md](./web/README.md).

## Notes

- **zmk-config version**: The module automatically reads the git hash of your zmk-config directory at build time (via the `ZMK_CONFIG` CMake variable set by `west build`). No extra setup is needed.
- **Hardware unique ID / reset cause**: Requires `CONFIG_HWINFO=y` (enabled automatically when supported by the board).
- **Build hash**: A [GNU build-id](https://interrupt.memfault.com/blog/gnu-build-id-for-firmware) — a SHA1 of the linked firmware image — is embedded in flash and reported here. It uniquely identifies the exact binary a device is running, so you can match a keyboard back to the matching ELF for crash/error analysis. Find the same value in a build artifact with:

  ```bash
  readelf -n build/zephyr/zmk.elf   # look for the "Build ID" line
  ```

  The module enables the build-id automatically (Zephyr disables it by default) whenever the Studio RPC feature is on; no configuration is required.

## Development

See [README in zmk-module-template](https://github.com/cormoran/zmk-module-template) for the full development guide including how to run tests.

```bash
# Run unit test + build test
python3 -m unittest

# Run web tests
cd web && npm test
```
