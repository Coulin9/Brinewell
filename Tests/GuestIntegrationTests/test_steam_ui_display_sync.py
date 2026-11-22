import importlib.machinery
import importlib.util
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "runtime/guest/rootfs/usr/lib/brinewell/steam-ui-display-sync"
loader = importlib.machinery.SourceFileLoader("steam_ui_display_sync", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
sync = importlib.util.module_from_spec(spec)
loader.exec_module(sync)


class SteamUIDisplaySyncTests(unittest.TestCase):
    def value(self, text):
        values, _, _ = sync.vdf.vdf_entries(text)
        return values[tuple(part.lower() for part in sync.SETTING)][0]

    def test_first_start_creates_native_resolution_setting(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            result = subprocess.run(["python3", str(SCRIPT)], env={**os.environ, "HOME": str(home)},
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            config = home / ".local/share/Steam/config/config.vdf"
            self.assertEqual(self.value(config.read_text()), "1")

    def test_enables_existing_false_value_and_preserves_other_preferences(self):
        text = ('// display preferences\n"InstallConfigStore" { "Gamescope" { '
                '"EnableSteamNativeExternalResolution" "0" "Other" "leave me" } '
                '"UI" { "display" { "ScaleFactor" "1.35" } } '
                '"GamescopeGameResolutionGlobal" "3840x2160" }\n')
        expected = text.replace('"EnableSteamNativeExternalResolution" "0"',
                                '"EnableSteamNativeExternalResolution" "1"')
        self.assertEqual(sync.enable_native_resolution(text), expected)

    def test_missing_gamescope_block_preserves_unrelated_text(self):
        text = '"InstallConfigStore" { "Software" { "Valve" { "Steam" { "Unrelated" "yes" } } } }\n'
        changed = sync.enable_native_resolution(text)
        self.assertEqual(self.value(changed), "1")
        self.assertIn('"Unrelated" "yes"', changed)
        self.assertEqual(sync.enable_native_resolution(changed), changed)

    def test_enabled_setting_is_case_insensitive_and_not_rewritten(self):
        text = '"installconfigstore" { "GAMESCOPE" { "enablesteamnativeexternalresolution" "1" } }\n'
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config.vdf"
            config.write_text(text)
            original = config.stat()
            self.assertFalse(sync.configure(config))
            self.assertEqual(config.read_text(), text)
            self.assertEqual(config.stat().st_mtime_ns, original.st_mtime_ns)
            self.assertEqual(config.stat().st_ino, original.st_ino)

    def test_rewrite_preserves_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config.vdf"
            config.write_text('"InstallConfigStore" {}\n')
            config.chmod(0o640)
            self.assertTrue(sync.configure(config))
            self.assertEqual(stat.S_IMODE(config.stat().st_mode), 0o640)

    def test_malformed_or_ambiguous_config_is_never_overwritten(self):
        for text in ['"InstallConfigStore" {',
                     '"OtherStore" {}',
                     '"InstallConfigStore" { "Gamescope" { "EnableSteamNativeExternalResolution" "0" '
                     '"EnableSteamNativeExternalResolution" "1" } }']:
            with self.subTest(text=text), tempfile.TemporaryDirectory() as temporary:
                config = Path(temporary) / "config.vdf"
                config.write_text(text)
                with self.assertRaises(ValueError):
                    sync.configure(config)
                self.assertEqual(config.read_text(), text)

    def test_service_runs_sync_before_steam_and_tolerates_display_config_failure(self):
        unit = ROOT / "runtime/guest/rootfs/usr/lib/systemd/user/steam.service.d/50-brinewell.conf"
        self.assertIn("ExecStartPre=-/usr/lib/brinewell/steam-ui-display-sync\n", unit.read_text())


if __name__ == "__main__":
    unittest.main()
