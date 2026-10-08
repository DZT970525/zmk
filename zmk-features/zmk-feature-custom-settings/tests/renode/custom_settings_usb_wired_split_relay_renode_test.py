#!/usr/bin/env python3
"""Hardware-free end-to-end test: a Studio host exercises the custom-settings
Studio RPC on a split CENTRAL over an emulated USB CDC, then reads AND writes the
split PERIPHERAL's settings across the wired split event relay.

Topology (two real nRF52840 images on one Renode session; see
custom_settings_usb_wired_split_lib.py):

    host(USB CDC) --USB(Studio)--> CENTRAL --wired(split relay)--> PERIPHERAL

What it proves, end to end against real firmware:

  1. Boot + subsystem discovery: the central answers `ListCustomSubsystem` over
     USB and advertises the `cormoran_custom_settings` subsystem; the peripheral
     comes up on the wired split.
  2. Local (central) RPC: ListSettings enumerates the sample settings, GetSetting
     reads a value, WriteSetting mutates it and a re-GetSetting reflects the
     change -- i.e. the whole request/response path over USB CDC.
  3. PERIPHERAL write over the relay: a WriteSetting targeting `source != local`
     is relayed to the peripheral, applied there, and the peripheral's resulting
     value-updated notification is forwarded back to the host (source = the
     peripheral's split index).
  4. PERIPHERAL read over the relay: a ListSettings scoped to the peripheral is
     relayed, and the peripheral's list-item notification carries back the value
     just written -- proving the write landed on the *peripheral*, not the
     central, and is independently re-readable through the relay.

The usb+wired path round-trips the relayed notification where a pure-Studio
transport can't: the wired split link (cormoran/zmk#34's relay-event transport)
has no radio cap, and Studio rides the emulated USB CDC. The central disables
Studio locking (CONFIG_ZMK_STUDIO_LOCKING=n in its shield conf) because a
headless Renode run has no physical unlock key -- otherwise every RPC is rejected
with UNLOCK_REQUIRED and no relay is ever dispatched.

Run via:
  west zmk-build tests/zmk-config --build-yaml tests/zmk-config/build-usb-split.yaml -af usb-wired-central
  west zmk-build tests/zmk-config --build-yaml tests/zmk-config/build-usb-split.yaml -af usb-wired-peripheral
  west zmk-renode-test --mode wired-split \
      --elf build/usb-wired-central/zephyr/zmk.elf \
      --peripheral-elf build/usb-wired-peripheral/zephyr/zmk.elf \
      tests/renode

Named `*_renode_test.py` (not `test_*.py`) so it stays out of `python3 -m
unittest` auto-discovery -- it needs real firmware ELFs. SKIPs if not built.
"""

from __future__ import annotations

import os
import sys
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

try:
    import renode_harness as rh
except ImportError:  # pragma: no cover - local-dev fallback
    for cand in (
        REPO_ROOT / "dependencies" / "zmk-west-commands" / "scripts" / "lib" / "renode",
        REPO_ROOT.parent / "zmk-west-commands" / "scripts" / "lib" / "renode",
    ):
        if cand.is_dir():
            sys.path.insert(0, str(cand))
            import renode_harness as rh  # noqa: F401

            break
    else:
        raise

sys.path.insert(0, str(Path(__file__).resolve().parent))
import custom_settings_usb_wired_split_lib as splitlib  # noqa: E402

SUBSYSTEM_IDENTIFIER = "cormoran_custom_settings"
# Settings all live under this custom_subsystem_id namespace (src/test/
# zmk_config_sample_settings.c). We address them by key alone (custom-settings
# treats an omitted subsystem index as "match any"), so the test never has to
# hardcode the namespace index.
SAMPLE_INT32_KEY = "int32_value"
SAMPLE_INT32_DEFAULT = 42

# Non-local source selects the peripheral(s): custom-settings relays any
# request whose source != LOCAL(0) to the split peripherals (the relay
# normalizes the exact index to "the peripheral's local view"), and the
# peripheral's notifications come back tagged with its own split source index.
SOURCE_LOCAL = 0
SOURCE_PERIPHERAL = 1

# custom-settings write modes (SettingWriteMode). MEMORY avoids depending on the
# peripheral's NVS being writable under Renode; the value still round-trips in RAM.
WRITE_MODE_MEMORY = 0

# SettingNotificationKind
KIND_LIST_ITEM = 0
KIND_VALUE_UPDATED = 1


def _elf(env_name: str, default_rel: str) -> Path:
    val = os.environ.get(env_name)
    return Path(val) if val else REPO_ROOT / default_rel


def _load_protos():
    studio_proto_dir = rh.find_studio_proto_dir(REPO_ROOT)
    studio_pb2 = rh.load_studio_pb2(studio_proto_dir)  # also puts out_dir on sys.path
    import custom_pb2  # noqa: E402  (compiled alongside studio_pb2)

    # custom_settings.proto uses proto3 `optional` fields, which protoc <3.15
    # (the container ships 3.12) only compiles with an explicit opt-in flag not
    # exposed by renode_harness.compile_protos -- so invoke protoc directly here.
    import subprocess
    import tempfile

    out = Path(tempfile.mkdtemp(prefix="cs-proto-"))
    proto = (
        REPO_ROOT
        / "proto"
        / "cormoran"
        / "zmk"
        / "custom_settings"
        / "custom_settings.proto"
    )
    subprocess.run(
        [
            "protoc",
            "--experimental_allow_proto3_optional",
            f"-I{REPO_ROOT / 'proto'}",
            f"--python_out={out}",
            str(proto),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    os.environ.setdefault("PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION", "python")
    sys.path.insert(0, str(out))
    from cormoran.zmk.custom_settings import custom_settings_pb2  # noqa: E402

    return studio_pb2, custom_pb2, custom_settings_pb2


class CustomSettingsUsbWiredSplitRelayTest(unittest.TestCase):
    session = None
    _request_id = 0

    @classmethod
    def setUpClass(cls):
        cls.central = _elf("ZMK_RENODE_ELF", "build/usb-wired-central/zephyr/zmk.elf")
        cls.peripheral = _elf(
            "ZMK_RENODE_PERIPHERAL_ELF", "build/usb-wired-peripheral/zephyr/zmk.elf"
        )
        for name, p in (("central", cls.central), ("peripheral", cls.peripheral)):
            if not p.is_file():
                raise unittest.SkipTest(
                    f"{name} ELF not built: {p} (see this file's header)"
                )

        cls.renode = rh.find_or_install_renode()
        if not cls.renode:
            raise unittest.SkipTest("Renode not available")

        cls.studio_pb2, cls.custom_pb2, cls.cs_pb2 = _load_protos()

        cls.session, cls.c_con, cls.rpc, cls.p_con = splitlib.boot(
            cls.renode, cls.central, cls.peripheral
        )
        cls.p_buf = ""
        cls.c_buf = ""
        # Resolved once the central answers ListCustomSubsystem (test_01).
        cls.subsystem_index = None

    @classmethod
    def tearDownClass(cls):
        # Dump the full peripheral console for post-mortem debugging.
        dbg = os.environ.get("CS_RENODE_DEBUG_DIR")
        if dbg:
            try:
                Path(dbg, "peripheral_console.log").write_text(cls.p_buf)
                Path(dbg, "central_console.log").write_text(cls.c_buf)
            except OSError:
                pass
        if cls.session is not None:
            for sock in getattr(cls.session, "_cdc_sockets", []):
                try:
                    sock.close()
                except OSError:
                    pass
            try:
                cls.c_con.close()
                cls.p_con.close()
            except OSError:
                pass
            cls.session.stop()

    # ---- framing helpers ---------------------------------------------------

    @classmethod
    def _next_request_id(cls) -> int:
        cls._request_id += 1
        return cls._request_id

    def _send_custom(self, cs_request) -> int:
        """Frame + send a cormoran.zmk.custom_settings.Request to the resolved
        subsystem. Returns the request_id used."""
        rid = self._next_request_id()
        call = self.custom_pb2.Request()
        call.call.subsystem_index = self.__class__.subsystem_index
        call.call.payload = cs_request.SerializeToString()
        req = self.studio_pb2.Request()
        req.request_id = rid
        req.custom.CopyFrom(call)
        self.rpc.send(req.SerializeToString())
        return rid

    def _await_response(self, rid: int, timeout: float = 10.0):
        """Read frames until the RequestResponse for `rid` arrives; decode its
        custom payload as a custom_settings.Response. Notifications seen along the
        way are drained into self._notifications."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.rpc.read_frame(timeout=1.0)
            self._drain_peripheral()
            if frame is None:
                continue
            resp = self.studio_pb2.Response()
            try:
                resp.ParseFromString(frame)
            except Exception:
                continue
            which = resp.WhichOneof("type")
            if which == "notification":
                self._record_notification(resp)
                continue
            if which != "request_response":
                continue
            rr = resp.request_response
            if rr.request_id != rid:
                continue
            if rr.WhichOneof("subsystem") != "custom":
                return None
            cs = self.cs_pb2.Response()
            cs.ParseFromString(rr.custom.call.payload)
            return cs
        return None

    def _record_notification(self, resp) -> None:
        note = resp.notification
        if note.WhichOneof("subsystem") != "custom":
            return
        cn = note.custom.custom_notification
        cs_note = self.cs_pb2.Notification()
        try:
            cs_note.ParseFromString(cn.payload)
        except Exception:
            return
        if cs_note.WhichOneof("notification_type") == "setting":
            # cs_note.setting is a SettingNotification{kind, setting}; store its
            # inner Setting (has key/value/source).
            self.__class__._notifications.append(cs_note.setting.setting)

    def _collect_notifications(self, duration: float = 4.0):
        """Drain Studio frames for `duration`s, recording setting notifications."""
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            frame = self.rpc.read_frame(timeout=0.5)
            self._drain_peripheral()
            if frame is None:
                continue
            resp = self.studio_pb2.Response()
            try:
                resp.ParseFromString(frame)
            except Exception:
                continue
            if resp.WhichOneof("type") == "notification":
                self._record_notification(resp)

    _notifications = []

    def _drain_peripheral(self):
        self.__class__.p_buf += rh.drain_text(self.p_con._sock, timeout=0.05)
        self.__class__.c_buf += rh.drain_text(self.c_con._sock, timeout=0.05)

    # ---- request builders --------------------------------------------------

    def _get_setting(self, key: str, source: int | None = None):
        req = self.cs_pb2.Request()
        req.get_setting.setting.key = key
        if source is not None:
            req.get_setting.setting.source = source
        return req

    def _write_setting(self, key: str, int32_value: int, source: int | None = None):
        req = self.cs_pb2.Request()
        req.write_setting.setting.key = key
        if source is not None:
            req.write_setting.setting.source = source
        req.write_setting.value.int32_value = int32_value
        req.write_setting.mode = WRITE_MODE_MEMORY
        return req

    def _list_settings(self, key: str | None = None, source: int | None = None):
        req = self.cs_pb2.Request()
        # Select the list_settings oneof arm even with an empty scope (an
        # all-local list); otherwise the request carries no request_type.
        req.list_settings.SetInParent()
        if key is not None:
            req.list_settings.scope.key = key
        if source is not None:
            req.list_settings.scope.source = source
        return req

    # ---- tests -------------------------------------------------------------

    def test_01_boot_and_subsystem_discovery(self):
        # The central console prints its banner on uart0 (USB carries Studio).
        banner = rh.wait_for_text(self.c_con._sock, "Welcome to ZMK", timeout=25.0)
        self.assertIn("Welcome to ZMK", banner, "central never printed its boot banner")

        # ListCustomSubsystem over USB -> find cormoran_custom_settings' index.
        rid = self._next_request_id()
        call = self.custom_pb2.Request()
        call.list_custom_subsystems.SetInParent()
        req = self.studio_pb2.Request()
        req.request_id = rid
        req.custom.CopyFrom(call)

        index = None
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline and index is None:
            self.rpc.send(req.SerializeToString())
            inner_deadline = time.monotonic() + 4.0
            while time.monotonic() < inner_deadline and index is None:
                frame = self.rpc.read_frame(timeout=1.0)
                self._drain_peripheral()
                if frame is None:
                    continue
                resp = self.studio_pb2.Response()
                try:
                    resp.ParseFromString(frame)
                except Exception:
                    continue
                if resp.WhichOneof("type") != "request_response":
                    continue
                rr = resp.request_response
                if rr.WhichOneof("subsystem") != "custom":
                    continue
                if rr.custom.WhichOneof("response_type") != "list_custom_subsystems":
                    continue
                for sub in rr.custom.list_custom_subsystems.subsystems:
                    if sub.identifier == SUBSYSTEM_IDENTIFIER:
                        index = sub.index
                        break

        self.assertIsNotNone(
            index,
            f"central never advertised the {SUBSYSTEM_IDENTIFIER} subsystem over USB "
            f"(central console tail:\n{banner[-400:]})",
        )
        self.__class__.subsystem_index = index

    def test_02_central_get_default(self):
        cs = self._await_response(
            self._send_custom(self._get_setting(SAMPLE_INT32_KEY))
        )
        self.assertIsNotNone(cs, "no response to local GetSetting")
        self.assertEqual(cs.WhichOneof("response_type"), "get_setting", str(cs))
        setting = cs.get_setting.setting
        self.assertEqual(setting.key, SAMPLE_INT32_KEY)
        self.assertEqual(setting.value.int32_value, SAMPLE_INT32_DEFAULT)
        self.assertEqual(setting.source, SOURCE_LOCAL)

    def test_03_central_write_and_reget(self):
        new_val = 55
        cs = self._await_response(
            self._send_custom(self._write_setting(SAMPLE_INT32_KEY, new_val))
        )
        self.assertIsNotNone(cs, "no response to local WriteSetting")
        self.assertEqual(cs.WhichOneof("response_type"), "status", str(cs))

        cs = self._await_response(
            self._send_custom(self._get_setting(SAMPLE_INT32_KEY))
        )
        self.assertEqual(cs.get_setting.setting.value.int32_value, new_val)

    def test_04_central_list_settings(self):
        self.__class__._notifications = []
        self._send_custom(self._list_settings())
        # List items are emitted on the low-priority workqueue in delayed
        # batches, so poll until every expected key shows up (or time out).
        expected = {"int32_value", "bool_value", "string_value", "bytes_value"}
        keys: set = set()
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline and not expected <= keys:
            self._collect_notifications(duration=2.0)
            keys = {n.key for n in self._notifications if n.source == SOURCE_LOCAL}
        for want in sorted(expected):
            self.assertIn(
                want,
                keys,
                f"local ListSettings did not enumerate {want!r} "
                f"(got {sorted(keys)}; {len(self._notifications)} notifications total)",
            )

    def test_05_peripheral_write_relayed(self):
        # Write to the PERIPHERAL (source != local). The synchronous response is a
        # StatusResponse ("No local setting matched source"); the applied value
        # comes back asynchronously as a value-updated notification tagged with
        # the peripheral's source index.
        self.__class__._notifications = []
        peripheral_val = 77
        cs = self._await_response(
            self._send_custom(
                self._write_setting(
                    SAMPLE_INT32_KEY, peripheral_val, source=SOURCE_PERIPHERAL
                )
            )
        )
        self.assertIsNotNone(cs, "no synchronous response to peripheral WriteSetting")
        self.assertEqual(cs.WhichOneof("response_type"), "status", str(cs))

        # Drain for the relayed value-updated notification from the peripheral.
        updated = None
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline and updated is None:
            self._collect_notifications(duration=2.0)
            for n in self._notifications:
                if (
                    n.key == SAMPLE_INT32_KEY
                    and n.source == SOURCE_PERIPHERAL
                    and n.value.int32_value == peripheral_val
                ):
                    updated = n
                    break

        self.assertIsNotNone(
            updated,
            "host never received the peripheral's relayed value-updated notification "
            f"for {SAMPLE_INT32_KEY}={peripheral_val} "
            f"(peripheral console tail:\n{self.p_buf[-800:]})",
        )

    def test_06_peripheral_read_relayed(self):
        # Independently re-read the peripheral setting through a relayed
        # ListSettings scoped to it: the peripheral's list-item notification must
        # carry back the value written in test_05, proving it landed on the
        # peripheral and is re-readable across the relay.
        self.__class__._notifications = []
        self._send_custom(
            self._list_settings(key=SAMPLE_INT32_KEY, source=SOURCE_PERIPHERAL)
        )

        found = None
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline and found is None:
            self._collect_notifications(duration=2.0)
            for n in self._notifications:
                if n.key == SAMPLE_INT32_KEY and n.source == SOURCE_PERIPHERAL:
                    found = n
                    break

        self.assertIsNotNone(
            found,
            "host never received a relayed list-item notification for the peripheral "
            f"{SAMPLE_INT32_KEY} (peripheral console tail:\n{self.p_buf[-800:]})",
        )
        self.assertEqual(
            found.value.int32_value,
            77,
            "relayed peripheral read did not reflect the value written in test_05",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
