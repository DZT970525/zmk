# Reproducible hardware-capture recipe

`docs/fingerprints.md` describes *what* was captured and *why* the
classifier looks the way it does. This file has the exact, copy-pasteable
commands used to get there — none of this is committed as scripts (it's
rig-specific and workflow-specific, not code the module depends on), so
it's written down here instead of only existing in shell history.

Everything below assumes: this workspace's rig (physical XIAO nRF52840 +
J-Link, see `skills/develop-zmk-module/references/hardware-rig.md` in the
parent `zmk-workspace`), the isolated west workspace already initialized in
this repo (`dependencies/` populated), and the board's USB-C plugged into
whatever host OS you want to fingerprint (SWD stays on this sandbox's
J-Link regardless — the two connections are electrically independent).

## 1. The flash-offset devicetree overlay

This specific rig's flash below `0x27000` holds stale firmware from an
unrelated prior project, so a normal `xiao_ble` build (linked at the
default `0x27000` bootloader offset) never runs when SWD-flashed directly —
symptom: no USB enumeration at all, J-Link halt shows PC parked in
`arch_cpu_idle` with an unchanged value across repeated halts (**not** a
fault loop, and not distinguishable from "load offset didn't take" without
checking the `.hex`'s first record / `.config`'s `CONFIG_FLASH_LOAD_OFFSET`
— see §4). The fix is a devicetree overlay that moves `code_partition` to
start at `0x0`, bypassing the (in this rig's case unusable) bootloader
region entirely:

```dts
/delete-node/ &sd_partition;
/delete-node/ &code_partition;
/delete-node/ &storage_partition;

&flash0 {
    partitions {
        code_partition: partition@0 {
            reg = <0x0 DT_SIZE_K(848)>;
        };

        storage_partition: partition@d4000 {
            reg = <DT_SIZE_K(848) DT_SIZE_K(128)>;
        };
    };
};
```

Save this as, e.g., `/tmp/flash-offset-0.overlay` (deliberately **not**
committed to this repo — it's a workaround for one physical unit's flash
history, not something every user of this module needs). A plain
`-DCONFIG_FLASH_LOAD_OFFSET=0x0` cmake argument does **not** work on its
own — the offset is derived from the devicetree partition, not a free
Kconfig prompt you can override directly.

## 2. Building the debug firmware

Same Kconfig set every time, only `CONFIG_ZMK_OS_DETECTION_BLE` differs if
you also want BLE signals (unverified — see fingerprints.md):

```bash
nix --extra-experimental-features 'nix-command flakes' develop /home/ubuntu/zmk-workspace/nix \
  --command bash -lc 'west build -s dependencies/zmk/app -d build/hw_validate -b xiao_ble//zmk -p always -- \
    -DSHIELD=tester_xiao \
    "-DZMK_EXTRA_MODULES=$(pwd)/tests/zmk-config;$(pwd)" \
    -DZMK_CONFIG=$(pwd)/tests/zmk-config/config \
    -DCONFIG_ZMK_OS_DETECTION=y \
    -DCONFIG_ZMK_OS_DETECTION_USB=y \
    -DCONFIG_ZMK_BLE=n \
    -DCONFIG_LOG=y \
    -DCONFIG_USE_SEGGER_RTT=y \
    -DCONFIG_LOG_BACKEND_RTT=y \
    -DCONFIG_ZMK_LOG_LEVEL_DBG=y \
    -DCONFIG_SEGGER_RTT_BUFFER_SIZE_UP=8192 \
    -DCONFIG_LOG_PROCESS_THREAD_STARTUP_DELAY_MS=0 \
    -DDTC_OVERLAY_FILE=/tmp/flash-offset-0.overlay'
```

Verify the offset actually took effect before flashing (cheap, catches the
"looks built fine but silently still linked at 0x27000" failure mode
early):

```bash
head -1 build/hw_validate/zephyr/zmk.hex        # first record should start at address 0
grep CONFIG_FLASH_LOAD_OFFSET build/hw_validate/zephyr/.config   # should read =0x0
```

Notes on the Kconfig choices:

- `CONFIG_ZMK_BLE=n` — not required for USB capture, but real BLE
  connect/disconnect churn (e.g. a stale bond auto-reconnecting) floods the
  small RTT buffer with unrelated log lines and can crowd out the USB
  SETUP-packet lines you actually want. Turn it back on only if you're
  after BLE signals instead.
- `CONFIG_LOG_PROCESS_THREAD_STARTUP_DELAY_MS=0` — defaults to 5000 in a
  normal ZMK build; the deferred log backend won't flush *anything* to RTT
  before that delay elapses. Forgetting this override looks exactly like
  "RTT capture isn't working" for the first 5 seconds after boot.
- `CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=8192` — the 1KB default overflows in
  well under a second once `CONFIG_ZMK_LOG_LEVEL_DBG=y` is on (boot-time
  `DBG` logging from kscan/behavior init/etc. is chatty). 8192 was enough
  to capture every real trace in `docs/fingerprints.md` without wrapping,
  read within a few seconds of reset.

## 3. Finding the RTT control block address

The `_SEGGER_RTT` symbol's address changes between builds (depends on
overall `.bss`/`.noinit` layout, e.g. it moved when `SEGGER_RTT_BUFFER_SIZE_UP`
changed from 1KB to 8KB). Look it up fresh for each build — don't reuse an
address from a previous build even if it happened to be identical before:

```bash
find /nix/store -maxdepth 1 -iname 'zephyr-sdk-*' # confirm the SDK's nix store path once
/nix/store/<zephyr-sdk-hash>/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm \
  build/hw_validate/zephyr/zmk.elf | grep ' B _SEGGER_RTT$'
```

`arm-zephyr-eabi-nm` (and the rest of the Zephyr SDK toolchain binaries)
are **not** on `PATH` even inside the nix devshell in this sandbox — call
via the direct `/nix/store/...` path as above, found once via `find`.

## 4. Flashing

```bash
cat > /tmp/flash.jlink <<'EOF'
loadfile build/hw_validate/zephyr/zmk.hex
r
go
exit
EOF
JLinkExe -device nRF52840_xxAA -if SWD -speed 4000 -autoconnect 1 -CommandFile /tmp/flash.jlink
```

`loadfile` already does an implicit halt+reset; the explicit `r`+`go`
after it is belt-and-suspenders. **Never** issue `erase` (removes the UF2
bootloader region on rigs that have one — not relevant to this particular
unit's flash-offset workaround, but a blanket rule for this rig regardless).

## 5. Reading RTT without `JLinkRTTLogger`/`JLinkRTTClient`

Both of those tools reported "RTT Control Block not found" in this
environment even when given the exact right `-RTTAddress` from step 3, and
even with `-RTTSearchRanges` covering the whole SRAM region. Root cause
undiagnosed (plausibly something about how they attach vs. how `JLinkExe`
attaches, or a version mismatch in this container's SEGGER tools build) —
worked around by reading the RTT ring buffer directly as plain memory via
`JLinkExe`, since the control block's layout is simple and documented:

```
struct SEGGER_RTT_CB {
    char    acID[16];              // "SEGGER RTT" + padding
    int     MaxNumUpBuffers;
    int     MaxNumDownBuffers;
    RTT_BUFFER aUp[MaxNumUpBuffers];    // aUp[0] is the one that matters here
    RTT_BUFFER aDown[MaxNumDownBuffers];
};
struct RTT_BUFFER {
    const char *sName;
    char       *pBuffer;
    unsigned    SizeOfBuffer;
    unsigned    WrOff;
    unsigned    RdOff;
    unsigned    Flags;
};
```

So `aUp[0]` starts at `_SEGGER_RTT + 24` (16-byte `acID` + two 4-byte
counts), and its fields are `sName, pBuffer, SizeOfBuffer, WrOff, RdOff,
Flags` in that order, 4 bytes each.

**The gotcha that costs the most time if you don't know it going in**:
nRF52's `AIRCR.SYSRESETREQ` (what `JLinkExe`'s `r` does) does **not** clear
RAM. SEGGER's RTT init checks whether its own `"SEGGER RTT"` signature is
already present in memory and, if so, treats the block as "already
initialized" and skips resetting `WrOff`/`RdOff`/`pBuffer`/`sName` — a
deliberate feature (lets a debugger attach after boot and still see
consistent state), but it means that after reflashing a *different* build,
the control block can keep pointing at stale metadata from the *previous*
image (e.g. `sName` pointing at a string address that only made sense in
the old, differently-sized binary) and `WrOff`/`RdOff` simply never
advance — even though the new firmware is running fine and is actively
writing new log lines through `SEGGER_RTT_Write()`, which doesn't care
whether `Init()` actually ran. Every one of the five captures in
`docs/fingerprints.md` needed this fix applied before it produced anything
but frozen leftover data from the previous capture:

```bash
cat > /tmp/clear_and_reset.jlink <<'EOF'
halt
w4 0x20002010, 0x00000000
w4 0x20002014, 0x00000000
w4 0x20002018, 0x00000000
w4 0x2000201C, 0x00000000
r
g
sleep 1500
mem32 0x20002028, 0x8
exit
EOF
JLinkExe -device nRF52840_xxAA -if SWD -speed 4000 -autoconnect 1 -CommandFile /tmp/clear_and_reset.jlink
```

(Replace `0x20002010` with whatever `_SEGGER_RTT` resolved to in step 3;
`0x20002028` is `_SEGGER_RTT + 24`, the start of `aUp[0]`.) The final
`mem32` line's fourth word is `WrOff` — sanity-check it's small/plausible
before moving on; `0x00000000`/near-zero right after a fresh boot is a good
sign, `0x00001FFF`-ish immediately on a 0x2000-byte buffer means it's
already near-full from very chatty boot logging and you should read out
the buffer soon before it wraps.

Then dump and read:

```bash
cat > /tmp/dump.jlink <<'EOF'
mem32 0x20002028, 0x8
savebin /tmp/rtt_capture.bin 0x20000010 0x2000
exit
EOF
JLinkExe -device nRF52840_xxAA -if SWD -speed 4000 -autoconnect 1 -CommandFile /tmp/dump.jlink

strings -n 3 /tmp/rtt_capture.bin | grep SETUP
```

(`0x20000010` is `pBuffer` from step 3's read, `0x2000` is `SizeOfBuffer` —
both need to match whatever the current build actually reports, don't
hardcode them across builds.) `strings` on the raw binary works fine even
though the buffer is logically circular — every capture so far was read
before `WrOff` wrapped past the end, so the bytes were physically
contiguous in the order written. If a future capture needs more than one
buffer's worth of data, don't rely on this shortcut — either enlarge
`CONFIG_SEGGER_RTT_BUFFER_SIZE_UP` further or read `WrOff` twice (before
and after the interesting activity) and extract only the delta range,
handling wraparound explicitly.

## 6. Sandbox note (corrected): `pre-commit`'s Node bootstrap is fine, just don't mix contexts with a stale cache

Not a hardware-capture issue, but worth recording precisely because an
*earlier, incorrect* diagnosis of this made it into this session's own
commits for a while. This project's original dev sandbox has two Node
installs (`/usr/local/bin/node` v24.18.0 and an old apt-installed
`/usr/bin/node` v12.22.9). At one point, after repeatedly invoking
`pre-commit run --all-files` alternately from inside a nix devshell and
from a plain shell in the same session, the `prettier`/`eslint`/`jest`/
`web-build` hooks started failing with `Error: Cannot find module
'node:path'` inside pre-commit's managed `node_env-system` environment -
consistently enough (several repeats) that it was documented in commit
messages as a persistent environment bug and worked around with
`SKIP=prettier,eslint,jest,web-build` (checks still verified natively via
plain `npm` first, so nothing was actually skipped in substance - just
redundant).

**That diagnosis was wrong.** A later, deliberate re-test - `rm -rf
~/.cache/pre-commit` followed by a single clean `pre-commit run
--all-files`, tried both purely outside the nix devshell and purely inside
it - passed *every* hook, including the four blamed above, with no `SKIP`
needed either time. The actual trigger was some interaction between
alternating nix-devshell and plain-shell invocations against the *same*
`~/.cache/pre-commit` directory (plausibly: the two contexts create/expect
a `node_env-system` built against different Node binaries, and reusing one
context's cached env from the other context's invocation breaks it) - not
a fixed, always-reproducible conflict. The one real, unavoidable
constraint is that the `zmk-module-test` hook needs the Zephyr SDK/`west`
toolchain, which is only on `PATH` inside the nix devshell (confirmed: it
fails with `FileNotFoundError`/missing-toolchain errors if pre-commit is
run in a plain shell that never sourced the devshell).

**If you hit the `node:path` error**: don't reach for a permanent `SKIP=`.
Just `rm -rf ~/.cache/pre-commit` and re-run `pre-commit run --all-files`
once, consistently in whichever context (nix devshell or plain shell) you
intend to keep using for the rest of the session.
