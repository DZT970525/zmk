import platform
import shutil
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import NoteSection

THIS_DIR = Path(__file__).parent.resolve()


def read_build_id(elf_path: Path) -> str | None:
    """Return the GNU build-id (hex) embedded in an ELF, or None if absent."""
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        for section in elf.iter_sections():
            if not isinstance(section, NoteSection):
                continue
            for note in section.iter_notes():
                if note["n_type"] == "NT_GNU_BUILD_ID":
                    return note["n_desc"]
    return None


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )


@dataclass
class NotFound:
    text: str


@dataclass
class ConfigAndDeviceTree:
    # Expected rows in .config
    config: list[str | NotFound]
    # Expected rows in devicetree_generated.h
    device: list[str | NotFound]


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(run_west(["topdir"]).stdout.strip())
        cls.BUILD_DIR = cls.WEST_TOPDIR / "build"

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        test_build_dir = self.BUILD_DIR / THIS_DIR.name
        shutil.rmtree(test_build_dir, ignore_errors=True)

        result = run_west(["zmk-test", "tests", "-m", ".", "-d", str(test_build_dir)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: test", result.stdout, result.stdout + result.stderr)
        self.assertIn("PASS: studio", result.stdout, result.stdout + result.stderr)
        self.assertIn(
            "PASS: studio_unsecured", result.stdout, result.stdout + result.stderr
        )
        self.assertNotIn("FAILED: ", result.stdout, result.stdout + result.stderr)

    def test_zmk_build(self):
        self._test_zmk_build(
            {
                "board_feature_disabled": ConfigAndDeviceTree(
                    config=[
                        'CONFIG_ZMK_KEYBOARD_NAME="Module Test"',
                        "CONFIG_ZMK_USB=y",
                        "CONFIG_ZMK_BLE=y",
                        "# CONFIG_ZMK_DEVICE_INFO is not set",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_zmk_keymap",
                    ],
                ),
                "board_with_device_info_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_DEVICE_INFO=y",
                        "CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC=y",
                        # A Studio unlock is required to read device info by default.
                        "CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC_REQUIRE_UNLOCK=y",
                    ],
                    device=[],
                ),
                "board_without_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_DEVICE_INFO=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                        NotFound("CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC"),
                    ],
                    device=[],
                ),
            }
        )

        # The RPC feature enables a GNU build-id so a running device can be
        # matched back to its ELF; it must be present (SHA1 => 40 hex chars) in
        # that artifact and absent when the RPC feature is off.
        rpc_elf = self.BUILD_DIR / "board_with_device_info_rpc" / "zephyr" / "zmk.elf"
        self.assertTrue(rpc_elf.exists(), f"{rpc_elf} is missing")
        rpc_build_id = read_build_id(rpc_elf)
        self.assertIsNotNone(
            rpc_build_id, "build-id note missing from RPC firmware ELF"
        )
        self.assertEqual(
            len(rpc_build_id), 40, f"expected SHA1 build-id, got: {rpc_build_id}"
        )

        no_rpc_elf = self.BUILD_DIR / "board_without_rpc" / "zephyr" / "zmk.elf"
        self.assertTrue(no_rpc_elf.exists(), f"{no_rpc_elf} is missing")
        self.assertIsNone(
            read_build_id(no_rpc_elf),
            "build-id note should be absent when RPC feature is disabled",
        )

    def _test_zmk_build(
        self, artifacts_and_expected_build_params: dict[str, ConfigAndDeviceTree]
    ):

        for artifact in artifacts_and_expected_build_params.keys():
            shutil.rmtree(self.BUILD_DIR / artifact, ignore_errors=True)

        result = run_west(["zmk-build", "tests/zmk-config", "-q"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_build_params.items():
            artifact_dir = self.BUILD_DIR / artifact / "zephyr"
            config_path = artifact_dir / ".config"
            device_tree_path = (
                artifact_dir
                / "include"
                / "generated"
                / "zephyr"
                / "devicetree_generated.h"
            )
            self._test_strings_in_file(
                config_path, entries.config, f"{artifact} config"
            )
            if entries.device:
                self._test_strings_in_file(
                    device_tree_path, entries.device, f"{artifact} device tree"
                )
            self.assertTrue(
                (artifact_dir / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {artifact_dir}",
            )

    def _test_strings_in_file(
        self, file_path: Path, expected_strings: list[str | NotFound], hint: str
    ):
        self.assertTrue(file_path.exists(), f"{hint}: {file_path} is missing")
        file_text = file_path.read_text()

        for expected in expected_strings:
            if isinstance(expected, NotFound):
                if expected.text in file_text:
                    self.fail(
                        f"{hint}: {expected.text} found in {file_path}, but it should not be present"
                    )
            else:
                if expected not in file_text:
                    self.fail(f"{hint}: {expected} not found in {file_path}")


if __name__ == "__main__":
    unittest.main()
