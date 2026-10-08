# zmk-feature-os-detection

![ZMK Version](https://img.shields.io/badge/ZMK-cormoran%20fork-blue)
[![Test](https://github.com/cormoran/zmk-feature-os-detection/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-os-detection/actions/workflows/zmk-module.yml)

A ZMK module that detects the host OS (Windows / macOS·iOS / Linux / unknown)
over **USB** and **BLE**, raises a `zmk_os_changed` event, and exposes live
state + per-BLE-profile manual overrides through a Custom Studio RPC
subsystem and Web UI. It can optionally auto-switch a keymap layer per
detected OS too, though for that use case we recommend
[zmk-feature-default-layer](https://github.com/cormoran/zmk-feature-default-layer)
instead - see [Layer auto-switch](#layer-auto-switch-opt-in-not-recommended) below.

This uses the **unofficial** custom ZMK Studio RPC protocol from
[cormoran/zmk-module-template-with-custom-studio-rpc](https://github.com/cormoran/zmk-module-template-with-custom-studio-rpc).
License: MIT. This module does not reference or reuse any QMK (GPL-2.0)
source code.

## How detection works (short version)

- **USB**: a linker-level hook (`--wrap=usb_handle_bos`) observes every
  standard USB SETUP packet during enumeration - which `GET_DESCRIPTOR`
  requests are sent, at what `wLength`, and whether `GET_DESCRIPTOR(BOS)`
  is requested - without changing anything the host actually receives.
  After a short debounce once enumeration settles, the pattern is
  classified.
- **BLE**: `bt_gatt_authorization_cb_register()` observes which GATT
  characteristics the host reads while pairing/reconnecting (HID Report
  Map, HID Info, DIS PnP ID, GAP Appearance), plus ATT MTU and the initial
  connection interval - always authorizing the read, never blocking it.
- Detection results and any manual BLE override are combined into one
  **effective OS** per transport (override wins), and the effective OS
  for whichever endpoint is currently active is what
  `zmk_os_detection_current()` and the `zmk_os_changed` event report.

**Read [docs/fingerprints.md](docs/fingerprints.md) before relying on this
in production.** The classifier thresholds shipped here are placeholders
based on the general behavior described for each OS, not a verified
capture - see "Known limitations" below.

## Installing

Add this module and the patched ZMK fork it depends on to your
`config/west.yml`:

```yaml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-feature-os-detection
      remote: cormoran
      revision: main
      import: true
    # Required: patched ZMK with Custom Studio RPC support
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol
      import:
        file: app/west.yml
```

`import: true` on `zmk-feature-os-detection` also pulls in its own
dependency, [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings)
(needed for BLE per-profile override persistence).

## Kconfig

Enable in your `config/<shield>.conf`:

```conf
CONFIG_ZMK_OS_DETECTION=y

# Pick the transport(s) you want detection on. Each requires the matching
# ZMK transport (ZMK_USB / ZMK_BLE) to already be enabled.
CONFIG_ZMK_OS_DETECTION_USB=y
CONFIG_ZMK_OS_DETECTION_BLE=y

# Optional: iPhone/iPad ANCS/AMS probe as an extra BLE signal (off by default)
CONFIG_ZMK_OS_DETECTION_BLE_GATT_CLIENT_PROBE=y

# Optional: Custom Studio RPC + Web UI for live state and BLE overrides
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_OS_DETECTION_STUDIO_RPC=y
CONFIG_ZMK_CUSTOM_SETTINGS=y
CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
```

Full option list is in [Kconfig](./Kconfig).

## C API

```c
#include <cormoran/os-detection/os_detection.h>

enum zmk_os { ZMK_OS_UNKNOWN, ZMK_OS_WINDOWS, ZMK_OS_MACOS, ZMK_OS_LINUX, ZMK_OS_IOS, ZMK_OS_ANDROID };

enum zmk_os zmk_os_detection_current(void); // effective OS for the active endpoint

// Subscribe to zmk_os_changed via ZMK_LISTENER/ZMK_SUBSCRIPTION to react to changes.
```

## Layer auto-switch (opt-in, not recommended)

This module can optionally auto-activate a keymap layer per detected OS by
listening to its own `zmk_os_changed` event. **This overlaps with
[zmk-feature-default-layer](https://github.com/cormoran/zmk-feature-default-layer),
which we recommend instead** - it's a dedicated, more capable module for
per-host default layer selection. Use this module's built-in switch only if
you specifically want it driven by this module's OS detection alone, with
no extra module dependency.

It's off by default and must be explicitly enabled:

```conf
CONFIG_ZMK_OS_DETECTION_LAYER_AUTO_SWITCH=y

# Layer to auto-activate per detected OS (-1 = disabled, default)
CONFIG_ZMK_OS_DETECTION_LAYER_WINDOWS=1
CONFIG_ZMK_OS_DETECTION_LAYER_MACOS=2
CONFIG_ZMK_OS_DETECTION_LAYER_LINUX=3
CONFIG_ZMK_OS_DETECTION_LAYER_IOS=4
CONFIG_ZMK_OS_DETECTION_LAYER_ANDROID=5
CONFIG_ZMK_OS_DETECTION_LAYER_UNKNOWN=-1
```

## Web UI

See [web/README.md](./web/README.md) for local web development.

1. Connect Studio to your keyboard (Web Serial - Chrome/Edge only) at the
   published UI URL: https://cormoran.github.io/zmk-feature-os-detection/
2. The **USB** card shows whether USB is connected and the detected OS.
3. The **BLE Profiles** table lists every paired profile (bonded /
   connected / detected / override / effective), with the currently active
   profile highlighted. Use the dropdown in the **Override** column to pin
   a profile to Windows/macOS/Linux, or back to **AUTO**. The override is
   saved to the keyboard's persistent settings immediately and survives
   reboots.

**Publishing**: merging to `main` builds and deploys the Web UI to GitHub
Pages automatically (see `.github/workflows/web-ui.yml`).

## Known limitations

- **Fixed 2026-07-05: enabling `CONFIG_ZMK_OS_DETECTION_USB` used to break
  Windows enumeration entirely.** Selecting `CONFIG_USB_DEVICE_BOS` bumped
  `bcdUSB` to 2.01 without this module ever registering a valid BOS
  capability, so the device answered `GET_DESCRIPTOR(BOS)` with a
  spec-invalid, zero-length descriptor — real Windows aborted enumeration
  on it (retried 3 times, never showed up as a keyboard), while Linux/macOS
  tolerated it and worked fine. If you're on a version older than this fix,
  update. See [docs/fingerprints.md](docs/fingerprints.md)'s "Windows
  enumeration failure discovered and fixed" section and
  [docs/windows-usb-enumeration-issue.md](docs/windows-usb-enumeration-issue.md)
  for the full root-cause writeup.
- **USB fingerprints are OS-version-dependent.** The `wLength` pattern a
  given Windows/macOS/Linux version requests can change between OS
  releases; treat the classifier as a best-effort guess, not a guarantee.
- **Any OS sharing the Linux kernel's USB host stack enumerates like
  Linux over USB and is reported as `ZMK_OS_LINUX`** — ChromeOS, and (real
  capture, 2026-07-05) Android in USB-host/OTG mode too, whose kernel-level
  enumeration was byte-for-byte identical to desktop Linux's. There's no
  separate ChromeOS value in `enum zmk_os` - this is by design, not a gap to
  fill. **Android is the exception over BLE specifically**: its GATT read
  pattern (real capture, 2026-07-05) turned out to actually differ from
  desktop Linux's (it never reads GAP Appearance), so `ZMK_OS_ANDROID` is a
  distinct value reachable only via BLE detection, not USB.
- **macOS and iOS are NOT distinguished over USB; both report `ZMK_OS_MACOS`.**
  A 2026-07-19 side-by-side capture of a real Mac and a real iPhone (see
  [docs/fingerprints.md](docs/fingerprints.md)) found their USB enumeration
  byte-for-byte identical — they share Apple's USB stack. An earlier revision
  split iOS out because a real iPhone sent `SET_FEATURE(DEVICE_REMOTE_WAKEUP)`,
  but the real Mac sends it too (the earlier macOS capture had just stopped one
  packet too early), so a Mac was being misdetected as iOS; that heuristic was
  removed. This is the same "shared stack ⇒ can't tell apart over USB" situation
  as ChromeOS/Android above. `ZMK_OS_IOS` is still produced over **BLE** (opt-in
  ANCS/AMS probe), where the distinction is real.
- **KVM switches and some USB hubs don't force re-enumeration** on switch,
  so USB detection can miss a host change until the next physical
  reconnect.
- **The USB hook requires Zephyr's legacy USB device stack**
  (`CONFIG_USB_DEVICE_STACK`). If ZMK ever migrates to the newer
  `device_next` stack, `--wrap=usb_handle_bos` stops applying and the
  build will fail loudly (a `CMakeLists.txt` check turns this into a clear
  error rather than a silently-broken feature) - the long-term fix is an
  observation hook contributed to ZMK itself
  ([zmkfirmware/zmk#2553](https://github.com/zmkfirmware/zmk/issues/2553)).
- **BLE detection is inherently heuristic, and currently reports live with
  no debounce.** The reported OS can visibly change multiple times within
  one connection as more GATT characteristics get read (a real Windows
  capture on 2026-07-05 briefly reported Linux, then macOS, before settling
  on Windows within about 2 seconds) - see
  [docs/fingerprints.md](docs/fingerprints.md)'s "Real bug this capture
  exposed" note. Treat per-profile manual override via the Web UI as the
  primary way to get a reliable result; auto-detection is only a first
  guess, refined as more GATT signals arrive, and anything reacting to
  `zmk_os_changed` mid-connection (e.g. auto-layer-switch) can see a
  transient wrong value.
- **BLE cannot reliably distinguish Windows from Linux.** Real captures
  (2026-07-05, both a real Windows PC and this project's own Linux
  dev host's BlueZ) read the *identical* set of GATT characteristics -
  the "Linux skips DIS PnP ID/GAP Appearance" assumption doesn't hold for
  at least one real, current BlueZ version. Since there's no reliable
  read-set signal to split them, `zmk_os_classify_ble()` deliberately
  resolves this ambiguous case to `ZMK_OS_WINDOWS` (larger install base) -
  Linux users hitting this should use the Web UI's manual per-profile
  override. See [docs/fingerprints.md](docs/fingerprints.md)'s "Real Linux
  capture" section for the full data and reasoning.
- **Custom Studio RPC is an unofficial protocol** and requires the
  `cormoran/zmk` fork (`main+custom-studio-protocol`) rather than upstream
  ZMK.
- **Split keyboards**: detection only runs on the central side, since only
  the central owns the USB/BLE host connection.
- USB detection is verified against real captures for macOS, Windows, and
  Linux (2026-07-05), plus Android confirmed to enumerate identically to Linux
  and a real Mac + iPhone confirmed to enumerate identically to each other
  (both report macOS) on 2026-07-19. BLE detection is verified against real
  captures for
  Windows, Linux (see above - resolves to Windows on purpose), Android
  (reported distinctly as `ZMK_OS_ANDROID`), and (partially - DIS/HIDS
  Report Map only) macOS and iPhone, also 2026-07-05.
- **BLE cannot distinguish iOS from macOS either.** A real iPhone capture
  read only the HIDS Report Map, matching the fallback rule that also
  covers macOS - `ZMK_OS_IOS` is never produced by a real BLE capture today
  (the ANCS/AMS-based signal meant to detect it,
  `CONFIG_ZMK_OS_DETECTION_BLE_GATT_CLIENT_PROBE`, is an unimplemented
  Kconfig stub). See [docs/fingerprints.md](docs/fingerprints.md)'s "Real
  iPhone capture" section for exactly what was captured, what's still a
  guess, and how to update either with new real capture data (including a
  working J-Link RTT capture recipe).

## Development

### Setup

```bash
git clone https://github.com/cormoran/zmk-feature-os-detection
cd zmk-feature-os-detection
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export
```

(Dev container users: this is done automatically.)

### Pre-commit

```bash
pip install pre-commit
pre-commit install
pre-commit run --all-files
```

If the `prettier`/`eslint`/`jest`/`web-build` hooks fail with `Error:
Cannot find module 'node:path'`, pre-commit's own managed Node environment
picked up a stale/wrong Node install for its bootstrap - this was seen
intermittently in this project's original dev sandbox (which has more than
one Node on `PATH`) after switching between running `pre-commit` inside a
nix devshell and outside it, but disappeared for good after `rm -rf
~/.cache/pre-commit` followed by one clean `pre-commit run --all-files`
(the corrupted/mismatched cache was the actual cause, not a fundamental
environment conflict - don't reach for a permanent `SKIP=` workaround for
this).

### Tests

```bash
# Unit tests (native_sim, incl. TEST_INJECT USB/RPC scenarios) + build tests
python3 -m unittest
# Build tests only
west zmk-build tests/zmk-config
# Unit tests only
west zmk-test tests -m .
# Web UI tests
cd web && npm test
```

### Manual real-hardware verification

Automated tests cover the classifier logic and RPC/build wiring, but the
actual fingerprint thresholds need verification against real hosts before
you can trust them:

1. Flash a board with `CONFIG_ZMK_OS_DETECTION_USB=y` (and `_BLE=y` if
   testing BLE) enabled - `tests/zmk-config`'s `os_detection_board_with_rpc`
   and `os_detection_custom_settings_board` artifacts already have this
   (plus [zmk-module-ble-management](https://github.com/cormoran/zmk-module-ble-management)
   and `CONFIG_ZMK_STUDIO_LOCKING=n`, so you can switch/unpair BLE profiles
   from its web UI and connect Studio without an unlock key-combo while
   repeatedly re-pairing to different hosts below).
2. **USB**: already verified against a real Mac, a real Windows PC, a real
   Linux machine, a real Android device (OTG host mode), and a real iPhone
   (see [docs/fingerprints.md](docs/fingerprints.md)). If you want to
   re-verify against a different OS version, plug into that
   host and capture the enumeration with `usbmon` (Linux: `sudo modprobe
   usbmon; sudo cat /sys/kernel/debug/usb/usbmon/1u`), Wireshark+USBPcap
   (Windows), or Wireshark's built-in USB capture (macOS) - or, if you have
   J-Link SWD access to the board (independent of whatever host its USB-C
   is plugged into), flash a debug build and read the module's own
   `zmk_os_detection_observe_setup()` `LOG_DBG` output straight out of
   target RAM; see [docs/fingerprints.md](docs/fingerprints.md)'s "RTT
   capture recipe" for the exact commands (this is how the existing
   signatures were captured). Compare against `zmk_os_detection_current()`
   / the Web UI's USB card, and update the thresholds in
   `src/os_detection_usb.c` + `docs/fingerprints.md` to match what you
   actually captured.
3. **BLE**: pair the same board with a Windows PC, a Mac, an iPhone, an
   Android device, and a Linux desktop (already done once, 2026-07-05 - see
   [docs/fingerprints.md](docs/fingerprints.md)). Check the Web UI's BLE
   profile table for each; use `btmon` (Linux) or `PacketLogger` (macOS) to
   see the actual GATT access pattern if the auto-detected OS is wrong, and
   adjust `src/os_detection_ble.c` + `docs/fingerprints.md`.
4. Confirm overrides set from the Web UI persist across a reboot.

### Sync changes from template

Run `Actions > Sync Changes in Template > Run workflow` to pull in
upstream template changes as a pull request.

### Coding agent on actions

- Mention `@copilot`
- Setup `ANTHROPIC_API_KEY` secret and mention `@claude`
