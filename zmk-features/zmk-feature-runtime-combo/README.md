# zmk-feature-runtime-combo

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-feature-runtime-combo/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-runtime-combo/actions/workflows/zmk-module.yml)

Runtime Combo is a ZMK module that adds combos which can be edited at runtime
through the unofficial custom ZMK Studio RPC protocol.

A combo watches multiple key positions. When all configured positions are pressed
within the combo timeout, the configured ZMK behavior is invoked. Each combo is
stored by numeric slot internally and can also have a display name for the Web UI.

## Features

- Runtime editable combo slots.
- Compile-time default combos declared in the `.keymap`, active immediately
  after flashing and restorable after a runtime edit.
- Separate RPC methods for combo content and combo names to keep request payloads
  small.
- Compact binary storage for combo bodies.
- Global timeout, slow-release, and require-prior-idle settings shared by all
  runtime combos, with per-combo overrides for all three.
- Native-parity overlap resolution: if one combo's positions are a subset of
  another enabled combo's positions, the longer combo always wins.
- Up to 63 combo slots, decoded into a RAM cache so the key-event hot path
  never touches the settings registry.
- Name and combo arrays stored through
  [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings).
- React Web UI for listing and editing combo slots, with a clickable layout
  for picking positions and a searchable behavior list.

## Storage Format

The combo body is saved as one packed byte-array custom setting per combo slot:

```text
version, flags, position_count, layer_mask, timeout_ms,
require_prior_idle_ms, positions[]
```

`positions[]` are 16-bit values. `timeout_ms` and `require_prior_idle_ms` are
per-combo overrides; `0` means inherit the corresponding global setting. The
`flags` byte also carries a tri-state slow-release override (inherit / on /
off).

The behavior binding is stored separately as a
[`BEHAVIOR`-typed custom setting](https://github.com/cormoran/zmk-feature-custom-settings)
array with the same index, so its behavior local ID and parameters are
validated against the target behavior's parameter metadata on write and exported
as a first-class behavior binding rather than opaque bytes. The name is likewise
saved as a string-array custom setting with the same index.

Timeout, slow-release, and require-prior-idle mode are saved as separate global
custom settings and apply to every runtime combo unless overridden. The global
settings RPC response also reports `max_combo`, the maximum number of combo
slots configured in the firmware.

## Compile-Time Default Combos

Default combos can be declared directly in the `.keymap` using a
`cormoran,runtime-combo-defaults` node, so they work immediately after
flashing and survive a settings erase:

```dts
/ {
    runtime_combo_defaults {
        compatible = "cormoran,runtime-combo-defaults";

        combo_esc {
            slot = <0>;             // optional; defaults to definition order
            key-positions = <0 1>;
            bindings = <&kp ESC>;
            layers = <0 1>;         // optional; omitted = all layers
            display-name = "Esc";   // optional; defaults to the node name
            timeout-ms = <30>;      // optional per-combo override
            require-prior-idle-ms = <125>; // optional per-combo override
            slow-release;           // optional; forces slow-release on
        };
    };
};
```

A slot's effective value follows this order:

1. **Runtime override** — if the slot has ever been written or explicitly
   disabled through the RPC, that value always wins (a disabled/deleted slot
   suppresses its default rather than falling back to it).
2. **Compile-time default** — used whenever the slot has no stored override.
3. **Empty** — a slot with neither a default nor a stored value.

The Web UI shows each slot's source (`Default`, `Overridden`, `Runtime`, or
`Empty`) as a badge, and a slot with a default shows a **Reset to Default**
button that erases the stored override and restores the compile-time values.

## Overlap Resolution

If pressing a set of keys exactly completes one combo, but a second enabled
combo also matches every currently pressed position and needs one or more
additional positions, the shorter combo does not fire immediately. It only
fires once no longer combo remains viable: because an unrelated key is pressed,
the timeout expires, or one of the pressed keys is released. Whichever combo
ends up firing always uses only its own positions; any positions pressed while
waiting on a longer candidate are replayed as normal key presses if that
candidate never completes.

## User Guide

1. Add the module to `config/west.yml`.

   ```yml
   manifest:
     remotes:
       - name: cormoran
         url-base: https://github.com/cormoran
     projects:
       - name: zmk-feature-runtime-combo
         remote: cormoran
         revision: main
         import: true
       - name: zmk
         remote: cormoran
         revision: main+custom-studio-protocol
         import:
           file: app/west.yml
   ```

   This module imports `zmk-feature-custom-settings`. The custom Studio RPC Web
   UI requires ZMK from cormoran's `main+custom-studio-protocol` branch.

2. Enable the feature in `config/<shield>.conf`.

   ```conf
   CONFIG_ZMK_RUNTIME_COMBO=y

   # Required for the custom Studio RPC Web UI
   CONFIG_ZMK_STUDIO=y
   CONFIG_ZMK_RUNTIME_COMBO_STUDIO_RPC=y
   CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
   CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_REQUEST_PAYLOAD_MAX_BYTES=96
   CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
   ```

3. Open the Web UI from the custom subsystem URL or run it locally:

   ```bash
   cd web
   npm install
   npm run dev
   ```

4. Connect to the keyboard, choose a slot, and set:

   - Name: display name only.
   - Behavior: pick by name from the connected firmware's behavior list (falls
     back to a raw numeric id until the list loads).
   - Param 1, Param 2: the same binding fields ZMK Studio uses.
   - Layer mask: `0` means all layers.
   - Positions: click keys on the rendered layout below the form to toggle
     them in or out of the combo.
   - Timeout ms / Require prior idle ms: `0` inherits the corresponding global
     setting; a nonzero value overrides it for this combo only.
   - Slow release: `Inherit global` follows the Global Settings panel; `On`/`Off`
     overrides it for this combo only.

Set global timeout, slow-release mode, and require-prior-idle time in the
Global Settings panel. Require-prior-idle suppresses a combo if a non-modifier
key was tapped within that many milliseconds before the combo's first key press
(useful for avoiding accidental combos while typing fast); `0` disables it.
Enable the corresponding persist checkbox before saving if the setting should
survive reboot. The panel also shows the maximum number of runtime combo slots
available on the connected firmware.

## Development

```bash
# Run unit test + build test and verify results
python3 -m unittest

# Run build test directly
west zmk-build tests/zmk-config

# Run unit test directly
west zmk-test tests -m .

# Run web tests
cd web && npm test
```

The Web protobuf TypeScript files are generated with:

```bash
cd web
npm run generate
```
