# Feature gating & modularization: compile out what you don't use

Status: design (2026-07-08). Successor to `simplification-redesign.md` (that
round landed as PR #31: pools, streamed reads, blob keyspaces, flash
descriptors). This round has two goals, decided with the owner:

1. **Simpler / easier to read** — split the 3589-line `src/custom_settings.c`
   monolith into one core file plus one file per optional feature, behind a
   single shared internal header.
2. **Smaller firmware** — add Kconfig gates so a keyboard compiles in *only*
   the features it uses. New per-feature options **default `n` (opt-in)**.

The two goals reinforce each other: once each optional feature lives in its own
translation unit, gating it out is a one-line CMake `..._ifdef` plus a small
number of `IS_ENABLED()` guards at the handful of core call sites — no
`#ifdef` soup inside the feature bodies.

## 1. Why this is achievable cleanly

The ARM target builds with `-ffunction-sections` + `-Wl,--gc-sections`
(confirmed: `dependencies/zephyr/cmake/compiler/gcc/target_arm.cmake`,
`.../linker/ld/linker_flags.cmake`). So a `static` function reachable only
through a provably-dead branch — `if (IS_ENABLED(CONFIG_X) && ...)` where
`CONFIG_X` is unset, i.e. `if (0 && ...)` — is dropped from the final image by
the linker. The firmware shrinks whether or not the code physically moves to
another file. Moving it to a `..._ifdef`-gated file is what makes the *source*
smaller and keeps the disabled path from compiling at all.

## 2. The five gate-able features (owner's list)

| Kconfig (all `bool`, `default n`) | Covers | Depends on |
|---|---|---|
| `ZMK_CUSTOM_SETTINGS_LARGE_VALUES` | large per-setting values + the shared pool (`pool_*`, `blob.pool`, `write_large_locked`, `DEFINE_SIZED`/`DEFINE_POOLED`, `LARGE_POOL_DEFINE`, chunked-write RPC, `with_large_raw_bytes`, `large_pool_used`) | — |
| `ZMK_CUSTOM_SETTINGS_ARRAY` | array settings (`ARRAY_DEFINE`, `array_*`, `array_view_pool`, find/read/write/push/pop/insert/remove-array, array RPC) | — |
| `ZMK_CUSTOM_SETTINGS_KEYSPACE` | RPC-creatable keyspaces / namespaces (`KEYSPACE_DEFINE`, `keyspace_*`, `CreateSetting`/`DeleteSetting` RPC) | **select `LARGE_VALUES`** (slot blobs are pool-backed) |
| `ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS` | bytes RPC serializer/deserializer hooks (`convert_rpc_bytes_value`, `serialize`/`deserialize_rpc_value`, the two descriptor fn-ptr fields) | — |
| `ZMK_CUSTOM_SETTINGS_RECORD` | struct/record TLV codec (`custom_settings_record.c`, already self-contained) | large records only: `LARGE_VALUES` |

Hard couplings verified in the entanglement audit (do not violate):

- **KEYSPACE → LARGE_VALUES**: a keyspace slot's whole entry is one opaque
  pool-backed BYTES blob (`keyspace->large_pool`, `blob.pool`,
  `pool_release_locked`). KEYSPACE cannot link without POOL. Enforced by
  `select`.
- **RPC_CONVERTERS is source-coupled to KEYSPACE**: the three converter
  functions read `setting->_keyspace` to choose per-keyspace vs per-setting
  converters. Those keyspace branches inside the converter file get their own
  `IS_ENABLED(CONFIG_...KEYSPACE)` guard; the converter feature is otherwise
  independent of the core lifecycle (no core function calls into it — only the
  Studio handler does).
- **RECORD** only calls core `zmk_custom_setting_write_bytes` /
  `zmk_custom_setting_read_into` (which live in core and handle the small path
  themselves). A record larger than the fixed carrier additionally needs
  `LARGE_VALUES`; document it, don't hard-select it (small structs are the
  common case).
- `write_bytes_raw()` is a **three-way seam** (large → pool, small → scalar
  path, keyspace → `keyspace_write_raw_payload_with_key`). It stays in **core**;
  only its `>carrier → write_large_locked` arm is `IS_ENABLED`/pool-gated.

## 3. Module decomposition

New shared internal header `src/custom_settings_internal.h` (NOT public — under
`src/`, not `include/`). It exposes the core internals a feature file needs:
the `settings_lock` mutex, the temp-slot pool + its helpers, `effective_value`,
`memory_value_locked`, `clear_temporary_locked`, `store_scalar_value_locked`,
`apply_scalar_default_locked`, `value_to_storage`, `setting_storage_name`,
`write_bytes_raw`, `value_from_raw`, the state-flag helpers, and forward
declarations of each feature's entry points (so core call sites can be
`IS_ENABLED`-guarded without the feature file being compiled).

Translation units after the split:

| File | Gate | Contents |
|---|---|---|
| `src/custom_settings.c` | always (`ZMK_CUSTOM_SETTINGS`) | lifecycle: find/read/write/save/discard/reset, `effective_value`/`memory_value_locked`, temp slots, scalar + **fixed** (non-pool) BYTES/STRING store, init, the `settings_load` handler skeleton, `apply_scope` skeleton, the `write_bytes_raw` seam, `value_from_raw`/`value_to_storage`/`value_from_storage` |
| `src/custom_settings_pool.c` | `LARGE_VALUES` | `pool_*`, the pool arm of the blob store, `write_large_locked`, `with_large_raw_bytes`, `large_pool_used` |
| `src/custom_settings_array.c` | `ARRAY` | everything under Feature 1 in the audit (`array_*`, `array_view_pool`, array storage-name split/load) |
| `src/custom_settings_keyspace.c` | `KEYSPACE` | everything under Feature 2 (`keyspace_*`, the two scratch buffers, ordinal bind/load) |
| `src/custom_settings_rpc_convert.c` | `RPC_CONVERTERS` | `convert_rpc_bytes_value`, `serialize`/`deserialize_rpc_value` |
| `src/custom_settings_record.c` | `RECORD` | unchanged; just add the `..._ifdef` |
| `src/studio/custom_settings_handler.c` | `PROTOBUF` (existing) | feature-specific request handlers guarded with `IS_ENABLED` (array push/pop, keyspace create/delete, chunk write); the big per-feature handler blocks may move to `custom_settings_handler_<feature>.c` where it reads cleanly, otherwise stay guarded in place |

Descriptor-struct policy: **keep all descriptor fields** (`array_*`, `_keyspace`,
`blob.pool`, `rpc_serializer/deserializer`) present unconditionally. They cost a
few bytes of *flash* per compile-time setting (const rodata), which is
negligible next to the KB of code gc-sections drops, and keeping them means the
`*_DEFINE` macros need **no** conditional field initializers. The small
predicates become `static inline` in the internal header, keyed on `IS_ENABLED`
so their branches fold away:

```c
static inline bool cs_is_array(const struct zmk_custom_setting *s) {
    return IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY) && s->array_key != NULL;
}
static inline const struct zmk_custom_setting_keyspace *
cs_keyspace_of(const struct zmk_custom_setting *s) {
    return IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE) ? s->_keyspace : NULL;
}
```

When a feature is off these return a compile-time constant, so every dependent
branch (and the feature-function call inside it) is dead-code-eliminated and its
`static` helpers are gc-dropped.

Public-header macros for a disabled feature (`ARRAY_DEFINE`, `KEYSPACE_DEFINE`,
`DEFINE_SIZED`/`DEFINE_POOLED`, `RECORD_*`) still expand, but reference
functions that aren't compiled → a **link error** if a consumer uses a disabled
feature. Add a `#if !IS_ENABLED(...)` `#error`-style `BUILD_ASSERT` guard inside
each such macro so the failure is a clear "enable CONFIG_… to use this" message
at the definition site instead of a bare undefined-reference at link.

## 4. Phased delegation plan (sequential stacked branches, Sonnet per phase)

Ordering respects KEYSPACE→POOL and avoids any broken intermediate build by
keeping every new gate **`default y` until the final flip**. Each phase leaves
the whole tree green (`pre-commit run --all-files`, `python3 -m unittest`,
`west zmk-test tests -m .`, `west zmk-build tests/zmk-config`, web tests) and
opens a PR; the orchestrator reviews + hardware-validates between phases.

- **P1 — POOL/LARGE extraction + gate.** Create `custom_settings_internal.h`;
  move the pool/large functions to `custom_settings_pool.c`; add
  `CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES` (`default y` for now);
  `IS_ENABLED`-guard the core large seams (`memory_value_locked` >carrier NULL,
  `write_bytes_raw` large arm, `value_from_storage` large load, chunked-RPC
  handler is already `#if`-gated — tie it to the new symbol). Verify: with
  `LARGE_VALUES=y` snapshots/size identical to baseline.
- **P2 — ARRAY extraction + gate** (`default y`). Move Feature 1 to
  `custom_settings_array.c`; guard core call sites + array RPC handlers.
- **P3 — KEYSPACE extraction + gate** (`default y`, `select LARGE_VALUES`). Move
  Feature 2 to `custom_settings_keyspace.c`; guard core call sites + keyspace
  RPC handlers.
- **P4 — RPC_CONVERTERS + RECORD gates.** Move converters to
  `custom_settings_rpc_convert.c`; `..._ifdef` `record.c`; guard the handler's
  converter/record use. Both `default y`.
- **P5 — flip to opt-in + minimize + validate.** Flip all five gates to
  `default n`; wire `select`/`depends`; add macro `BUILD_ASSERT` guards; update
  `tests/zmk-config` + every self-test build to explicitly enable the features
  it exercises (self-tests keep exercising everything, so snapshots are
  unchanged); add build.yaml targets `custom_settings_board_minimal` (module on,
  all five off) and confirm it builds + measurably shrinks vs
  `custom_settings_board_full`; README "Feature Selection" section; **hardware
  validation** of the minimal build and a full build on the XIAO rig
  (single-board USB RPC + split relay per the established procedure).
- **Cross-repo follow-up (separate, after P5 merges):** zmk-feature-runtime-macro
  uses KEYSPACE (+POOL via select); add `select ZMK_CUSTOM_SETTINGS_KEYSPACE`
  (or explicit config) to its Kconfig so its consumers keep building, and repin
  its west dep.

## 5. Testing (must include hardware)

- **Unit**: keep the three self-test variants (`tests/{test,studio,split_peripheral}`)
  building with all features enabled (snapshots stay exact-match). Add a new
  build-only target / test that compiles with every feature **off** and asserts
  the core scalar path still works and the feature symbols are absent
  (`strings`/nm on the ELF, or a compile that would fail if a disabled feature's
  code were reachable).
- **Build**: `custom_settings_board_minimal` (all off) proves the gates link and
  measure its flash/RAM delta vs full in the P5 PR body.
- **Hardware** (P5, on the two-J-Link XIAO rig): flash `custom_settings_board_minimal`
  → confirm boot + core scalar get/set/persist over USB RPC; flash a full build
  → re-confirm the existing large-value + keyspace + array + split-relay
  behaviors still pass (regression). Record results in the P5 PR comments, as in
  the simplification round's §16.

## 6. Non-goals this round

- No change to wire format, storage schema, or the public value/descriptor
  types beyond adding the macro guards.
- No new features. TEMPORARY mode, BEHAVIOR values, constraints stay core.
- The descriptor fields are not gated out of the struct (see §3 rationale).

## P5 (as landed)

All five gates now `default n` (Kconfig: dropping the `default y` line is
enough - a `bool` with no default is `n`). `ZMK_CUSTOM_SETTINGS_KEYSPACE`'s
`select ZMK_CUSTOM_SETTINGS_LARGE_VALUES` is unchanged.

**Macro `BUILD_ASSERT` guards** were added to the innermost registration
macro each public `*_DEFINE*` variant funnels through, so registering a
setting whose feature is off fails to compile with a clear "enable
CONFIG_..." message instead of an undefined-reference at link time:

- `ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE` and
  `ZMK_CUSTOM_SETTING_DEFINE_POOLED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS`
  (which `DEFINE_SIZED`/`DEFINE_POOLED` both funnel through) →
  `CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES`.
- `ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS` →
  `CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY`.
- `ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE_RPC_CONVERTERS_AND_CONSTRAINTS`
  → `CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE`.
- `ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE` → `CONFIG_ZMK_CUSTOM_SETTINGS_RECORD`.
- `ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS` and
  `ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS` (the wrapper macros
  that only a caller passing explicit converters reaches - the plain,
  no-converter path bypasses them entirely and calls the shared
  `..._AND_CONSTRAINTS` innermost directly with `NULL, NULL`) →
  `CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS`.
  **Keyspace converters are the one exception**: unlike `DEFINE`/`ARRAY_DEFINE`,
  `ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS` has
  no separate plain-path bypass - the no-converter
  `KEYSPACE_DEFINE_WITH_CONSTRAINTS` funnels through the very same macro,
  passing `NULL, NULL`. Asserting there would misfire on every plain keyspace
  registration with `RPC_CONVERTERS` off. Per §3's escape hatch this assert
  is skipped for keyspaces (documented at the macro); a keyspace registered
  with real converter functions while the gate is off silently falls back to
  identity passthrough (`zmk_custom_setting_serialize/deserialize_rpc_value`'s
  existing `IS_ENABLED` branch in `custom_settings.c`) rather than failing to
  build. Documented as a `CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS`
  prerequisite in the README instead.

**Self-tests** (`tests/{test,studio,split_peripheral}/native_sim.conf`) now
explicitly set all five gates to `y` - `src/test/custom_settings_test.c`
exercises every feature unconditionally and was left untouched, so the three
snapshot files stay byte-identical (confirmed: empty `git diff` on all three
`keycode_events.snapshot` files).

**Sample settings** (`src/test/zmk_config_sample_settings.c`, used by the
`tests/zmk-config` hardware targets) are now `#if IS_ENABLED(CONFIG_...)`
guarded per feature, so the file - and any target that includes it - builds
in any gate combination: the large-bytes/pooled-bytes samples under
`LARGE_VALUES`, the array sample under `ARRAY`, the keyspace sample under
`KEYSPACE`, and the bytes-RPC-converter sample (plus its converter function)
under `RPC_CONVERTERS`. The scalar samples (int32/bool/string/bytes/
hid_usage/layer_id/secure_int32/device_private_string, plus behavior samples
under their own independent `CONFIG_ZMK_BEHAVIOR_LOCAL_IDS` gate) are
unconditional, so even the minimal build has something to read/write.

**`tests/zmk-config/build.yaml`**: `custom_settings_board_with_rpc`,
`custom_settings_board_without_rpc`, and the two split-relay targets (via the
`custom-settings-split-rpc-relay` snippet's `.conf`, since that is how they
already pulled in `CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES`) now
explicitly enable all five gates, so they keep exercising every feature. A
new `custom_settings_board_minimal` target enables only the module, Studio
RPC, and the samples - all five feature gates left at their opt-in default
(`n`) - to prove the opt-out path builds and to measure its size.

**Measured size** (`arm-zephyr-eabi-size` on the `tests/zmk-config` ELF,
xiao_ble/zmk + tester_xiao shield, both images carrying the same sample
settings file):

| Artifact                        | text   | data  | bss   | FLASH (text+data) | RAM (data+bss) |
| -------------------------------- | ------ | ----- | ----- | ------------------ | --------------- |
| `custom_settings_board_minimal`  | 212672 | 30785 | 75526 | 243,457            | 106,311         |
| `custom_settings_board_with_rpc` | 220072 | 32533 | 79222 | 252,605            | 111,755         |
| **Delta**                        |        |       |       | **9,148 B smaller (~8.9 KiB, 3.6%)** | **5,444 B smaller (~5.3 KiB, 4.9%)** |

The delta is a floor, not a ceiling: `with_rpc` also raises
`CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE=256` (irrelevant when
`LARGE_VALUES` is off in `minimal`) and both images carry every sample
setting regardless of gate state where possible; a real keyboard that only
uses core settings would see a larger relative reduction.

**Cross-repo follow-up** (tracked separately, not part of this PR):
`zmk-feature-runtime-macro` registers a keyspace and needs
`select ZMK_CUSTOM_SETTINGS_KEYSPACE` (or an explicit
`CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=y` default) added to its own Kconfig
before it can build against this version, now that the gate defaults to `n`.

## P6 — defaults reverted to `y` (owner decision, post-hardware-validation)

After the measured minimal-vs-full delta above (~8.9 KiB flash / ~5.3 KiB RAM)
turned out smaller than expected, the owner decided the breaking-change cost
of opt-in defaults (every existing consumer, including
`zmk-feature-runtime-macro`, would need a Kconfig change to keep building) was
not worth it. All five gates are `default y` again - each still independently
toggleable to `n` to shrink firmware, but no consumer's build breaks by
default. This removes the P5 cross-repo follow-up above:
`zmk-feature-runtime-macro` needs no change.

Only two other files needed a matching change, since everything else in P1-P5
(the file split, the `IS_ENABLED`-foldable predicates, the `BUILD_ASSERT`
macro guards, the self-test configs, the sample file's per-feature `#if`
guards) is orthogonal to which way the default points:

- `Kconfig`: re-added `default y` to all five gates.
- `tests/zmk-config/build.yaml`: `custom_settings_board_minimal` now passes
  explicit `-DCONFIG_ZMK_CUSTOM_SETTINGS_*=n` for all five gates (previously
  relied on the opt-in default to stay minimal); the other targets' existing
  explicit `=y` flags are now redundant but harmless, left as-is.
