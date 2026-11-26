import errno
import fcntl
import hashlib
import json
import os
import subprocess
import contextlib
import io
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

    def test_rdr2_seed_qualification_and_cleanup(self):
        self.assertEqual(game_patches.PATCHES["1174180"], "rdr2")
        environment = dict(self.environment, SteamAppId="1174180")
        prefix = Path(environment["STEAM_COMPAT_DATA_PATH"]) / "pfx"
        target = prefix / game_patches.RDR2_SETTINGS
        state = self.root / "records"
        (prefix / "drive_c/users/steamuser/Documents").mkdir(parents=True)
        with patch.object(game_patches, "patch_state_dir", return_value=state):
            for mode in ("neptune", "mullion"):
                for name in ("PlayRDR2.exe", "RDR2.exe"):
                    with self.subTest(mode=mode, name=name):
                        self.mode.write_text(mode)
                        command = self.command(name)
                        self.assertIsNotNone(game_patches.prepare("rdr2", command, environment, "")[2])
                        self.assertEqual(target.read_bytes(), game_patches.RDR2_SEED)
                        record = json.loads(next(state.glob("*.json")).read_text())
                        self.assertEqual(record["sha256"], hashlib.sha256(target.read_bytes()).hexdigest())
                        self.mode.write_text("venus")
                        self.assertIsNone(game_patches.prepare("rdr2", command, environment, "")[2])
                        self.assertFalse(target.exists())
            self.mode.write_text("neptune")
            target.write_text("<API>kSettingAPI_Vulkan</API>")
            command = self.command("RDR2.exe")
            game_patches.prepare("rdr2", command, environment, "")
            self.assertEqual(target.read_text(), "<API>kSettingAPI_Vulkan</API>")
            target.unlink()
            game_patches.prepare("rdr2", command, environment, "")
            target.write_text("game-saved settings")
            game_patches.prepare("rdr2", command, environment, "")
            self.mode.write_text("venus")
            game_patches.prepare("rdr2", command, environment, "")
            self.assertEqual(target.read_text(), "game-saved settings")
            target.unlink()
            self.mode.write_text("neptune")
            for flag in ("-sgadriver=vulkan", "-vulkan"):
                game_patches.prepare("rdr2", command, environment, "")
                self.assertTrue(target.exists())
                game_patches.prepare("rdr2", command + [flag], environment, "")
                self.assertFalse(target.exists())
                game_patches.prepare("rdr2", command + [flag.upper()], environment, "")
                self.assertFalse(target.exists())
            self.assertIsNone(game_patches.prepare("rdr2", self.command("other.exe"), environment, "")[2])

    def test_rdr2_rejects_escaping_symlink(self):
        environment = dict(self.environment, SteamAppId="1174180")
        prefix = Path(environment["STEAM_COMPAT_DATA_PATH"]) / "pfx"
        user = prefix / "drive_c/users/steamuser"
        user.mkdir(parents=True)
        outside = self.root / "outside"
        outside.mkdir()
        (user / "Documents").symlink_to(outside, target_is_directory=True)
        with patch.object(game_patches, "patch_state_dir", return_value=self.root / "records"):
            game_patches.prepare("rdr2", self.command("PlayRDR2.exe"), environment, "")
        self.assertEqual(list(outside.iterdir()), [])

    def test_rdr2_fresh_prefix_deferred_launch(self):
        environment = dict(self.environment, SteamAppId="1174180")
        prefix = Path(environment["STEAM_COMPAT_DATA_PATH"]) / "pfx"
        state = self.root / "records"
        command = self.command("RDR2.exe")
        with patch.object(game_patches, "patch_state_dir", return_value=state):
            _, _, identity = game_patches.prepare("rdr2", command, environment, "")
        self.assertFalse(prefix.exists())
        self.assertEqual(identity["deferred_seed"]["prefix"], str(prefix.resolve()))
        proton = self.tool / "proton"
        proton.write_text("#!/usr/bin/env python3\nimport os, pathlib, time\n"
                          "p = pathlib.Path(os.environ['STEAM_COMPAT_DATA_PATH']) / 'pfx/drive_c/users/steamuser/Documents'\n"
                          "p.mkdir(parents=True)\n"
                          "time.sleep(1.5)\n"
                          "assert (p / 'Rockstar Games/Red Dead Redemption 2/Settings/system.xml').is_file()\n")
        env = dict(os.environ, **environment)
        result = subprocess.run([sys.executable, "-c",
            "import sys; sys.path.insert(0, sys.argv[1]); import game_patches; "
            "game_patches.MODE_FILE=__import__('pathlib').Path(sys.argv[2]); "
            "game_patches.patch_state_dir=lambda _: __import__('pathlib').Path(sys.argv[3]); "
            "import importlib.machinery; m=importlib.machinery.SourceFileLoader('launch', sys.argv[1]+'/brinewell-game-launch').load_module(); "
            "m.Path=lambda _: type('Cmdline', (), {'read_text': lambda self: ''})(); "
            "sys.argv=['brinewell-game-launch','--profile','rdr2','--']+sys.argv[4:]; sys.exit(m.main())",
            str(SCRIPTS), str(self.mode), str(state), str(proton), "run", command[-1]],
            env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((prefix / game_patches.RDR2_SETTINGS).read_bytes(), game_patches.RDR2_SEED)

    def test_rdr2_cleanup_triggers_and_same_bytes_replacement(self):
        environment = dict(self.environment, SteamAppId="1174180")
        prefix = Path(environment["STEAM_COMPAT_DATA_PATH"]) / "pfx"
        (prefix / "drive_c/users/steamuser/Documents").mkdir(parents=True)
        target = prefix / game_patches.RDR2_SETTINGS
        command = self.command("RDR2.exe")
        state = self.root / "records"
        with patch.object(game_patches, "patch_state_dir", return_value=state):
            for mode, env, flags in [("venus", {}, []), ("neptune", {"WINEDLLOVERRIDES": "d3d12=n"}, []),
                                     ("neptune", {}, ["-Vulkan"]), ("neptune", {}, ["-SgaDriver=Vulkan"]),
                                     ("neptune", {}, ["-SgaDriver", "Vulkan"])]:
                with self.subTest(mode=mode, env=env, flags=flags):
                    self.mode.write_text("neptune")
                    game_patches.prepare("rdr2", command, environment, "")
                    self.assertTrue(target.exists())
                    self.mode.write_text(mode)
                    game_patches.prepare("rdr2", command + flags, dict(environment, **env), "")
                    self.assertFalse(target.exists())
            self.mode.write_text("neptune")
            game_patches.prepare("rdr2", command, environment, "")
            replacement = target.with_name("replacement")
            replacement.write_bytes(target.read_bytes())
            replacement.replace(target)
            self.mode.write_text("venus")
            game_patches.prepare("rdr2", command, environment, "")
            self.assertEqual(target.read_bytes(), game_patches.RDR2_SEED)
            self.assertFalse(list(state.glob("*.json")))

    def test_rdr2_cleanup_retries_after_transient_error(self):
        prefix = self.root / "pfx"
        (prefix / "drive_c/users/steamuser/Documents").mkdir(parents=True)
        state = self.root / "records"
        game_patches.seed_rdr2_settings(prefix, state)
        target = prefix / game_patches.RDR2_SETTINGS
        record = next(state.glob("*.json"))
        original_unlink = os.unlink

        def fail_target(path, *args, **kwargs):
            if path == "system.xml":
                raise OSError(errno.EIO, "temporary failure")
            return original_unlink(path, *args, **kwargs)

        with patch.object(game_patches.os, "unlink", side_effect=fail_target):
            with self.assertRaises(OSError):
                game_patches.cleanup_rdr2_settings(prefix, state)
        self.assertTrue(record.exists())
        self.assertEqual(target.read_bytes(), game_patches.RDR2_SEED)
        game_patches.cleanup_rdr2_settings(prefix, state)
        self.assertFalse(target.exists())
        self.assertFalse(record.exists())

    def test_rdr2_cleanup_preserves_fifo_without_blocking(self):
        prefix = self.root / "pfx"
        (prefix / "drive_c/users/steamuser/Documents").mkdir(parents=True)
        state = self.root / "records"
        game_patches.seed_rdr2_settings(prefix, state)
        target = prefix / game_patches.RDR2_SETTINGS
        target.unlink()
        os.mkfifo(target)
        game_patches.cleanup_rdr2_settings(prefix, state)
        self.assertTrue(target.exists())
        self.assertFalse(list(state.glob("*.json")))

    def test_deferred_seed_locked_state_does_not_delay_child_exit(self):
        prefix = self.root / "pfx"
        (prefix / "drive_c/users/steamuser/Documents").mkdir(parents=True)
        state = self.root / "records"
        state.mkdir()
        with (state / "files.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            result = subprocess.run([sys.executable, "-c",
                "import sys, importlib.machinery; sys.path.insert(0, sys.argv[1]); "
                "m=importlib.machinery.SourceFileLoader('launch', sys.argv[1]+'/brinewell-game-launch').load_module(); "
                "sys.exit(m.run_with_deferred_seed([sys.executable, '-c', 'import time; time.sleep(0.2); exit(7)'], "
                "__import__('os').environ, {'prefix':sys.argv[2], 'state':sys.argv[3]}))",
                str(SCRIPTS), str(prefix), str(state)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertFalse((prefix / game_patches.RDR2_SETTINGS).exists())

    def test_rdr2_seed_fallback_preserves_file_if_record_fails(self):
        prefix = self.root / "pfx"
        (prefix / "drive_c/users/steamuser/Documents").mkdir(parents=True)
        state = self.root / "records"
        real_write_record = game_patches.write_record
        calls = []

        def write_record(path, record):
            calls.append(record)
            if len(calls) == 2:
                raise OSError(errno.EIO, "record failure")
            real_write_record(path, record)

        with patch.object(game_patches.os, "link", side_effect=OSError(errno.EPERM, "no links")), \
             patch.object(game_patches, "write_record", side_effect=write_record):
            with self.assertRaises(OSError):
                game_patches.seed_rdr2_settings(prefix, state)
        self.assertEqual((prefix / game_patches.RDR2_SETTINGS).read_bytes(), game_patches.RDR2_SEED)

    def test_rdr2_internal_symlink_and_io_errors_do_not_block(self):
        environment = dict(self.environment, SteamAppId="1174180")
        prefix = Path(environment["STEAM_COMPAT_DATA_PATH"]) / "pfx"
        documents = prefix / "drive_c/users/steamuser/Documents"
        settings = documents / "Rockstar Games/Red Dead Redemption 2/Settings"
        settings.parent.mkdir(parents=True)
        destination = prefix / "elsewhere"
        destination.mkdir()
        settings.symlink_to(destination, target_is_directory=True)
        command = self.command("RDR2.exe")
        with patch.object(game_patches, "patch_state_dir", return_value=self.root / "records"):
            with contextlib.redirect_stderr(io.StringIO()) as stderr:
                self.assertIsNotNone(game_patches.prepare("rdr2", command, environment, "")[2])
            self.assertIn("RDR2 seed failed", stderr.getvalue())
            self.assertEqual(list(destination.iterdir()), [])
            settings.unlink()
            settings.write_text("not a directory")
            with contextlib.redirect_stderr(io.StringIO()) as stderr:
                self.assertIsNotNone(game_patches.prepare("rdr2", command, environment, "")[2])
            self.assertIn("RDR2 seed failed", stderr.getvalue())

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
