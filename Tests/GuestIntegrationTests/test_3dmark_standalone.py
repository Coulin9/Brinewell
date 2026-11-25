import importlib.machinery
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "runtime/guest/rootfs/usr/lib/brinewell/3dmark-standalone"
loader = importlib.machinery.SourceFileLoader("standalone_3dmark", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
standalone = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(SCRIPT.parent), *sys.path]):
    loader.exec_module(standalone)

REGISTRY_KEY = r"HKCU\Control Panel\Desktop\WindowMetrics"


def jks_utf(value):
    return struct.pack(">H", len(value)) + value


def local_keystore(certificates):
    encrypted_key = b"encrypted private key"
    fields = [
        struct.pack(">4I", 0xFEEDFEED, 2, 1, 1),
        jks_utf(b"choros"),
        struct.pack(">QI", 0, len(encrypted_key)),
        encrypted_key,
        struct.pack(">I", len(certificates)),
    ]
    for certificate in certificates:
        fields.extend([jks_utf(b"X.509"), struct.pack(">I", len(certificate)), certificate])
    fields.append(bytes(20))
    return b"".join(fields)


class Standalone3DMarkTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_automatic_live_backend(self):
        mode = self.root / "mode"
        mode.write_text("automatic\n")
        with patch.object(standalone, "MODE_FILE", mode):
            for resolved, expected in (("neptune", "d3dmetal"), ("mullion", "mullion"), ("venus", None)):
                with self.subTest(resolved=resolved), patch.object(
                        standalone.graphics, "effective_mode", return_value=resolved) as lookup:
                    self.assertEqual(standalone.live_backend(), expected)
                    lookup.assert_called_once_with("automatic", "223850")
            with patch.object(standalone.graphics, "DEFAULT_RESOLVE_SOCK", str(self.root / "missing")):
                self.assertIsNone(standalone.live_backend())

    def test_install_does_not_resolve_automatic_backend(self):
        installer = self.root / "data_x64.msi"
        installer.touch()
        mode = self.root / "mode"
        mode.write_text("automatic\n")
        with patch.object(sys, "argv", ["3dmark-standalone", "--install", str(installer)]), \
             patch.dict(standalone.os.environ, {"DISPLAY": ":0", "XDG_CURRENT_DESKTOP": "KDE"}), \
             patch.object(standalone.os, "umask"), \
             patch.object(Path, "home", return_value=self.root), \
             patch.object(standalone, "MODE_FILE", mode), \
             patch.object(standalone.graphics, "resolve_backend_for_app") as resolve, \
             patch.object(standalone, "wine_command", return_value=(["wine"], {})), \
             patch.object(standalone.subprocess, "run"), \
             patch.object(standalone, "install_app"):
            standalone.main()
        resolve.assert_not_called()

    def test_launch_resolves_automatic_backend_once(self):
        game = self.root / "3dmark-standalone/pfx/drive_c/Program Files/UL/3DMark"
        game.mkdir(parents=True)
        (game / "3DMark.exe").touch()
        mode = self.root / "mode"
        mode.write_text("automatic\n")
        with patch.object(sys, "argv", ["3dmark-standalone"]), \
             patch.dict(standalone.os.environ, {"DISPLAY": ":0", "XDG_CURRENT_DESKTOP": "KDE"}), \
             patch.object(standalone.os, "umask"), \
             patch.object(Path, "home", return_value=self.root), \
             patch.object(standalone, "MODE_FILE", mode), \
             patch.object(standalone.graphics, "resolve_backend_for_app", return_value="mullion") as resolve, \
             patch.object(standalone, "wine_command", return_value=(["wine"], {})) as wine, \
             patch.object(standalone, "browser_command", return_value=["browser"]), \
             patch.object(standalone, "open_desktop", return_value=["browser"]), \
             patch.object(standalone.os, "execvp"):
            standalone.main()
        resolve.assert_called_once_with("223850", sock_path=None, timeout=6.0)
        wine.assert_called_once_with(self.root / "3dmark-standalone", "mullion")

    def run_metrics_repair(self, metrics):
        registry = self.root / "registry.json"
        registry.write_text(json.dumps(metrics))
        reg_script = self.root / "wine.py"
        reg_script.write_text(
            "import json, sys\n"
            "from pathlib import Path\n"
            "registry = Path(sys.argv[1])\n"
            "command, action, key, *options = sys.argv[2:]\n"
            f"assert command == 'reg.exe' and key.casefold() == {REGISTRY_KEY!r}.casefold()\n"
            "metrics = json.loads(registry.read_text())\n"
            "if action == 'query':\n"
            "    names = [options[options.index('/v') + 1]] if '/v' in options else metrics\n"
            "    if not metrics or any(name not in metrics for name in names):\n"
            "        sys.exit(1)\n"
            "    for name in names:\n"
            "        print(f'    {name}    REG_SZ    {metrics[name]}')\n"
            "elif action == 'add':\n"
            "    name = options[options.index('/v') + 1]\n"
            "    metrics[name] = options[options.index('/d') + 1]\n"
            "    registry.write_text(json.dumps(metrics))\n"
            "else:\n"
            "    sys.exit(2)\n"
        )
        standalone.repair_window_metrics([sys.executable, str(reg_script), str(registry)])
        return json.loads(registry.read_text())

    def test_repairs_corrupt_window_metrics_in_launch_prefix(self):
        metrics = {"CaptionHeight": "6352142", "SmCaptionHeight": "6352142",
                   "MenuHeight": "12704094", "CaptionWidth": "6352142"}
        self.assertEqual(self.run_metrics_repair(metrics),
                         {"CaptionHeight": "25", "SmCaptionHeight": "17",
                          "MenuHeight": "18", "CaptionWidth": "6352142"})

    def test_leaves_custom_and_missing_window_metrics_alone(self):
        # Wine accepts negative twips values and positive pixel values.
        metrics = {"CaptionHeight": "31", "SmCaptionHeight": "-255"}
        self.assertEqual(self.run_metrics_repair(metrics), metrics)
        self.assertEqual(self.run_metrics_repair({}), {})

    def make_jar(self, keystore):
        jar = self.root / "choros.jar"
        with zipfile.ZipFile(jar, "w") as archive:
            archive.writestr("keystore.jks", keystore)
        return jar

    def test_reads_certificate_without_returning_private_key(self):
        certificate = b"\x30\x03\x02\x01\x01"
        jar = self.make_jar(local_keystore([certificate]))
        self.assertEqual(standalone.local_certificate(jar), certificate)

    def test_rejects_truncated_or_ambiguous_keystore(self):
        certificate = b"\x30\x03\x02\x01\x01"
        one_certificate = local_keystore([certificate])
        for keystore in (one_certificate[:-24], local_keystore([certificate, certificate])):
            with self.subTest(keystore=keystore):
                with self.assertRaises(ValueError):
                    standalone.local_certificate(self.make_jar(keystore))


if __name__ == "__main__":
    unittest.main()
