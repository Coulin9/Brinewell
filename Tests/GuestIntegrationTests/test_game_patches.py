import importlib.machinery
import struct
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPTS = Path(__file__).resolve().parents[2] / "runtime/guest/rootfs/usr/lib/brinewell"
with patch.object(sys, "path", [str(SCRIPTS), *sys.path]):
    import game_patches
    loader = importlib.machinery.SourceFileLoader("game_launch", str(SCRIPTS / "brinewell-game-launch"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    game_launch = importlib.util.module_from_spec(spec)
    loader.exec_module(game_launch)


class GamePatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.mode = self.root / "graphics-backend"
        self.mode.write_text("neptune\n")
        patcher = patch.object(game_patches, "MODE_FILE", self.mode)
        self.addCleanup(patcher.stop)
        patcher.start()
        self.install = self.root / "Steam Library/Game"
        self.install.mkdir(parents=True)
        self.environment = {
            "SteamAppId": "1091500",
            "STEAM_COMPAT_INSTALL_PATH": str(self.install),
            "STEAM_COMPAT_DATA_PATH": str(self.root / "compatdata/1091500"),
            "STEAM_COMPAT_TOOL_PATHS": str(self.root / "Steam/compatibilitytools.d/proton-11"),
        }
        self.tool = self.root / "Steam/compatibilitytools.d/proton-11"
        self.tool.mkdir(parents=True)
        (self.tool / "proton").touch()
        (self.tool / "proton").chmod(0o755)
        wine = self.tool / "files/lib/wine"
        unix = wine / "x86_64-unix"
        unix.mkdir(parents=True)
        elf = bytearray(64)
        elf[:4] = b"\x7fELF"
        struct.pack_into("<H", elf, 18, 62)
        (unix / "ntdll.so").write_bytes(bytes(elf))
        dxvk = wine / "dxvk/x86_64-windows"
        dxvk.mkdir(parents=True)
        pe = bytearray(512)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 0x3C, 0x80)
        pe[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<H", pe, 0x84, 0x8664)
        (dxvk / "d3d11.dll").write_bytes(bytes(pe))

    def command(self, name, *arguments):
        executable = self.install / name
        executable.touch()
        return ["FEX", "waitforexitandrun", "--", "runtime", "--verb=run", "--",
                str(self.tool / "proton"),
                "waitforexitandrun", str(executable), *arguments]

    def test_automatic_only_applies_resolved_neptune_patches(self):
        self.mode.write_text("automatic\n")
        command = self.command("REDprelauncher.exe")
        for backend, expected in (("neptune", True), ("mullion", True), ("venus", False)):
            with self.subTest(backend=backend), patch.object(
                    game_patches.graphics, "resolve_backend_for_app", return_value=backend):
                changed, _, identity = game_patches.prepare(
                    "cyberpunk2077", command, self.environment, "")
                self.assertEqual(identity is not None, expected)
                self.assertEqual("--launcher-skip" in changed, expected)

    def test_automatic_probe_after_qualification(self):
        self.mode.write_text("automatic\n")
        command = self.command("REDprelauncher.exe")
        with patch.object(game_patches.graphics, "resolve_backend_for_app", return_value="probe"), \
             patch.object(game_patches.graphics, "probe_local_backend", return_value="neptune") as probe:
            changed, _, identity = game_patches.prepare("cyberpunk2077", command, self.environment, "")
        self.assertIsNotNone(identity)
        self.assertIn("--launcher-skip", changed)
        self.assertEqual(probe.call_args.args[:2], (command[-1], str(self.install)))

    def test_automatic_runinprefix_never_resolves(self):
        self.mode.write_text("automatic\n")
        command = self.command("REDprelauncher.exe")
        command[-2] = "runinprefix"
        with patch.object(game_patches.graphics, "resolve_backend_for_app") as resolve:
            changed, environment, identity = game_patches.prepare(
                "cyberpunk2077", command, self.environment, "")
        resolve.assert_not_called()
        self.assertEqual((changed, environment, identity), (command, self.environment, None))

    def test_automatic_sekiro_launcher_never_connects_to_resolve_socket(self):
        self.mode.write_text("automatic\n")
        command = self.command("launcher.exe")
        environment = dict(self.environment, SteamAppId="814380")
        with patch.object(game_patches.graphics.socket, "socket") as socket:
            changed, actual_environment, identity = game_patches.prepare(
                "sekiro", command, environment, "")
        socket.assert_not_called()
        self.assertEqual((changed, actual_environment, identity), (command, environment, None))

    def test_cyberpunk_only_changes_game_arguments(self):
        command = self.command("REDprelauncher.exe", "--intro-skip")
        changed, environment, identity = game_patches.prepare(
            "cyberpunk2077", command, self.environment, "brinewell.gfx=neptune")
        self.assertEqual(changed, command + ["--launcher-skip"])
        self.assertEqual(environment, self.environment)
        self.assertEqual(identity["appid"], "1091500")
        changed, _, _ = game_patches.prepare(
            "1091500", changed, self.environment, "brinewell.gfx=neptune")
        self.assertEqual(changed.count("--launcher-skip"), 1)

    def test_wrong_app_backend_or_proton_leaves_original_command(self):
        command = self.command("REDprelauncher.exe")
        for environment, cmdline, candidate in (
                (dict(self.environment, SteamAppId="42"), "brinewell.gfx=neptune", command),
                (self.environment, "brinewell.gfx=venus", command),
                (self.environment, "brinewell.gfx=neptune", ["stock-proton", "run", command[-1]])):
            if cmdline == "brinewell.gfx=venus":
                self.mode.write_text("venus\n")
            else:
                self.mode.write_text("neptune\n")
            with self.subTest(environment=environment, cmdline=cmdline, candidate=candidate):
                changed, actual_env, identity = game_patches.prepare(
                    "cyberpunk2077", candidate, environment, cmdline)
                self.assertEqual(changed, candidate)
                self.assertEqual(actual_env, environment)
                self.assertIsNone(identity)

    def test_outer_steam_compatibility_app_id_is_supported(self):
        command = self.command("REDprelauncher.exe")
        environment = dict(self.environment, STEAM_COMPAT_APP_ID="1091500")
        environment.pop("SteamAppId")
        changed, _, identity = game_patches.prepare(
            "cyberpunk2077", command, environment, "brinewell.gfx=neptune")
        self.assertEqual(changed, command + ["--launcher-skip"])
        self.assertEqual(identity["appid"], "1091500")
        environment["SteamAppId"] = "42"
        changed, _, identity = game_patches.prepare(
            "cyberpunk2077", command, environment, "brinewell.gfx=neptune")
        self.assertEqual(changed, command)
        self.assertIsNone(identity)

    def test_non_game_verbs_and_external_executable_are_untouched(self):
        command = self.command("REDprelauncher.exe")
        command[-2] = "getcompatpath"
        self.assertIsNone(game_patches.prepare(
            "cyberpunk2077", command, self.environment, "brinewell.gfx=neptune")[2])
        outside = self.root / "REDprelauncher.exe"
        outside.touch()
        command[-2:] = ["run", str(outside)]
        self.assertIsNone(game_patches.prepare(
            "cyberpunk2077", command, self.environment, "brinewell.gfx=neptune")[2])

    def test_3dmark_preserves_explicit_configuration(self):
        environment = game_patches.three_dmark_env({"JAVA_TOOL_OPTIONS": "-Dfile.encoding=UTF-8"})
        self.assertEqual(environment["JAVA_TOOL_OPTIONS"],
                         "-Dfile.encoding=UTF-8 -Dsun.java2d.d3d=false -Xint")
        self.assertEqual(game_patches.three_dmark_env(environment), environment)
        self.assertEqual(environment["FEX_DISKCACHE"], "0")
        self.assertEqual(game_patches.three_dmark_arguments([]),
                         ["--disable-gpu", "--systeminfo=off", "--gpuCount=1"])
        explicit = ["--systeminfo=on", "--gpuCount", "2"]
        self.assertEqual(game_patches.three_dmark_arguments(explicit), explicit + ["--disable-gpu"])
        repeated = ["--systeminfo=on", "--systeminfo=off"]
        self.assertEqual(game_patches.three_dmark_arguments(repeated),
                         repeated + ["--disable-gpu", "--gpuCount=1"])

    def test_compiler_updates_and_removes_only_our_unchanged_copy(self):
        source = self.root / "compiler.dll"
        target = self.install / "d3dcompiler_47.dll"
        state = self.root / "records"
        source.write_bytes(b"version one")
        game_patches.ensure_compiler(source, target, state)
        self.assertEqual(target.read_bytes(), b"version one")
        source.write_bytes(b"version two")
        game_patches.ensure_compiler(source, target, state)
        self.assertEqual(target.read_bytes(), b"version two")
        game_patches.remove_owned_files(state)
        self.assertFalse(target.exists())
        game_patches.ensure_compiler(source, target, state)
        target.write_bytes(b"user file")
        game_patches.remove_owned_files(state)
        self.assertEqual(target.read_bytes(), b"user file")

    def test_existing_compiler_is_preserved(self):
        source = self.root / "compiler.dll"
        target = self.install / "d3dcompiler_47.dll"
        source.write_bytes(b"bundled")
        target.write_bytes(b"existing")
        game_patches.ensure_compiler(source, target, self.root / "records")
        self.assertEqual(target.read_bytes(), b"existing")

    def test_nightreign_returns_launch_specific_identity(self):
        command = self.command("start_protected_game.exe")
        environment = dict(self.environment, SteamAppId="2622380")
        actual, _, identity = game_patches.prepare(
            "nightreign", command, environment, "brinewell.gfx=neptune")
        self.assertEqual(actual, command)
        self.assertTrue(identity["affinity"])
        self.assertEqual(identity["processes"], ("nightreign.exe", "start_protected"))
        self.assertEqual(identity["game"], "nightreign.exe")
        self.assertEqual(identity["prefix"], str(Path(self.root / "compatdata/1091500/pfx").resolve()))

    def test_sekiro_returns_launch_specific_identity(self):
        command = self.command("sekiro.exe")
        environment = dict(self.environment, SteamAppId="814380")
        actual, _, identity = game_patches.prepare(
            "sekiro", command, environment, "brinewell.gfx=neptune")
        self.assertEqual(actual, command)
        self.assertTrue(identity["affinity"])
        self.assertEqual(identity["processes"], ("sekiro.exe",))
        self.assertEqual(identity["game"], "sekiro.exe")
        self.assertEqual(identity["prefix"], str(Path(self.root / "compatdata/1091500/pfx").resolve()))

    def test_sekiro_rejects_other_executables(self):
        command = self.command("launcher.exe")
        environment = dict(self.environment, SteamAppId="814380")
        actual, _, identity = game_patches.prepare(
            "sekiro", command, environment, "brinewell.gfx=neptune")
        self.assertEqual(actual, command)
        self.assertIsNone(identity)

    def test_affinity_members_follow_this_launch_and_reject_reused_pids(self):
        scope = "0::/user.slice/app-steam-app2622380-101.scope\n"
        processes = {
            101: {"parent": 1, "started": 40, "group": scope},
            102: {"parent": 101, "started": 41, "group": scope},
            103: {"parent": 102, "started": 42, "group": scope},
            201: {"parent": 1, "started": 43, "group": "0::/other.scope\n"},
        }
        members, scopes = game_launch.collect_members(processes, {101: 40}, set(), "2622380")
        self.assertEqual(members, {101: 40, 102: 41, 103: 42})
        self.assertEqual(scopes, {scope})
        members, scopes = game_launch.collect_members(processes, {101: 12}, set(), "2622380")
        self.assertEqual(members, {})
        self.assertEqual(scopes, set())

    def test_affinity_members_sekiro_scope(self):
        scope = "0::/user.slice/app-steam-app814380-201.scope\n"
        processes = {
            201: {"parent": 1, "started": 40, "group": scope},
            202: {"parent": 201, "started": 41, "group": scope},
        }
        members, scopes = game_launch.collect_members(processes, {201: 40}, set(), "814380")
        self.assertEqual(members, {201: 40, 202: 41})
        self.assertEqual(scopes, {scope})

    def test_slow_launcher_keeps_handoff_open(self):
        self.assertEqual(game_launch.watch_finished(False, False, False, True, 120, 32),
                         (False, None))
        self.assertEqual(game_launch.watch_finished(False, False, False, False, 120, None),
                         (False, 150))
        self.assertEqual(game_launch.watch_finished(False, False, False, False, 149, 150),
                         (False, 150))
        self.assertEqual(game_launch.watch_finished(False, False, False, False, 150, 150),
                         (True, 150))

    def test_game_completion_does_not_wait_for_background_wine_services(self):
        self.assertEqual(game_launch.watch_finished(False, True, False, False, 40, None),
                         (True, None))
        self.assertEqual(game_launch.watch_finished(False, True, True, False, 40, None),
                         (False, None))
        self.assertEqual(game_launch.watch_finished(True, True, False, False, 40, None),
                         (False, None))
        self.assertIn("wineserver", game_launch.WINE_SERVICES)
        self.assertIn("services.exe", game_launch.WINE_SERVICES)


if __name__ == "__main__":
    unittest.main()
