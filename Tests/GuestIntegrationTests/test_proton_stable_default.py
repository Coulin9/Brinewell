import runpy
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
ROOTFS = ROOT / "runtime/guest/rootfs"
TOOLS_DIR = ROOTFS / "usr/share/steam/compatibilitytools.d"
TOOLS = (
    ("proton-stable", "Proton 11.0 (x86-64)", "4628710", "4628711"),
    ("proton-experimental", "Proton Experimental (x86-64)", "1493710", "1493711"),
)
vdf_entries = runpy.run_path(str(ROOTFS / "usr/lib/brinewell/brinewell_vdf.py"))["vdf_entries"]


class ProtonStableDefaultTests(unittest.TestCase):
    def test_registration_resolves_official_proton(self):
        for tool_id, display_name, appid, depotid in TOOLS:
            with self.subTest(tool_id=tool_id):
                vdf = TOOLS_DIR / f"brinewell-{tool_id}" / "compatibilitytool.vdf"
                values, blocks, _ = vdf_entries(vdf.read_text())
                self.assertFalse(set(values) & set(blocks))
                tools = ("compatibilitytools", "compat_tools")
                prefix = (*tools, tool_id)
                self.assertIn(tools, blocks)
                self.assertEqual(
                    {path for path in (*values, *blocks)
                     if len(path) == len(tools) + 1 and path[:len(tools)] == tools},
                    {prefix},
                )
                self.assertIn(prefix, blocks)
                expected = {
                    "install_path": ".",
                    "display_name": display_name,
                    "appid": appid,
                    "depotid": depotid,
                    "require_tool_appid": "4183110",
                    "from_oslist": "windows",
                    "to_oslist": "linux",
                    "unlisted": "1",
                }
                for key, value in expected.items():
                    with self.subTest(key=key):
                        self.assertEqual(values[(*prefix, key)][0], value)

    def test_tool_manifest(self):
        for tool_id, _, _, _ in TOOLS:
            with self.subTest(tool_id=tool_id):
                manifest = TOOLS_DIR / f"brinewell-{tool_id}" / "toolmanifest.vdf"
                values, blocks, _ = vdf_entries(manifest.read_text())
                self.assertIn(("manifest",), blocks)
                expected = {
                    "version": "2",
                    "commandline": "/proton %verb%",
                    "require_tool_appid": "4183110",
                    "use_sessions": "1",
                    "compatmanager_layer_name": "proton",
                }
                for key, value in expected.items():
                    with self.subTest(key=key):
                        self.assertEqual(values[("manifest", key)][0], value)

    def test_registration_is_readable_from_guest(self):
        for tool_id, _, _, _ in TOOLS:
            tool_dir = TOOLS_DIR / f"brinewell-{tool_id}"
            for file in (tool_dir / "compatibilitytool.vdf", tool_dir / "toolmanifest.vdf"):
                with self.subTest(file=file):
                    self.assertEqual(file.stat().st_mode & 0o644, 0o644)
            for directory in [tool_dir, *tool_dir.parents]:
                with self.subTest(directory=directory):
                    self.assertEqual(directory.stat().st_mode & 0o555, 0o555)
                if directory == ROOTFS:
                    break


if __name__ == "__main__":
    unittest.main()
