import importlib.machinery
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from test_graphics_wrap import SCRIPTS, _elf, _pe


loader = importlib.machinery.SourceFileLoader("shader_replay", str(SCRIPTS / "steam-shader-replay"))
spec = importlib.util.spec_from_loader(loader.name, loader)
replay = importlib.util.module_from_spec(spec)
loader.exec_module(replay)


class ShaderReplayTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)
        self.steam = self.home / ".local/share/Steam"
        self.library = self.home / "Shared Library"
        (self.steam / "config").mkdir(parents=True)
        (self.steam / "config/libraryfolders.vdf").write_text(
            f'"libraryfolders" {{ "1" {{ "path" "{self.library}" }} }}')
        (self.steam / "logs").mkdir()
        self.log = self.steam / "logs/compat_log.txt"
        self.log.write_text('[now] Client version: 1\n'
                            '[now] Registering tool proton-stable, AppID 42\n'
                            '[now] Mapping AppID 1174180 to tool "proton-stable" with priority 85\n')
        apps = self.library / "steamapps"
        tool = apps / "common/Proton"
        wine = tool / "files/lib/wine"
        (wine / "x86_64-unix").mkdir(parents=True)
        (wine / "x86_64-unix/ntdll.so").write_bytes(_elf(62))
        (wine / "dxvk/x86_64-windows").mkdir(parents=True)
        (wine / "dxvk/x86_64-windows/d3d11.dll").write_bytes(_pe(0x8664))
        (tool / "proton").touch()
        for app_id, name in (("42", "Proton"), ("1174180", "RDR2")):
            (apps / f"appmanifest_{app_id}.acf").write_text(f'"AppState" {{ "installdir" "{name}" }}')
        self.db = str(self.steam / "steamapps/shadercache/1174180/fozpipelinesv6/steam_pipeline_cache.foz")
        self.args = [self.db, "--master-process", "--shmem-fd", "131", "--control-fd", "164"]

    def test_prefix_and_replay_arguments(self):
        runtime = self.home / "run"
        generator = SCRIPTS.parent / "systemd/user-environment-generators/65-brinewell-fossilize"
        env = {"HOME": str(self.home), "XDG_RUNTIME_DIR": str(runtime)}
        setting = subprocess.check_output([generator], env=env, text=True).strip()
        key, prefix = setting.split("=", 1)
        self.assertEqual(key, "STEAM_FOSSILIZE_REPLAY_WRAP")
        binary = self.steam / "steamrtarm64/fossilize_replay"
        entry = f"{prefix}{self.steam}/steamrtarm64/../steamrtarm64/fossilize_replay"
        self.assertEqual(Path(entry).readlink(), Path("/usr/lib/brinewell/steam-shader-replay"))
        state = self.home / "graphics-backend"
        with patch.dict(os.environ, {key: prefix}), patch.object(replay, "MODE_FILE", state), \
                patch.object(os, "execv") as execute:
            for mode in ("neptune", "venus", "mullion"):
                state.write_text(mode)
                replay.main([entry, *self.args])
                path, args = execute.call_args.args
                self.assertEqual(path, binary)
                expected = [str(binary), *self.args]
                if mode == "neptune":
                    expected += ["--graphics-pipeline-range", "0", "0", "--compute-pipeline-range", "0", "0",
                                 "--raytracing-pipeline-range", "0", "0"]
                self.assertEqual(args, expected)
            state.unlink()
            replay.main([entry, *self.args])
            execute.assert_called_with(binary, [str(binary), *self.args])

    def test_auto_uses_the_game_launch_resolver(self):
        game = self.library / "steamapps/common/RDR2"
        with patch.object(replay.graphics, "resolve_backend_for_app", return_value="probe"), \
                patch.object(replay.graphics, "probe_local_backend", return_value="neptune") as probe:
            self.assertTrue(replay.skip_pipelines(self.steam, self.args, "automatic"))
            self.assertEqual(probe.call_args.args[1], game)
        with patch.object(replay.graphics, "resolve_backend_for_app", return_value="venus"):
            self.assertFalse(replay.skip_pipelines(self.steam, self.args, "automatic"))

    def test_native_unknown_and_arm_proton_pass_through(self):
        mapped = self.log.read_text()
        for change in ("Unmapping AppID 1174180", 'Mapping AppID 1174180 to tool "native" with priority 250',
                       "Client version: 2"):
            self.log.write_text(mapped + f"[now] {change}\n")
            self.assertFalse(replay.skip_pipelines(self.steam, self.args, "neptune"))
        self.log.write_text(mapped)
        with patch.object(replay.graphics, "select_arch", return_value="aarch64"):
            self.assertFalse(replay.skip_pipelines(self.steam, self.args, "neptune"))

    def test_local_proton_uses_registered_install_path(self):
        manifest = self.library / "steamapps/common/Proton/compatibilitytool.vdf"
        manifest.write_text('"compatibilitytools" { "compat_tools" { "GE-Proton" { "install_path" "." } } }')
        self.log.write_text(f'[now] Processing local tool list at {manifest}...\n'
                            '[now] Mapping AppID 1174180 to tool "GE-Proton" with priority 250\n')
        self.assertTrue(replay.skip_pipelines(self.steam, self.args, "neptune"))

    def test_ambiguous_and_non_master_jobs_pass_through(self):
        for args in (self.args[1:], [self.db], ["/some/cache.foz", *self.args],
                     [self.db.replace("1174180", "550"), *self.args]):
            self.assertFalse(replay.skip_pipelines(self.steam, args, "neptune"))
