import importlib.machinery
import importlib.util
import os
from pathlib import Path
import shutil
import json
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "runtime/guest/rootfs/usr/lib/brinewell/steam-fex-eac"
loader = importlib.machinery.SourceFileLoader("steam_fex_eac", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
fex_eac = importlib.util.module_from_spec(spec)
loader.exec_module(fex_eac)

BINARIES = ("FEX", "FEXBash", "FEXGetConfig", "FEXOfflineCompiler", "FEXServer", "FEXServerManager", "FEXpidof")


def versions(describe, bash="0" * 40):
    return "FEX describe\t{}\nFEX bash\t{}\n".format(describe, bash)


def make_tool(root, describe, tag):
    tool = root / "steamapps/common/FEX-Emu"
    for name in BINARIES:
        path = tool / "usr/bin" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes("{}-{}".format(tag, name).encode() * 64)
        path.chmod(0o755)
    (tool / "FEXCompatTool").write_bytes("{}-FEXCompatTool".format(tag).encode() * 32)
    (tool / "ConfigTemplate.json").write_bytes("{}-ConfigTemplate.json".format(tag).encode() * 32)
    (tool / "VERSIONS.txt").write_text(versions(describe))
    return tool


class FexEacTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        base = Path(self.directory.name)
        self.steam = base / "steam"
        self.state = base / "state"
        self.payload = make_tool(base, "eac-2610.5", "payload")
        template = {"Config": {"DiskCache": "1", "ProfileStats": "1"}}
        (self.payload / "ConfigTemplate.json").write_text(json.dumps(template))
        self.tool = make_tool(self.steam, "FEX-2607-abcdefg", "valve")
        environment = {
            "BRINEWELL_STEAM_ROOT": str(self.steam),
            "BRINEWELL_FEX_EAC_PAYLOAD": str(self.payload),
            "BRINEWELL_FEX_EAC_STATE": str(self.state),
        }
        patcher = mock.patch.dict(os.environ, environment)
        patcher.start()
        self.addCleanup(patcher.stop)
        busy = mock.patch.object(fex_eac, "steam_busy", return_value=None)
        busy.start()
        self.addCleanup(busy.stop)
        self.backup = self.state / "backup"

    def snapshot(self, root):
        return {str(path.relative_to(root)): path.read_bytes()
                for path in sorted(root.rglob("*")) if path.is_file()}

    def assert_installed(self):
        for name in fex_eac.FILES:
            self.assertEqual(fex_eac.installed_bytes(self.payload, name),
                             (self.tool / name).read_bytes(), str(name))

    def test_apply_installs_payload_and_backs_up_valve_build(self):
        fex_eac.apply()
        self.assert_installed()
        self.assertTrue(fex_eac.complete(self.backup))
        self.assertEqual(fex_eac.describe(self.backup), "FEX-2607-abcdefg")
        self.assertEqual((self.backup / "usr/bin/FEX").read_bytes()[:5], b"valve")

    def test_second_apply_is_noop_and_backup_unchanged(self):
        fex_eac.apply()
        snapshot = self.snapshot(self.backup)
        fex_eac.apply()
        self.assertEqual(self.snapshot(self.backup), snapshot)

    def test_steam_tool_update_is_rebacked_up_and_repatched(self):
        fex_eac.apply()
        make_tool(self.steam, "FEX-2607-fffffff", "newvalve")
        fex_eac.apply()
        self.assert_installed()
        self.assertEqual(fex_eac.describe(self.backup), "FEX-2607-fffffff")
        self.assertEqual((self.backup / "usr/bin/FEX").read_bytes()[:8], b"newvalve")

    def test_other_eac_build_installs_without_backup(self):
        shutil.rmtree(self.tool)
        self.tool = make_tool(self.steam, "eac-2609.1", "othereac")
        fex_eac.apply()
        self.assert_installed()
        self.assertFalse(self.backup.exists())

    def test_restore_then_disable_then_enable(self):
        fex_eac.apply()
        fex_eac.restore()
        self.assertEqual((self.tool / "usr/bin/FEX").read_bytes()[:5], b"valve")
        self.assertEqual(fex_eac.describe(self.tool), "FEX-2607-abcdefg")
        self.assertTrue((self.state / "disabled").exists())
        snapshot = self.snapshot(self.tool)
        fex_eac.apply()
        self.assertEqual(self.snapshot(self.tool), snapshot)
        fex_eac.enable()
        self.assert_installed()
        self.assertFalse((self.state / "disabled").exists())

    def test_missing_tool_is_noop(self):
        shutil.rmtree(self.tool)
        fex_eac.apply()
        self.assertFalse(self.tool.exists())
        self.assertIn("not installed", (self.state / "status").read_text())

    def test_busy_steam_changes_nothing(self):
        with mock.patch.object(fex_eac, "steam_busy", return_value="steam"):
            fex_eac.apply()
        self.assertEqual(fex_eac.describe(self.tool), "FEX-2607-abcdefg")
        self.assertFalse(self.backup.exists())

    def test_incomplete_payload_errors_and_leaves_tool_untouched(self):
        os.remove(self.payload / "usr/bin/FEXServer")
        snapshot = self.snapshot(self.tool)
        with self.assertRaises(SystemExit) as raised:
            fex_eac.apply()
        self.assertEqual(raised.exception.code, 1)
        self.assertEqual(self.snapshot(self.tool), snapshot)
        self.assertFalse(self.backup.exists())

    def test_restore_without_backup_errors(self):
        with self.assertRaises(SystemExit) as raised:
            fex_eac.restore()
        self.assertEqual(raised.exception.code, 1)
        self.assertIn("no Valve build backed up", (self.state / "status").read_text())



    def test_installed_template_enables_vulkan_thunk(self):
        fex_eac.apply()
        config = json.loads((self.tool / "ConfigTemplate.json").read_text())
        payload_config = json.loads((self.payload / "ConfigTemplate.json").read_text())
        self.assertEqual(config["ThunksDB"], {"Vulkan": "1"})
        config.pop("ThunksDB")
        self.assertEqual(config, payload_config)

    def test_second_apply_is_noop_for_installed_files(self):
        fex_eac.apply()
        stat = (self.tool / "ConfigTemplate.json").stat()
        fex_eac.apply()
        after = (self.tool / "ConfigTemplate.json").stat()
        self.assertEqual((after.st_ino, after.st_mtime_ns), (stat.st_ino, stat.st_mtime_ns))
        self.assertIn("installed", (self.state / "status").read_text())

    def test_existing_vulkan_value_is_kept(self):
        template = {"Config": {"DiskCache": "1"}, "ThunksDB": {"Vulkan": "0"}}
        (self.payload / "ConfigTemplate.json").write_text(json.dumps(template))
        fex_eac.apply()
        config = json.loads((self.tool / "ConfigTemplate.json").read_text())
        self.assertEqual(config["ThunksDB"]["Vulkan"], "0")

    def test_restore_writes_exact_valve_template_bytes(self):
        valve_bytes = (self.tool / "ConfigTemplate.json").read_bytes()
        fex_eac.apply()
        fex_eac.restore()
        self.assertEqual((self.tool / "ConfigTemplate.json").read_bytes(), valve_bytes)


if __name__ == "__main__":
    unittest.main()
