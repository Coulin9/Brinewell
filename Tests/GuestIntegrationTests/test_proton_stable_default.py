import runpy
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
ROOTFS = ROOT / "runtime/guest/rootfs"
TOOL_DIR = ROOTFS / "usr/share/steam/compatibilitytools.d/brinewell-proton-stable"
VDF = TOOL_DIR / "compatibilitytool.vdf"
MANIFEST = TOOL_DIR / "toolmanifest.vdf"
vdf_entries = runpy.run_path(str(ROOTFS / "usr/lib/brinewell/brinewell_vdf.py"))["vdf_entries"]


class ProtonStableDefaultTests(unittest.TestCase):
    def test_registration_resolves_official_proton(self):
        values, blocks, _ = vdf_entries(VDF.read_text())
        self.assertFalse(set(values) & set(blocks))
        tools = ("compatibilitytools", "compat_tools")
        prefix = (*tools, "proton-stable")
        self.assertIn(tools, blocks)
        self.assertEqual(
            {path for path in (*values, *blocks)
             if len(path) == len(tools) + 1 and path[:len(tools)] == tools},
            {prefix},
        )
        self.assertIn(prefix, blocks)
        expected = {
            "install_path": ".",
            "appid": "4628710",
            "depotid": "4628711",
            "require_tool_appid": "4183110",
            "from_oslist": "windows",
            "to_oslist": "linux",
            "unlisted": "1",
        }
        for key, value in expected.items():
            with self.subTest(key=key):
                self.assertEqual(values[(*prefix, key)][0], value)
        self.assertTrue(values[(*prefix, "display_name")][0].strip())

    def test_tool_manifest(self):
        values, blocks, _ = vdf_entries(MANIFEST.read_text())
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
        for file in (VDF, MANIFEST):
            with self.subTest(file=file):
                self.assertEqual(file.stat().st_mode & 0o444, 0o444)
        for directory in [VDF.parent, *VDF.parent.parents]:
            with self.subTest(directory=directory):
                self.assertEqual(directory.stat().st_mode & 0o555, 0o555)
            if directory == ROOTFS:
                break


if __name__ == "__main__":
    unittest.main()
