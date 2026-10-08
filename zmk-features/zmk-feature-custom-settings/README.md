# ZMK Custom Settings

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)

This module provides a typed custom settings registry for ZMK modules and an
unofficial custom ZMK Studio RPC interface for editing those settings from the
web UI.

## Features

- Register settings from any module with a custom subsystem namespace and key.
  RPC clients address the namespace by its ZMK Studio custom subsystem index.
- Store typed values: bytes, int32, bool, string, behavior bindings, and
  dynamic arrays of those scalar types.
- Validate writes with optional range, option-list, HID usage, layer ID, or
  behavior ID constraints. Behavior values are additionally validated against
  the target behavior's own ZMK parameter metadata.
- Write values in memory, persist them to flash, discard memory changes, or
  reset persisted values back to defaults.
- Take part in ZMK's official factory reset: a ZMK Studio "reset settings"
  wipes every custom setting (and split peripherals' custom settings) too.
- Mark values as device-private, RPC-personal, or RPC-public.
- Require Studio unlock independently for reads and writes.
- Notify Studio clients when values change.
- Let RPC clients create and delete user-named entries (profiles, macros, ...)
  at runtime through a fixed-size keyspace slot pool, with no heap and no
  per-entry module code required to survive a reboot.

## Module User Guide

### Add The Module

Add the module to `config/west.yml`. This module uses the custom Studio RPC
support from cormoran's ZMK branch.

```yml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-feature-custom-settings
      remote: cormoran
      revision: main
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol
      import:
        file: app/west.yml
```

### Enable The Module

Enable the module in your keyboard config:

```conf
CONFIG_ZMK_CUSTOM_SETTINGS=y

# Optional: expose settings through the custom Studio RPC subsystem.
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
```

`CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE` must be large enough for custom settings
RPC requests.
`CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE` should be increased because listing
settings builds encoded notifications from the low priority workqueue.
When Studio RPC is enabled, a single-frame *write* is limited to 64 bytes (the
`WriteSetting` request decodes into a fixed carrier) and setting keys are
limited to 48 bytes by the generated RPC schema; reads have no such limit
(`GetSetting`/`ListSettings` stream a value of any size). Individual
`ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES`/`ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING`
settings can hold larger values (up to
`CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE`) by registering with an
explicit per-setting `max_size` and writing the value over the chunked RPC —
see [Large Values](#large-values-per-setting-max_size).

### Feature Selection

Beyond the always-available core (scalar/BYTES/STRING/BEHAVIOR settings,
memory/persist/temporary writes, save/discard/reset, Studio RPC list/get/write),
five optional features are gated behind their own Kconfig option. Each is
**enabled by default (`default y`)** so existing configurations keep working
unchanged; disable the ones your firmware doesn't use to shrink flash/RAM:

| Kconfig                                  | Enables                                                                          |
| ----------------------------------------- | --------------------------------------------------------------------------------- |
| `CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES` | Values larger than `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` and the shared value pool (`ZMK_CUSTOM_SETTING_DEFINE_SIZED`/`_POOLED`, `ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE`, the chunked write RPC). |
| `CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY`        | Array settings (`ZMK_CUSTOM_SETTING_ARRAY_DEFINE`, push/pop/insert/remove, array RPC). |
| `CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE`     | RPC-creatable keyspaces (`ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE`, `CreateSetting`/`DeleteSetting` RPC). Automatically selects `LARGE_VALUES` (a keyspace slot's `[key][payload]` blob is always pool-backed). |
| `CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS` | Per-setting bytes RPC serializer/deserializer hooks (`ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS` and the array/keyspace equivalents). |
| `CONFIG_ZMK_CUSTOM_SETTINGS_RECORD`       | Record/struct settings (`ZMK_CUSTOM_SETTING_RECORD_*`, the TLV codec). A record whose encoded size exceeds `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` additionally needs `LARGE_VALUES`. |

Disable any feature your firmware doesn't actually register settings for —
each one you turn off drops its translation unit from the build entirely (the
module is split one file per feature; see
[`docs/design/feature-gating-and-modularization.md`](docs/design/feature-gating-and-modularization.md)),
shrinking flash and RAM. Registering a setting with a macro whose feature is
disabled fails to *build* (a `BUILD_ASSERT` in the macro names the missing
`CONFIG_...`), not to link, so a misconfigured build is easy to diagnose.

As measured on `tests/zmk-config`'s `custom_settings_board_minimal` (module +
Studio RPC on, all five features off) vs `custom_settings_board_with_rpc`
(all five on), both with the same sample settings compiled in: minimal is
**~9.1 KiB smaller in flash** (243,457 vs 252,605 bytes, `text+data`) and
**~5.3 KiB smaller in RAM** (106,311 vs 111,755 bytes, `data+bss`). The exact
delta depends on which settings you register and their sizing knobs (see
[Memory Notes](#memory-notes)).

> **Upgrading from an earlier version:** these five gates previously defaulted
> to `y`. If your `.conf` does not already set them explicitly, a firmware
> update to this version will silently compile out any array/keyspace/large-
> value/converter/record setting you use — re-enable the gates your config
> needs (e.g. a module that registers a keyspace needs
> `CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=y`, which pulls in `LARGE_VALUES` via
> `select`) before upgrading.

### Split Keyboards

For split keyboards, enable ZMK's relay-event transport on both halves:

```conf
CONFIG_ZMK_SPLIT_RELAY_EVENT=y
CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY=y
# Optionally increase relay event data max length
# CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=256
```

Only the central half needs `CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y`. Split
peripherals can leave Studio and custom settings Studio RPC disabled; enabling
`CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY` is enough to build the protobuf
relay handler.

The custom settings relay payload size is
`CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN - 2`; the two overhead bytes store the
source and encoded payload size. Larger keys, setting values, or metadata-heavy
list responses may need a larger BLE MTU and split relay event data size.

Values that do not fit the relay envelope are **not** relayed across the
split — the notification omits an oversized value rather than failing — and
there is no chunked-write-over-relay path either. See
[Large Values](#large-values-per-setting-max_size) for the details.

### Register Settings

Register a setting from another module:

```c
#include <cormoran/zmk/custom_settings.h>

ZMK_CUSTOM_SETTING_DEFINE(
    // C symbol name for this setting registration.
    my_speed_setting,
    // Custom Studio subsystem id owned by your module.
    "my_module",
    // Setting key within that subsystem.
    "speed",
    // Value type exposed by the firmware API and RPC.
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    // Default value used when nothing is saved in flash.
    ZMK_CUSTOM_SETTING_VALUE_INT32(10),
    // RPC visibility: public, personal, or device-private.
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    // Read permission: secure requires Studio unlock.
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    // Write permission: secure requires Studio unlock.
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    // Optional validation rule; use NO_CONSTRAINT when unrestricted.
    ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));
```

The subsystem id and key are the stable identifiers used by firmware and RPC
clients. Pick short, unique names because they are encoded into settings keys
and Studio custom subsystem requests.

### Setting Reference

#### Value Types

| Value type                             | Default value helper                     | Description                                                                                            |
| -------------------------------------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES`  | `ZMK_CUSTOM_SETTING_VALUE_BYTES(...)`    | Raw bytes. Use this for binary data or for module-defined data that needs custom RPC conversion hooks. |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32`  | `ZMK_CUSTOM_SETTING_VALUE_INT32(value)`  | Signed 32-bit integer. Use this for numeric settings, indexes, and IDs.                                |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL`   | `ZMK_CUSTOM_SETTING_VALUE_BOOL(value)`   | Boolean setting.                                                                                       |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING` | `ZMK_CUSTOM_SETTING_VALUE_STRING(value)` | UTF-8/string setting stored with an explicit byte length.                                              |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR` | `ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(behavior_id, param1, param2)` | A ZMK behavior binding (behavior local ID plus its two binding parameters). See [Behavior Settings](#behavior-settings). |

#### Confidentiality

| Confidentiality                                     | RPC behavior                                                                                      | Typical use                                                                         |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE` | Value is not exposed over RPC.                                                                    | Device-local secrets or implementation details that Studio clients should not read. |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL`   | Value may be read over RPC, but clients should treat it as personal data and avoid publishing it. | User-specific preferences or values that may identify the user's setup.             |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC`     | Value may be read over RPC and exported/shared by clients.                                        | Layout, behavior, or tuning settings intended to be portable.                       |

#### Constraints

| Constraint helper or type                                        | Applies to      | Description                                                                                                                               |
| ---------------------------------------------------------------- | --------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `ZMK_CUSTOM_SETTING_NO_CONSTRAINT`                               | Any type        | No validation beyond matching the registered value type.                                                                                  |
| `ZMK_CUSTOM_SETTING_RANGE_INT32(min, max)`                       | `INT32`         | Requires the integer value to be between `min` and `max`, inclusive.                                                                      |
| `ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS`                          | Any scalar type | Requires the value to match one of the provided `struct zmk_custom_setting_options` entries. Optional labels are exposed in RPC metadata. |
| `ZMK_CUSTOM_SETTING_HID_USAGE(usage_page, usage_min, usage_max)` | `INT32`         | Requires an encoded HID usage whose page matches `usage_page` and whose usage is within the inclusive range.                              |
| `ZMK_CUSTOM_SETTING_LAYER_ID`                                    | `INT32`         | Requires a valid local ZMK layer ID.                                                                                                      |
| `ZMK_CUSTOM_SETTING_BEHAVIOR_ID`                                 | `INT32`         | Requires a valid local ZMK behavior ID when behavior local IDs are enabled.                                                               |

### Bytes RPC Conversion

Bytes settings may define RPC serializer/deserializer hooks. Firmware APIs and
flash storage keep the internal byte format; RPC read/write uses the converted
byte format. When hooks are omitted, bytes are copied as-is. Requires
[`CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS=y`](#feature-selection):

```conf
CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS=y
```

```c
static int my_blob_to_rpc(const struct zmk_custom_setting *setting,
                          const uint8_t *src, size_t src_size,
                          uint8_t *dest, size_t *dest_size,
                          size_t dest_capacity) {
    ARG_UNUSED(setting);

    // Encode an internal C struct into RPC bytes, for example with nanopb.
    if (src_size > dest_capacity) {
        return -EMSGSIZE;
    }
    memcpy(dest, src, src_size);
    *dest_size = src_size;
    return 0;
}

static int my_blob_from_rpc(const struct zmk_custom_setting *setting,
                            const uint8_t *src, size_t src_size,
                            uint8_t *dest, size_t *dest_size,
                            size_t dest_capacity) {
    ARG_UNUSED(setting);

    // Decode RPC bytes back into the firmware's internal C struct layout.
    if (src_size > dest_capacity) {
        return -EMSGSIZE;
    }
    memcpy(dest, src, src_size);
    *dest_size = src_size;
    return 0;
}

ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS(
    my_blob_setting, "my_module", "blob", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    ZMK_CUSTOM_SETTING_VALUE_BYTES(0, 0, 0, 0),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    my_blob_to_rpc, my_blob_from_rpc,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
```

### Record Settings (Structs)

A record setting stores a C struct as a single `BYTES` setting, using a
versioned, per-field TLV (tag, length, value) encoding declared through a
field schema. Unlike a hand-written bytes converter, individual fields are
validated separately, and the encoding tolerates schema evolution: a field
added to the struct later does not invalidate previously stored data (it is
just absent from old records, so the caller's own default for that field
applies), and a field removed from the schema is silently skipped when
decoding old data. Requires
[`CONFIG_ZMK_CUSTOM_SETTINGS_RECORD=y`](#feature-selection) (plus
`LARGE_VALUES` if the encoded record exceeds
`CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`):

```conf
CONFIG_ZMK_CUSTOM_SETTINGS_RECORD=y
```

```c
struct my_profile {
    int32_t speed;
    bool invert;
    char label[12];
};

ZMK_CUSTOM_SETTING_RECORD_RANGE_INT32_DEFINE(my_profile_speed_range, 0, 100);

ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE(
    my_profile_schema, struct my_profile,
    ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32(struct my_profile, speed, /* field id */ 1,
                                          &my_profile_speed_range),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_BOOL(struct my_profile, invert, /* field id */ 2),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_STRING(struct my_profile, label, /* field id */ 3));

ZMK_CUSTOM_SETTING_DEFINE(my_profile_setting, "my_module", "profile",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

struct my_profile profile = {.speed = 50, .invert = false, .label = "default"};
zmk_custom_setting_record_get(my_profile_setting, &my_profile_schema, &profile);

profile.speed = 80;
int ret = zmk_custom_setting_record_set(my_profile_setting, &my_profile_schema, &profile,
                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
/* ret == -ERANGE if a field like speed violates its constraint; nothing is
 * written in that case. */
```

Field ids are the stable identifiers for schema evolution, so keep them
fixed once a field ships (like setting keys). `_id` values are single bytes
(0-255) and only need to be unique within one schema.

Field constraints are limited to none and a range on `INT32` fields
(`zmk_custom_setting_record_set` returns `-ENOTSUP` for other constraint
kinds on a field). There is no dedicated Studio RPC message for records yet:
a record setting's RPC representation is its encoded bytes, so a Studio
client sees an opaque blob rather than a generic per-field edit form.

### Behavior Settings

`ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR` stores a ZMK behavior binding: a
behavior local ID (`CONFIG_ZMK_BEHAVIOR_LOCAL_IDS`) plus its two binding
parameters, e.g. for reassigning a key at runtime.

```c
ZMK_CUSTOM_SETTING_DEFINE(my_key_setting, "my_module", "key_binding",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
                          ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(0, 0, 0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

struct zmk_custom_setting_behavior_value binding;
zmk_custom_setting_get_behavior(my_key_setting, &binding);
zmk_custom_setting_set_behavior(my_key_setting,
                                (struct zmk_custom_setting_behavior_value){
                                    .behavior_id = zmk_behavior_get_local_id("key_press"),
                                    .param1 = A,
                                    .param2 = 0,
                                },
                                ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
```

Every write is validated against the target behavior's own ZMK parameter
metadata (`zmk_behavior_validate_binding`, `CONFIG_ZMK_BEHAVIOR_METADATA`):
an unknown `behavior_id`, or a `param1`/`param2` combination the behavior
itself rejects (e.g. an out-of-range HID usage for `&kp`), fails with
`-EINVAL` instead of being stored. This requires
`CONFIG_ZMK_BEHAVIOR_LOCAL_IDS`; without it, writes fail with `-ENOTSUP`.

Internally (both in the temporary-override pool and on flash), a behavior
value is stored the same way ZMK's own keymap settings storage persists
behavior bindings: a packed `{behavior_id: uint16, param1: uint32, param2:
uint32}` struct with trailing all-zero params truncated before writing (10
bytes down to as little as 2 when both params are zero), since most
bindings only use one or zero params. There is no dedicated Studio RPC
message for the encoded bytes: `SettingBehaviorValue` in the Studio RPC
protocol carries `behavior_id`/`param1`/`param2` as plain `uint32` fields,
and the compact encoding is purely an internal storage detail.

### Studio RPC Access

For RPC access, the setting namespace should match a Studio custom subsystem
identifier registered by the module that owns the setting. The web UI obtains
that subsystem's index from ZMK Studio and sends the index in setting requests.
The custom Studio subsystem identifier for this module is
`cormoran_custom_settings`.

#### Requesting Default Values

`GetSettingRequest` and `ListSettingsRequest` each accept a `require_default`
flag (alongside the existing `require_meta`). When set, the returned `Setting`
includes a `default_value` field carrying the setting's default — its runtime
override if one was installed, otherwise its compile-time default. To keep the
common case cheap, `default_value` is populated **only when the current value
differs from the default**, so a client can drive a "reset to default"
affordance without a second round trip and without paying for a redundant copy
when nothing has changed. It is never present for array or keyspace settings
(which have no compile-time default). Over the split relay, a `BYTES`/`STRING`
default is subject to the same envelope-size limit as the value itself and may
be omitted if the notification would not otherwise fit. The web UI exposes this
as an "Include Default" toggle next to "Include Metadata".

#### Factory Reset

This module hooks into ZMK's official factory reset. When a ZMK Studio client
sends the core `reset_settings` request (Studio's "Reset Settings" / restore
defaults action), ZMK invokes every registered subsystem reset handler,
including this module's — so a factory reset clears **all** custom settings
across every namespace, exactly like the built-in keymap reset. A fixed setting
reverts to its compile-time default; a runtime-created keyspace entry (which has
no compile-time default) is removed entirely, freeing its slot and its pool
region — so a pool-backed namespace such as
[zmk-feature-runtime-macro](https://github.com/cormoran/zmk-feature-runtime-macro)'s
is fully wiped, not left occupying memory until the next reboot. On a split
keyboard the central additionally relays the reset to every peripheral so their
custom settings are wiped too. No configuration is required; the behavior is
always available whenever the Studio RPC is enabled.

This is separate from the module's own scoped `ResetSettingsRequest`, which lets
a client reset a chosen subsystem/key subset; the official reset always covers
everything.

### Large Values (Per-Setting max_size)

By default every setting value is capped at
`CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` (64 bytes, the size of one RPC
frame). A `BYTES`/`STRING` setting that needs to store more than that registers
with an explicit per-setting `max_size` using `ZMK_CUSTOM_SETTING_DEFINE_SIZED`.
Requires [`CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES=y`](#feature-selection):

```conf
CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES=y
```

```c
/* Store up to 256 bytes for this one setting. */
ZMK_CUSTOM_SETTING_DEFINE_SIZED(
    my_blob_setting,
    /* max_size = */ 256,
    "my_module", "blob",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    ZMK_CUSTOM_SETTING_VALUE_BYTES(0),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
```

`ZMK_CUSTOM_SETTING_DEFINE_SIZED` is sugar for a private, single-member
[large-value pool](#shared-large-value-pools) sized just for this one setting
— the pool is this module's only large-value backing store, so "sized" and
"pooled" are the same mechanism at different sharing granularities. Making one
setting large this way does **not** grow the RAM footprint of every other
setting, and (unlike a plain static buffer) the setting's region is only
allocated once it actually holds a non-empty value — see
[Shared Large-Value Pools](#shared-large-value-pools) for what that means in
practice. `max_size` must not exceed
`CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE` (default equal to
`CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`), so raise that config to enable
large values:

```conf
CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE=256
```

Which knob is authoritative:

- `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` is the single-frame value size and
  the size of the stack-allocated API exchange carrier
  `struct zmk_custom_setting_value`. It stays pinned to 64 while the protobuf
  schema is enabled and is the default per-setting `max_size`. It does not
  size any per-setting storage (see [Memory Notes](#memory-notes)).
- `CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE` is the ceiling for any
  per-setting `max_size` and for the shared chunk/record staging buffers.

A value larger than `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` is reachable
through:

- the firmware API `zmk_custom_setting_write_bytes` / `zmk_custom_setting_read_into`
  (the fixed-carrier `zmk_custom_setting_read`/`write` return `-EMSGSIZE` for an
  oversized value rather than truncating it);
- **reading** it: the plain `GetSetting`/`ListSettings` RPC response streams a
  value of any size — the ZMK Studio RPC transport's send path streams a
  response incrementally regardless of size, so there is no read-side chunking
  RPC (there used to be a `ReadValueChunk`; it is gone);
- **writing** it: the chunked `WriteValueChunk` RPC (see below) — the receive
  path buffers a whole request frame *before* decoding it, so a value bigger
  than one frame genuinely cannot arrive any other way; the web UI does this
  automatically once a `bytes`/`string` edit exceeds one frame.

`ZMK_CUSTOM_SETTING_DEFINE_SIZED` also works for record settings — size the
underlying `BYTES` setting so the encoded record fits — and keyspaces accept a
larger `max_size` too (a keyspace with a large `max_size` draws every entry's
region from its own per-keyspace pool the same way — see
[RPC-Creatable Keyspaces](#rpc-creatable-keyspaces)).

> **Large values are effectively local-only on split keyboards.** A value
> larger than `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` is fully readable and
> writable only on the half that owns the setting, via the firmware API or the
> RPCs above. The split relay envelope
> (`CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN`) is a small fixed buffer, unlike the
> direct RPC path: a peripheral setting-changed notification includes a value
> only if it fits that envelope (which may be smaller or a little larger than
> `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` depending on configuration) and
> otherwise omits it cleanly (`has_value` unset, no crash); there is no
> chunked-write-over-relay path, so a central cannot write a peripheral's large
> value at all. Register large settings on the half whose RPC client (Studio)
> talks to them.

### Shared Large-Value Pools

Every large value in this module — a `ZMK_CUSTOM_SETTING_DEFINE_SIZED`
setting, a `ZMK_CUSTOM_SETTING_DEFINE_POOLED` setting, and a large keyspace
entry alike — is backed by the same mechanism: a region carved on demand from
a `struct zmk_custom_setting_large_pool`. `DEFINE_SIZED` is just sugar for a
private pool sized for exactly one setting, so it costs one small pool
descriptor more than a bare buffer would, but a setting holding an empty value
still costs zero pool bytes (see below) instead of always reserving
`max_size` bytes. `ZMK_CUSTOM_SETTING_DEFINE_POOLED` lets a group of
`BYTES`/`STRING` settings share one explicit, fixed-size budget instead of
each getting its own private pool — useful when a module has *many* large
settings that are not all expected to hold their maximum value at the same
time (e.g. one `BYTES` setting per user-defined macro body), since reserving
`max_size` bytes for each would cost `N * max_size` bytes of RAM even while
most of them are empty or short:

```c
/* One 4 KiB budget shared by every macro body. */
ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(macro_body_pool, 4096);

/* Each of these can hold up to 512 bytes, but only actually consumes pool
 * space while it holds a non-empty value. */
ZMK_CUSTOM_SETTING_DEFINE_POOLED(
    macro_body_0,
    /* max_size = */ 512, macro_body_pool,
    "runtime_macro", "macro/0/body",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    ZMK_CUSTOM_SETTING_VALUE_BYTES(0),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
```

Use `DEFINE_POOLED` when several large settings share a budget and are not
all expected to be full at once; use `DEFINE_SIZED` for one standalone large
setting that should always have its own guaranteed capacity.

A pooled setting with an empty value costs zero pool bytes (a `STRING`'s
trailing NUL still costs one byte even when empty, since it is stored inside
the pool region alongside the payload). Writing a value moves it into a
right-sized region inside the pool, relocating other members as needed to
keep the pool free of gaps — this is invisible to every reader, since a
setting's storage pointer is only ever dereferenced while holding the
settings lock, never cached across it. If the pool has no room for a write
even after that compaction, the write fails with `-ENOSPC` and the setting's
previous value is left untouched; `max_size` is still capped by
`CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE` per setting, but is **not**
checked against the pool's total size at compile time, since an
over-committed pool (the sum of every member's `max_size` exceeding the pool)
is the normal, expected way to use one — it only becomes a real problem once
enough members are simultaneously non-empty to exhaust it. A persisted value
that no longer fits its pool on boot (e.g. after shrinking the pool across a
firmware update) is skipped with a `LOG_WRN` instead of failing boot, the
same policy already used for keyspace-pool exhaustion.

`zmk_custom_setting_large_pool_used(&pool)` returns the pool's current total
usage, useful for a UI showing remaining budget.

### Reading And Writing Large Values

Reading a large value needs no special RPC: `GetSetting`/`ListSettings`
stream `Setting.value`'s bytes/string payload directly, regardless of size.
This works because the ZMK Studio RPC transport's send path streams a
response incrementally — `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE` is a
transmit-chunk size, not a maximum response length — so nothing but the old
fixed-size protobuf field was ever standing in the way. (Before this, a value
past `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` was omitted from the
response and had to be fetched with a now-removed `ReadValueChunk` RPC.)

Writing a large value still needs chunking: the receive path buffers a whole
request frame into `CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE` *before* nanopb decodes
it, so a value bigger than one frame genuinely cannot arrive in a single
`WriteSetting` request. `CONFIG_ZMK_CUSTOM_SETTINGS_CHUNKED_RPC` (enabled by
default alongside Studio RPC) adds the `WriteValueChunk` request for
bytes/string settings:

- Send `WriteValueChunkRequest{setting, total_size, offset, data, commit,
  mode}` starting at `offset = 0`. Each request carries one chunk (bounded by
  the protobuf schema, independent of
  `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`). Chunks for one transfer must
  arrive in order; the value is validated and applied only when a chunk sets
  `commit = true`, so a partially-sent value is never observable by readers.
  Only one write transfer is staged at a time
  (`CONFIG_ZMK_CUSTOM_SETTINGS_CHUNK_STAGING_SIZE`); starting a new transfer
  (`offset = 0`) for any setting replaces an abandoned one.

### Array Settings

Array settings are registered with a **single macro call** for the whole array:
one descriptor owns one contiguous backing buffer sized for the maximum
element count, instead of one registration per element. The active array
length can be smaller than the maximum. The firmware storage key is expanded
to `key/index` (plus a `key/_size` entry for the active length), but the
public API and RPC protocol use the base key plus an explicit array index and
active length. Flash storage saves the active length and only the active
array items. RPC does not expose the maximum length; appending past it
returns an error. Requires
[`CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY=y`](#feature-selection):

```conf
CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY=y
```

```c
/* Declare the per-index defaults first (a plain macro argument works for
 * short literals like ZMK_CUSTOM_SETTING_VALUE_INT32(0) elsewhere in this
 * guide, but an array of defaults needs its own declaration so it can be
 * passed as a pointer - see the comment on
 * ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE for why). */
ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(my_layers_default, 0, 1, 2);

/* One registration for up to 3 elements, with active length starting at 3. */
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(my_layers, "my_module", "layers",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                /* max_count = */ 3, /* default_size = */ 3,
                                my_layers_default,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_RANGE_INT32(0, 31));
```

`ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_CONSTRAINTS` and
`ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS[_AND_CONSTRAINTS]`
follow the same naming pattern as the scalar `ZMK_CUSTOM_SETTING_DEFINE*`
macros for zero-or-more constraints and/or custom bytes RPC converters.

> **Migrating from `ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE`**: older versions
> of this module registered arrays one element at a time
> (`ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(name, subsys, key, index,
> max_size, ...)` called once per index, each with its own default). That
> macro has been **removed**, not kept as a compatibility shim: independent
> per-index macro expansions (e.g. a `LISTIFY`/`UTIL_LISTIFY` loop) have no
> way to share the one contiguous buffer the new design requires, so there is
> no mechanical way to keep the old call sites compiling. Replace each array's
> N old registrations with one `ZMK_CUSTOM_SETTING_ARRAY_DEFINE` call, moving
> each element's old default into one `ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_*`
> declaration. The on-flash storage format is unchanged (`key/index` and
> `key/_size` entries persist and load exactly as before), so existing user
> data survives the migration with no extra code.

### RPC-Creatable Keyspaces

For entries whose *keys* are not known at compile time - user-created
profiles, named macros, and similar - define a **keyspace**: a fixed-size
pool of slots under a shared key prefix, with entries created and deleted at
runtime by RPC clients (or firmware) instead of by `ZMK_CUSTOM_SETTING_DEFINE`.
No heap is used: `ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE` statically allocates
`max_entries` slots plus one shared per-keyspace byte pool that every entry's
key + value are carved from on demand. Like `ZMK_CUSTOM_SETTING_DEFINE`, the
macro registers the keyspace at compile time (a linker section entry) - there
is no runtime registration call. Requires
[`CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=y`](#feature-selection), which
automatically selects `CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES` (a slot's
`[key][payload]` blob is always pool-backed):

```conf
CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=y
```

```c
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(
    my_macros, "my_module", /* key prefix = */ "macro/",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    /* max_size = */ sizeof(int32_t), /* max_key_len = */ 16,
    /* max_entries = */ 8,
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
```

`max_key_len` bounds a created entry's key length (including the NUL
terminator; it must not exceed `CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN`), and
`max_size` bounds each entry's value size. `max_entries` bounds how many
entries can exist at once. The keyspace's pool is sized
`max_entries * (max_key_len + max_size)` bytes and is shared across entries,
so a mix of long-key/short-value and short-key/large-value entries draws from
one budget instead of every slot reserving its own worst case.

Create and delete entries with `zmk_custom_setting_keyspace_create` /
`zmk_custom_setting_keyspace_delete`, or over Studio RPC with the
`CreateSetting` / `DeleteSetting` requests (`setting.key` must start with the
keyspace's prefix and fit `max_key_len`, including the NUL terminator):

```c
const struct zmk_custom_setting *created;
int ret = zmk_custom_setting_keyspace_create(
    &my_macros, "macro/my-macro-1", &ZMK_CUSTOM_SETTING_VALUE_INT32(1),
    ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, &created);
/* -ENOSPC if every slot is in use (or the pool cannot fit the new
 * key + value), -EEXIST if the key already has a live slot,
 * -ENAMETOOLONG if the key does not fit, -EINVAL for a mismatched
 * prefix or value type. */

zmk_custom_setting_keyspace_delete(&my_macros, "macro/my-macro-1");
/* -ENOENT if the key has no live slot. */
```

For a `BYTES`/`STRING` keyspace, `max_size` can exceed
`CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` exactly like
[`ZMK_CUSTOM_SETTING_DEFINE_SIZED`](#large-values-per-setting-max_size). Large
entries read and write like any other
[large value](#large-values-per-setting-max_size) (streamed on read, chunked
on write).

Once created, an entry behaves like any other setting: it is find-able
(`zmk_custom_setting_find("my_module", "macro/my-macro-1")`), readable and
writable by its key with the ordinary firmware API and `GetSetting`/
`WriteSetting` RPC, discoverable via the existing `key_prefix` scope filter
(`ListSettingsRequest{scope:{key_prefix:"macro/"}}` returns every live
entry), and covered by save/discard/reset scope operations.

Persisted entries survive a reboot with **no module code running on their
behalf**: each entry persists under a stable per-slot storage record
(internally named `custom_settings/<subsystem>/<prefix>#<slot>`, with the
user-visible key stored inside the record's value), and while
`settings_load()` is loading those records each one is re-bound to its slot
before its value is applied. If a persisted entry no longer fits (e.g. after
lowering `max_entries` or shrinking the pool across a firmware update), it is
skipped with a warning rather than failing boot.

> **Upgrade note (storage format change):** keyspace entries created by
> firmware built before this version persisted under their literal user key
> with a bare value. This version persists them under the per-slot record
> name above with the key embedded in the value, and does not migrate old
> records - previously created entries are dropped (ignored on load) after
> upgrading. Compile-time settings, arrays, and pooled/large values are
> unaffected. Recreate keyspace entries once after upgrading.

> **Removed in this version:** the general runtime-registration API
> (`zmk_custom_settings_register()` / `zmk_custom_settings_unregister()`)
> existed only to support the previous keyspace implementation and has been
> deleted. Settings are registered at compile time via the
> `ZMK_CUSTOM_SETTING_DEFINE`/`ZMK_CUSTOM_SETTING_ARRAY_DEFINE` macros;
> runtime-created *entries* are the keyspace feature above.

### Firmware API

Read or update settings from firmware:

```c
struct zmk_custom_setting_value value;
const struct zmk_custom_setting *setting =
    zmk_custom_setting_find("my_module", "speed");

zmk_custom_setting_read(setting, &value);
zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(20),
                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
zmk_custom_setting_save(setting);

zmk_custom_setting_write_array_by_key("my_module", "layers", 1,
                                      &ZMK_CUSTOM_SETTING_VALUE_INT32(4),
                                      ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

const struct zmk_custom_setting *layer_1 =
    zmk_custom_setting_find_array_element("my_module", "layers", 1);
zmk_custom_setting_write_array_element(layer_1, &ZMK_CUSTOM_SETTING_VALUE_INT32(4),
                                       2, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

const struct zmk_custom_setting *layers =
    zmk_custom_setting_find_array("my_module", "layers");
zmk_custom_setting_array_push_back(layers, &ZMK_CUSTOM_SETTING_VALUE_INT32(5),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
zmk_custom_setting_array_pop_back(layers, NULL, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

/* Insert/remove in the middle of the array. Both shift the remaining active
 * elements over the array's single contiguous buffer, so they are cheap
 * regardless of array length. */
zmk_custom_setting_array_insert_at(layers, 1, &ZMK_CUSTOM_SETTING_VALUE_INT32(9),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
struct zmk_custom_setting_value removed;
zmk_custom_setting_array_remove_at(layers, 1, &removed, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
```

`struct zmk_custom_setting_value` is sized by
`CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`, so declaring one on the stack
just to read or write a small value, or to write a byte buffer whose size
isn't known at compile time, is wasteful or impossible with the macros
above. A view-based API avoids both:

```c
/* Read into a right-sized buffer instead of a full zmk_custom_setting_value. */
int32_t speed;
zmk_custom_setting_get_int32(setting, &speed);
zmk_custom_setting_set_int32(setting, 20, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

/* zmk_custom_setting_get_bool/set_bool follow the same pattern. */

/* Write bytes/string settings from a runtime buffer of dynamic length. */
uint8_t payload[dynamic_len];
zmk_custom_setting_write_bytes(blob_setting, payload, dynamic_len,
                               ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

/* Copy any setting's raw payload into a caller-sized buffer; fails with
 * -EMSGSIZE instead of overflowing if the buffer is too small. */
uint8_t buf[8];
size_t size;
zmk_custom_setting_read_into(setting, buf, sizeof(buf), &size, NULL);

/* Borrow the effective value with no copy at all; the callback runs while
 * the settings lock is held, so keep it short and do not call back into
 * other zmk_custom_setting_* functions from within it. */
void log_speed(const struct zmk_custom_setting_value *value, void *user_data) {
    ARG_UNUSED(user_data);
    LOG_INF("speed=%d", value->int32_value);
}
zmk_custom_setting_with_value(setting, log_speed, NULL);
```

### Events

The module raises two ZMK events you can subscribe to from firmware.

`zmk_custom_settings_initialized` is raised **exactly once per boot**, right
after the settings subsystem has finished loading persisted values into every
registered setting. Reading a setting from a plain `SYS_INIT` is racy — it may
run before `settings_load()` has populated the persisted value, so you would
see the compile-time default instead. Subscribe to this event instead when you
need to apply a stored setting to hardware at startup:

```c
#include <zmk/event_manager.h>
#include <cormoran/zmk/custom_settings.h>

static int on_settings_ready(const zmk_event_t *eh) {
    if (as_zmk_custom_settings_initialized(eh) != NULL) {
        int32_t speed;
        zmk_custom_setting_get_int32(
            zmk_custom_setting_find("my_module", "speed"), &speed);
        /* ...apply `speed` to hardware; the value is now the persisted one. */
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(my_module, on_settings_ready);
ZMK_SUBSCRIPTION(my_module, zmk_custom_settings_initialized);
```

The later, targeted reloads that `zmk_custom_setting_discard` performs do not
re-raise it, and in a build without `CONFIG_SETTINGS` (nothing ever calls
`settings_load()`) it never fires.

`zmk_custom_setting_changed` is raised whenever a single setting is updated,
saved, discarded, or reset (see `enum zmk_custom_setting_changed_kind`); its
`setting`/`kind`/`source` fields identify what changed.

### Memory Notes

A setting's registration descriptor (identity, type, permissions,
constraints, default pointer) is `const` and lives in flash; the only RAM a
setting costs is a compact per-setting state block (~24 bytes on ARM32:
status flags, temporary-override slot index, the runtime default override
pointer, pool bookkeeping, and - for `INT32`/`BOOL`/`BEHAVIOR` - the value
itself, stored inline) plus the exact storage for its value:

- `INT32`/`BOOL`/`BEHAVIOR`: no extra storage at all - the value fits the
  state block's inline union. A bool setting costs ~24 bytes of RAM total.
- `BYTES`/`STRING` via plain `ZMK_CUSTOM_SETTING_DEFINE`: one exact-size
  static buffer of `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1` bytes.
- `BYTES`/`STRING` via `ZMK_CUSTOM_SETTING_DEFINE_SIZED`/`_POOLED` (and
  keyspace entries): a region carved on demand from the declared
  [pool](#shared-large-value-pools) - zero bytes while empty.

The default value lives in flash and is referenced by pointer, and the
persisted value is not cached separately:

- `ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY` overrides come from a small
  shared pool instead of a dedicated slot per setting
  (`CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS`, default 2;
  `CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE`, default 32 bytes). Writing a
  value larger than the slot size in temporary mode fails with `-EMSGSIZE`;
  writing while the pool is full fails with `-EBUSY`. Other write modes are
  unaffected by either limit.
- `zmk_custom_setting_discard` re-reads the previously saved value from flash
  instead of restoring a RAM-cached copy. This is a rare, explicit user
  action, so the flash read is not a concern; `zmk_custom_setting_has_unsaved_value`
  stays a cheap flag check (it does not read flash) and reports "written
  since the last save/discard/reset", which also means writing back the
  already-saved value still reports unsaved until the next save/discard.
- Persisting (`ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST` or
  `zmk_custom_setting_save`) a value equal to the setting's default **deletes**
  the stored record instead of writing a redundant copy. This keeps flash
  clean and, more usefully, lets a setting that was saved at its old default
  transparently follow a later change to that default: with no record present
  the setting falls back to the current default on the next boot. Keyspace
  entries are exempt (they carry a user key and have no compile-time default).

## Web UI

The web UI in `web/` connects to a keyboard over serial, finds the
`cormoran_custom_settings` subsystem, and sends typed read/write/save/discard/reset
requests. It can also export all RPC-readable setting values as JSON and import
that JSON back to the device using the selected write mode. Array values can be
written by index or changed with push-back/pop-back commands. A "Create
Setting" panel sends `CreateSetting` for a registered keyspace's prefix (see
RPC-Creatable Keyspaces above), and any selected setting - including a
keyspace entry - can be removed with the "Delete" button, which sends
`DeleteSetting`.

Listed settings are grouped into one table per subsystem (the subsystem
identifier is the table heading), so the **Setting** column shows only the key
without its subsystem prefix. Each table also shows the setting's static
metadata - **Confidentiality**, **Read** and **Write** permission, and a summary
of its **Constraints** - alongside the per-source **Value** and **Unsaved**
columns. These metadata columns are populated when settings are loaded with
**Include Metadata** checked (on by default); they show `—` otherwise.

### Constraint-aware editing

When a setting is listed or read with **Include Metadata** checked, the firmware
advertises the setting's constraints over RPC and the value editor renders a
matching control instead of a plain text box:

- **Options** - a dropdown of the advertised choices, showing each human label
  and submitting the underlying value. If the list reaches the RPC cap of 8
  entries a "may be truncated" hint is shown (the transport truncates longer
  lists; this is a firmware/proto limit, not fixed here).
- **Range** - a bounded number input plus a slider using the advertised
  min/max.
- **HID usage** - a number input bounded by the advertised usage-page/usage
  range.
- **Layer id** / **Behavior id** - a constrained input. No layer or behavior
  list is available over RPC, so these degrade to a validated number input
  (layer index) or a `behaviorId,param1,param2` field respectively.
- **No constraint** - the existing typed free-text input.

The chosen value is validated on the client against the advertised constraint,
and an inline message explains any violation while disabling Write, so obvious
mistakes are caught without a firmware round-trip.

```bash
cd web
npm install
npm run dev
```

See [web/README.md](./web/README.md) for web development commands.

## Development

The `tests/zmk-config` build enables `CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES`
for module-enabled artifacts. Flash `custom_settings_board_with_rpc` to a real
device to test Studio RPC with the sample custom subsystem `zmk_config_sample`.
For split testing, flash `custom_settings_split_central_with_rpc_relay` to the
central half and `custom_settings_split_peripheral_with_rpc_relay` to the
peripheral half. The sample subsystem registers int32, bool, string, bytes,
bytes-with-RPC-conversion, array, private, secure, HID usage, layer id, and
behavior id settings where supported.

```bash
# Run unit test + build test and verify the results
python3 -m unittest

# Run build test directly
west zmk-build tests/zmk-config

# Run unit test directly
west zmk-test tests -m .

# Run web tests
cd web && npm test

# Run lint/test hooks before commit
pre-commit run --all-files
```

### Hardware-free split-relay test (Renode)

`tests/renode/custom_settings_usb_wired_split_relay_renode_test.py` exercises the
whole Studio RPC surface — including reading and **writing a split peripheral's
settings across the wired split relay** — with no physical hardware, using the
[Renode](https://renode.io/) emulator via `west zmk-renode-test` (provided by
`zmk-west-commands`).

It boots two real firmware images on one Renode session: a wired-split **central**
that answers ZMK Studio over the emulated USB CDC, and a relay-enabled
**peripheral**, cross-connected by a Renode UART hub. A Studio host then drives
the central over USB — local get/write/list — and relays a write and a scoped
list to the peripheral, asserting the peripheral's value-updated and list-item
notifications round-trip back through the relay.

```bash
# Build both split halves (a WIRED split whose central still speaks Studio over USB).
west zmk-build tests/zmk-config --build-yaml tests/zmk-config/build-usb-split.yaml -af usb-wired-central
west zmk-build tests/zmk-config --build-yaml tests/zmk-config/build-usb-split.yaml -af usb-wired-peripheral

# Boot both halves, run the generic usb+wired smoke, then the relay round-trip.
west zmk-renode-test --mode wired-split \
  --elf build/usb-wired-central/zephyr/zmk.elf \
  --peripheral-elf build/usb-wired-peripheral/zephyr/zmk.elf \
  tests/renode
```

CI runs this automatically in the `Renode custom-settings wired-split relay test`
job, via `zmk-west-commands`' [`zmk-renode-test` composite
action](https://github.com/cormoran/zmk-west-commands/tree/main/.github/actions/zmk-renode-test)
(which installs Renode + the protobuf/protoc deps for you).
