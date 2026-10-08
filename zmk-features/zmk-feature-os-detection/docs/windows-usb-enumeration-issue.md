# Windows USB enumeration failure — investigation plan

Status: **root cause confirmed and fixed, verified on real hardware
(Phases 0, 1, and most of 3 done); Phase 2's exact Windows error text and
the Phase 4 fallout follow-ups are still open.** Written 2026-07-05 after
the module owner reported that a board built with this module is no longer
recognized by Windows when connected over USB (macOS/Linux reportedly still
fine); updated the same day once a follow-up session executed most of this
plan.

## Resolution summary (2026-07-05, second session)

H1 was confirmed exactly as hypothesized: `CONFIG_ZMK_OS_DETECTION_USB`
selects `CONFIG_USB_DEVICE_BOS`, which bumps `bcdUSB` to `0x0201`, but
nothing in ZMK or this module ever called `usb_bos_register_cap()` /
`usb_bos_fix_total_length()`, so the device answered every BOS read with a
spec-invalid `wTotalLength=0` descriptor. Real Windows aborted enumeration
right after reading it (confirmed: this is exactly what the module's
original "verified Windows capture" in `docs/fingerprints.md` had recorded,
misread at the time as real Windows behavior rather than a failure).

**Fix** (`src/os_detection_usb.c`): register a standard USB 2.0 Extension
BOS capability (`bmAttributes=0`, no LPM claimed) via
`SYS_INIT(..., POST_KERNEL, 0)`, guaranteed to run before ZMK's own
`usb_enable()` (`APPLICATION`/`CONFIG_ZMK_USB_INIT_PRIORITY`).

**Verified directly on real hardware, no assumptions**:
- Raw `pyusb` BOS read from this workspace's sandbox host (itself a real
  Linux USB host one test board was already plugged into):
  `wTotalLength` `0` → `12`, `bNumDeviceCaps` `0` → `1`.
- The *other* test board was flashed with the fix and its USB-C was
  plugged into a real Windows PC (by the module owner, mid-session) while
  RTT/SWD stayed on this sandbox. The RTT capture shows Windows now
  reaching `SET_CONFIGURATION` and reading the HID `REPORT` descriptor —
  neither ever happened pre-fix — i.e. the keyboard should now actually
  function on Windows, not just get fingerprinted differently.
- A fresh Linux re-capture (same sandbox host) confirmed no regression:
  Linux now does a genuine two-step BOS read (5 bytes → 12 bytes) instead
  of its old one-shot 5-byte read, everything else unchanged.

**Fallout found and fixed** (exactly as Phase 4 below predicted before any
hardware was touched): a successfully-enumerating Windows now reads string
descriptors just like Linux does, so the pre-fix "Windows: BOS at more than
minimal length" check (`bos_wlength > 5`) started colliding with the Linux
rule. Fixed by tightening it to `bos_wlength == 255` (Windows's persistent
"read BOS in one blind max-length shot" behavior, vs. Linux's now-correct
two-step read) and moving the Windows check ahead of the Linux check in
`zmk_os_classify_usb()`. `inject_windows_like()`/`inject_linux_like()` and
`docs/fingerprints.md` were updated to match; full detail in
fingerprints.md's "Windows enumeration failure discovered and fixed"
section — read that instead of duplicating it here.

**Not done**: Phase 2's exact Windows Device Manager error text was never
collected (the owner moved a board's USB-C to a real Windows PC directly
mid-session rather than following the ask-first protocol below, and the
fix was verified via RTT before/after rather than via that error message) —
still worth getting once the owner can check Device Manager, mainly to
confirm the *original* code did produce the expected symptom. macOS/iOS
were not re-captured against the fix (see fingerprints.md's "Still
unverified" section) — their classification branch doesn't consult BOS
length so it isn't expected to be affected, but this is unconfirmed by real
data. Whether the keyboard now actually **types** on the user's Windows PC
also hasn't been explicitly confirmed by the user — the RTT evidence
(reaches `SET_CONFIGURATION` + reads the HID Report descriptor) is strong
circumstantial proof but not the same as an explicit "yes it types" from
the user.

## Symptom (as reported, needs confirmation in Phase 2)

With `CONFIG_ZMK_OS_DETECTION_USB=y` firmware, plugging the board's USB-C
into a Windows PC results in Windows not recognizing the device at all —
presumably "Unknown USB Device (Device Descriptor Request Failed)" in Device
Manager, but the exact error has not been recorded yet.

## Established facts (verified in this checkout, no hardware needed)

1. `CONFIG_ZMK_OS_DETECTION_USB` does `select USB_DEVICE_BOS`
   ([Kconfig:13](../Kconfig)).

2. `CONFIG_USB_DEVICE_BOS` changes the device descriptor's `bcdUSB` from
   `0x0200` to `0x0201`
   (`dependencies/zephyr/subsys/usb/device/usb_descriptor.c:62-65`,
   `USB_SRN_2_0_1`). `bcdUSB >= 0x0201` is precisely what tells a host "I
   support `GET_DESCRIPTOR(BOS)`" — it is why Windows requests BOS at all,
   i.e. the very signal `zmk_os_classify_usb()` relies on. A stock ZMK build
   (no module) reports `0x0200` and Windows never asks for BOS.

3. **Zephyr's legacy USB stack returns a malformed BOS descriptor unless a
   capability is explicitly registered.**
   `dependencies/zephyr/subsys/usb/device/bos.c` defines the header as
   `wTotalLength = 0 /* should be corrected with register */`,
   `bNumDeviceCaps = 0 /* should be set with register */`. The only functions
   that fix `wTotalLength` are `usb_bos_register_cap()` and
   `usb_bos_fix_total_length()`, and **neither is called anywhere in this
   tree** (grep `dependencies/zephyr/subsys/usb/` and
   `dependencies/zmk/app/src/` — zero call sites; upstream only the webusb
   sample calls `usb_bos_register_cap()`). So the device answers
   `GET_DESCRIPTOR(BOS)` with the 5 bytes `05 0F 00 00 00` —
   `wTotalLength = 0`, which is invalid (must be ≥ 5, the header's own size).
   Additionally, the USB 2.0 LPM ECN requires a device reporting
   `bcdUSB >= 0x0201` to include a USB 2.0 Extension capability in its BOS;
   an empty BOS violates that too.

4. **The "verified Windows capture" in [fingerprints.md](fingerprints.md)
   reads, in hindsight, like a capture of a *failing* enumeration**: the
   sequence `DEVICE(64) → DEVICE(18) → CONFIG(255) → BOS(255)` was "repeated
   identically 3 times in a row", never reached `SET_CONFIGURATION`, never
   read the HID report descriptor, and read no string descriptors — while
   the macOS, Linux, Android, and iOS captures all reached
   `SET_CONFIGURATION` (and HID report reads where captured). Windows
   retries enumeration exactly 3 times before giving up with "Device
   Descriptor Request Failed". So the most likely story is: **the module
   never actually worked as a keyboard on Windows; the classifier was
   validated against the failure trace** (which, ironically, classified
   correctly). The user "noticing it stopped working" may simply be the
   first time typing on Windows was attempted.

## Hypotheses, in priority order

- **H1 (prime suspect)**: malformed BOS (`wTotalLength=0`) combined with
  `bcdUSB=0x0201` makes Windows fail/abandon enumeration right after the
  `BOS` read. Linux and macOS treat an unusable BOS as non-fatal (Linux
  proceeds without BOS on parse failure), which explains the asymmetry.
  Fact 4's "stops right after BOS(255), 3 retries" matches this exactly.
- **H2**: stale Windows descriptor cache (same VID/PID/serial `1d50:615e`,
  descriptors changed vs. an earlier module-less firmware). Cheap to rule
  out; does *not* explain Fact 4 on its own, but must be excluded so the
  before/after test in Phase 3 is trustworthy.
- **H3**: side effects of the `--wrap=usb_handle_bos` hook or its `LOG_DBG`
  per SETUP packet (timing in the USB thread). Unlikely — the hook is
  observe-only and logging is deferred — verify only by elimination if H1
  fails.
- **H4**: other module features (layer auto-switch, settings, Studio RPC).
  Essentially impossible for an enumeration-level failure; covered by the
  config matrix in Phase 3 as a byproduct.

## Investigation steps

### Phase 0 — sandbox only, no hardware

1. Re-verify facts 1–3 against the exact pinned revisions actually used by
   the failing build (`dependencies/zephyr`, `dependencies/zmk`).
2. Build the hardware artifact (`west zmk-build tests/zmk-config`; needs
   `ZEPHYR_TOOLCHAIN_VARIANT=zephyr` and
   `ZEPHYR_SDK_INSTALL_DIR=/home/ubuntu/agent-home/zephyr-sdk-0.16.8`
   exported) and confirm in the produced `zmk.elf` that `bos_hdr`'s
   `wTotalLength` bytes are `00 00` (e.g.
   `arm-zephyr-eabi-objdump -s -j <section containing bos_hdr>` or `nm` +
   `objcopy` extract) and that the device descriptor's `bcdUSB` is
   `01 02`.

### Phase 1 — board plugged into the sandbox's own Linux host

The rig's board USB-C can be plugged into this sandbox's host (a real Linux
USB host) — see `docs/fingerprints.md` "What this environment can and cannot
capture". `usbmon`/`dmesg` are unavailable here, but userspace control
transfers via usbfs work.

1. Flash current (unfixed) firmware, plug into sandbox host.
2. `lsusb -d 1d50:615e -v` — confirm `bcdUSB 2.01` on the wire.
3. Read the raw BOS with a direct control transfer (pyusb:
   `dev.ctrl_transfer(0x80, 0x06, 0x0F00, 0, 5)`) — expected result proving
   H1's premise: `05 0F 00 00 00` (`wTotalLength=0`).
4. Confirm the keyboard still enumerates and types on Linux (baseline for
   "only Windows is broken").

### Phase 2 — confirm the Windows-side symptom & exclude H2 (needs the user)

Requires the user to plug the board into their Windows PC. Coordinate via
the hardware-lock protocol
(`/home/ubuntu/zmk-workspace/docs/hardware-locking.md`).

1. Ask the user for the Device Manager error text/code (expect "Unknown USB
   Device (Device Descriptor Request Failed)", Code 43).
2. Rule out H2: different physical USB port, and/or uninstall the device
   (Device Manager → View → Show hidden devices → uninstall) then replug.
   If a fresh port/cache still fails → H2 excluded.
3. Optional but valuable: while plugged into Windows, capture the SETUP
   sequence device-side over RTT (recipe: [hw-capture-recipe.md](hw-capture-recipe.md);
   remember the flash-offset overlay, `LOG_PROCESS_THREAD_STARTUP_DELAY_MS=0`,
   and the RTT signature-zeroing gotcha). Expected if H1 holds: the same
   3×-repeated sequence ending at `BOS(255)`, no `SET_CONFIGURATION`.

### Phase 3 — fix candidate + before/after matrix

Preferred fix for H1: **register a real BOS capability so the BOS descriptor
becomes spec-valid**, keeping the Windows-reads-BOS signal intact. In the
module (compiled only under `CONFIG_ZMK_OS_DETECTION_USB`), define a USB 2.0
Extension capability and register it before USB is enabled, following
`zephyr/samples/subsys/usb/webusb`:

```c
/* USB 2.0 Extension descriptor, bmAttributes=0 (no LPM claimed) */
USB_DEVICE_BOS_DESC_DEFINE_CAP struct usb_bos_capability_lpm bos_cap_lpm = {
    .bLength = sizeof(struct usb_bos_capability_lpm),
    .bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
    .bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
    .bmAttributes = 0,
};
/* in an early SYS_INIT (before ZMK enables USB): */
usb_bos_register_cap((void *)&bos_cap_lpm);
```

This makes BOS = `05 0F 0C 00 01` + `07 10 02 00 00 00 00`
(`wTotalLength=12`, `bNumDeviceCaps=1`). Verify the SYS_INIT ordering
actually runs before `usb_enable()` (ZMK enables USB in its own init);
registering later would leave `wTotalLength` stale for the first
enumeration.

Build matrix to flash in order (each step's outcome decides the next):

| Build | Purpose | Expected if H1 |
|---|---|---|
| (a) `ZMK_OS_DETECTION_USB=n` (module otherwise on) | baseline: is the module's USB part really the trigger? | Windows OK |
| (b) current firmware, fresh Windows port/cache | H2 exclusion (same as Phase 2) | Windows still broken |
| (c) fixed BOS build (cap registered) | the actual fix | Windows OK, detection still works |

Verify (c) first on the sandbox Linux host (Phase 1 method: BOS now reads
back 12 bytes, keyboard types, classifier still reports `LINUX`), then hand
to the user for Windows. Success criteria on Windows: RTT shows
`SET_CONFIGURATION` + HID `REPORT` descriptor read, the user can type, and
the settled classification is `WINDOWS`.

If (c) fails but (a) passed → H1 alone is insufficient: next lever is the
`bcdUSB` bump itself — test build with `dependencies/zephyr`
`usb_descriptor.c` locally patched back to `USB_SRN_2_0` (breaks detection,
pure isolation step), then consider redesigning the Windows signal without
BOS (see fallback below).

### Phase 4 — expected fallout: re-fingerprint and likely reclassify

Two classifier risks that the fix itself creates — plan for them, don't be
surprised by them:

1. **A healthy Windows enumeration will read string descriptors** (serial
   number etc., typically directly at `wLength=255`). The current rule order
   checks Linux (`string@255 && !short_probe`) *before* Windows, so a fixed,
   fully-enumerating Windows host would likely be **misclassified as
   Linux**. The Windows rule almost certainly needs to move ahead of the
   Linux rule or gain stronger signals (config-at-255-direct;
   `DEVICE_QUALIFIER`-retried-3x as a Linux-only marker; see the "not yet
   wired into the classifier" notes in fingerprints.md).
2. **macOS/Linux may re-read BOS at the new `wTotalLength` (12)** once
   capabilities exist (fingerprints.md explicitly predicted this), so the
   Windows test `bos_wlength > 5` becomes unsafe. A more robust Windows
   signal is `bos_wlength == 255` (blind max-length read) vs.
   read-exactly-what's-advertised.

Therefore after the fix: re-capture **all** of Windows/macOS/Linux/iOS (and
Android if convenient) with the RTT recipe, update `zmk_os_classify_usb()`,
the `inject_*_like()` sequences, `tests/os_detection_usb`, and rewrite
fingerprints.md's Windows section to state honestly that the old capture was
of a failing enumeration.

## Rig / environment notes for the executor

- Hardware lock: `/home/ubuntu/zmk-workspace/docs/hardware-locking.md` —
  take it before touching J-Link/board; the user must physically move the
  USB-C plug for Windows/macOS steps.
- RTT capture recipe (flash offset overlay, `JLinkExe` `mem32`/`savebin`
  instead of RTTLogger, the zero-the-RTT-signature-before-reset gotcha,
  `LOG_PROCESS_THREAD_STARTUP_DELAY_MS=0`, buffer sizing):
  [hw-capture-recipe.md](hw-capture-recipe.md) and
  `docs/fingerprints.md` § "RTT capture recipe".
- Build env: export `ZEPHYR_TOOLCHAIN_VARIANT=zephyr`,
  `ZEPHYR_SDK_INSTALL_DIR=/home/ubuntu/agent-home/zephyr-sdk-0.16.8`.
  Tests: `python3 -m unittest` (unit + build), `pre-commit run` before
  committing.

## Done checklist

- [x] Phase 1: `wTotalLength=0` confirmed on the wire, pre-fix (raw `pyusb`
      read from the sandbox's own Linux host: `05 0f 00 00 00`)
- [ ] Phase 2: Windows error code never recorded (owner moved the board to
      Windows directly; fix verified via RTT before/after instead — see
      "Resolution summary"); H2 (driver cache) not separately excluded,
      though the RTT-visible root cause (malformed BOS, deterministic
      regardless of any Windows-side cache) makes it very unlikely to be a
      contributing factor
- [x] Phase 3 (USB-level): fixed build reaches `SET_CONFIGURATION` + HID
      `REPORT` read on Windows, and enumerates cleanly on Linux (RTT +
      `pyusb`, both real hardware) — **not separately confirmed**: an
      explicit user "yes it types" on the Windows PC
- [x] Phase 4: Windows + Linux fingerprints re-captured, classifier
      (`bos_wlength == 255`, reordered) + `inject_*_like()` + unit tests
      updated and passing. **Not done**: macOS/iOS re-capture (no Mac/iPhone
      available this session)
- [x] fingerprints.md corrected re: the old Windows capture (see its new
      "Windows enumeration failure discovered and fixed" section)
- [ ] README "Known limitations" — not yet reviewed for this change
