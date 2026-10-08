#!/usr/bin/env python3
"""Boot a USB+wired custom-settings split (central answering Studio over USB CDC
+ a relay-enabled peripheral) under Renode and hand back a framed Studio RPC
socket for the central's USB CDC.

This is a thin wrapper over zmk-west-commands' renode_harness.boot_usb_wired_split
(no local sensor, so the generic sensor-less wired-split repl is exactly right)
that additionally attaches the DualCdcAcmBridge USB host to the central and picks
the Studio CDC socket, so the test file stays focused on the RPC round-trips.

Topology:

    host(CDC bridge) --USB(Studio)--> CENTRAL --wired(split relay)--> PERIPHERAL

Requires `renode_harness` on PYTHONPATH (provided by `west zmk-renode-test`).
"""

from __future__ import annotations

import re
import time

import renode_harness as rh

# The central presents a SINGLE USB CDC (Studio) because the shield disables the
# board console CDC (CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n) -- so the Studio RPC
# rides cdc0. A dual-CDC image (board console CDC on) would put Studio on cdc1;
# we detect that via the bridge's IsWired flags, mirroring renode_smoke.
USB_BRIDGE_NAME = "bridge"


def boot(
    renode_path,
    central_elf,
    peripheral_elf,
    port_base: int = 33000,
    usb_settle: float = 2.0,
):
    """Boot the two machines, attach the USB CDC bridge to the central, and return
    (session, central_console, studio_rpc, peripheral_console). studio_rpc is the
    framed RpcSocket for the central's Studio CDC (host side). Caller owns cleanup
    (session.stop() + closing sockets)."""
    session, central_console, peripheral_console = rh.boot_usb_wired_split(
        renode_path,
        central_elf=central_elf,
        peripheral_elf=peripheral_elf,
        port_base=port_base,
    )
    try:
        # boot_usb_wired_split leaves the central selected; let it finish USB
        # bring-up before the host attaches, then wire the CDC bridge.
        time.sleep(usb_settle)
        cdc = list(
            rh.attach_dual_cdc_bridge(
                session, port_base + 3, port_base + 4, name=USB_BRIDGE_NAME
            )
        )
        dual_cdc = bool(
            _mon_flag(session.mon, f"sysbus.{USB_BRIDGE_NAME}_cdc1 IsWired")
        )
        studio_rpc = cdc[1] if dual_cdc else cdc[0]
        session._cdc_sockets = cdc  # keep both alive for the session's lifetime
    except Exception:
        session.stop()
        raise
    return session, central_console, studio_rpc, peripheral_console


def _mon_flag(mon, cmd: str, timeout: float = 5.0):
    """Return True/False for a monitor command that prints a boolean, else None."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        out = mon.execute(cmd, settle=0.3)
        m = re.search(r"\b(True|False)\b", out)
        if m:
            return m.group(1) == "True"
    return None
