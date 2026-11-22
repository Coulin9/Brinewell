import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPTS = Path(__file__).resolve().parents[2] / "runtime/guest/rootfs/usr/lib/brinewell"
loader = importlib.machinery.SourceFileLoader("compat_migrate", str(SCRIPTS / "brinewell-compat-migrate"))
spec = importlib.util.spec_from_loader(loader.name, loader)
migrate = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(SCRIPTS), *sys.path]):
    loader.exec_module(migrate)
    import brinewell_vdf as vdf


def _config(entries):
    lines = ['"InstallConfigStore"', '{', '\t"Software"', '\t{', '\t\t"Valve"',
             '\t\t{', '\t\t\t"Steam"', '\t\t\t{', '\t\t\t\t"CompatToolMapping"',
             '\t\t\t\t{']
    for appid, fields in entries:
        lines.append(f'\t\t\t\t\t"{appid}"')
        lines.append('\t\t\t\t\t{')
        for key, value in fields.items():
            lines.append(f'\t\t\t\t\t\t"{key}"\t\t"{value}"')
        lines.append('\t\t\t\t\t}')
    lines += ['\t\t\t\t}', '\t\t\t}', '\t\t}', '\t}', '}', '']
    return "\n".join(lines)


def _vdf(tool_id, install):
    return ('"compatibilitytools"\n{\n\t"compat_tools"\n\t{\n'
            f'\t\t"{tool_id}"\n\t\t{{\n\t\t\t"install_path"\t\t"{install}"\n'
            '\t\t\t"display_name"\t\t"x"\n\t\t}\n\t}\n}\n')


def _vdf_no_install(tool_id):
    return ('"compatibilitytools"\n{\n\t"compat_tools"\n\t{\n'
            f'\t\t"{tool_id}"\n\t\t{{\n\t\t\t"display_name"\t\t"x"\n'
            '\t\t}\n\t}\n}\n')


class MigrateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.steam = Path(temporary.name).resolve() / "Steam"
        self.tools = self.steam / "compatibilitytools.d"
        self.tools.mkdir(parents=True)
        (self.steam / "config").mkdir()
        self.config = self.steam / "config/config.vdf"
        # Official base: Proton 11.0 without any compatibilitytool.vdf.
        self.base = self.steam / "steamapps/common/Proton 11.0"
        (self.base / "files/bin").mkdir(parents=True)
        (self.base / "proton").touch()

    def _write_config(self, entries):
        self.config.write_text(_config(entries))

    def _schema2(self, name="appledeck-neptune", tool_id="appledeck_neptune_proton",
                 runtime="runtimes/f3b0cd5a252c8e2581e29b1f", **marker_over):
        root = self.tools / name
        (root / runtime).mkdir(parents=True)
        (root / "compatibilitytool.vdf").write_text(_vdf(tool_id, runtime))
        marker = {"schema": 2, "base_key": "steam:4628710",
                  "base": str(self.base), "runtime": runtime,
                  "tool_id": tool_id, "base_id": "proton_11"}
        marker.update(marker_over)
        (root / runtime / "appledeck-neptune.json").write_text(json.dumps(marker))
        return root

    def _schema1(self, name="appledeck-neptune", tool_id="appledeck_neptune",
                 **marker_over):
        root = self.tools / name
        root.mkdir(parents=True)
        (root / "compatibilitytool.vdf").write_text(_vdf(tool_id, "."))
        marker = {"schema": 1, "base_key": "steam:4628710", "base": str(self.base)}
        marker.update(marker_over)
        (root / "appledeck-neptune.json").write_text(json.dumps(marker))
        return root

    def _run(self):
        with patch.dict(os.environ, {"BRINEWELL_STEAM_ROOT": str(self.steam)}):
            migrate.main()

    def test_schema2_rewrites_and_retires(self):
        self._schema2()
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
            ("814380", {"name": "appledeck_neptune_proton", "config": "x", "priority": "250"}),
            ("550", {"name": "proton_11-arm64", "config": "", "priority": "250"}),
        ])
        self._run()
        mapping = vdf.mappings(self.config.read_text())
        self.assertEqual(mapping["0"]["name"], "proton_11")
        self.assertEqual(mapping["814380"]["name"], "proton_11")
        self.assertEqual(mapping["814380"]["priority"], "250")
        self.assertEqual(mapping["550"]["name"], "proton_11-arm64")
        root = self.tools / "appledeck-neptune"
        self.assertFalse((root / "compatibilitytool.vdf").exists())
        self.assertTrue((root / "compatibilitytool.vdf.appledeck-retired").exists())

    def test_schema1_dot_install_root_marker(self):
        self._schema1()
        self._write_config([
            ("0", {"name": "appledeck_neptune", "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"], "proton_11")

    def test_schema2_root_marker_only_is_untrusted(self):
        root = self._schema2()
        # Marker at root instead of inside the runtime: schema2 never falls back.
        (root / "appledeck-neptune.json").write_text(
            (root / "runtimes/f3b0cd5a252c8e2581e29b1f/appledeck-neptune.json").read_text())
        (root / "runtimes/f3b0cd5a252c8e2581e29b1f/appledeck-neptune.json").unlink()
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"],
                         "appledeck_neptune_proton")
        self.assertTrue((root / "compatibilitytool.vdf").exists())

    def test_schema2_missing_base_id_skips_tool(self):
        self._schema2(base_id="")
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"],
                         "appledeck_neptune_proton")

    def test_missing_base_removes_only_owned_redirect(self):
        self._schema2(base=str(self.steam / "gone"))
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
            ("550", {"name": "proton_11-arm64", "config": "", "priority": "250"}),
        ])
        self._run()
        mapping = vdf.mappings(self.config.read_text())
        self.assertNotIn("0", mapping)
        self.assertEqual(mapping["550"]["name"], "proton_11-arm64")
        self.assertFalse((self.tools / "appledeck-neptune/compatibilitytool.vdf").exists())

    def test_bad_base_path_skips_without_touching_mappings(self):
        for bad in (123, "relative/path", str(self.tools / "appledeck-neptune" / "inner")):
            for path in (self.steam / "appledeck-retired",):
                pass
            root = self._schema2(name=f"appledeck-neptune-{abs(hash(bad)) % 99999}",
                                 base=bad)
            self._write_config([
                ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
            ])
            self._run()
            self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"],
                             "appledeck_neptune_proton", msg=f"base={bad!r}")
            self.assertTrue((root / "compatibilitytool.vdf").exists())
            import shutil
            shutil.rmtree(root)

    def test_malformed_marker_and_vdf_left_untouched(self):
        root = self._schema2()
        (root / "runtimes/f3b0cd5a252c8e2581e29b1f/appledeck-neptune.json").write_text("garbage{")
        other = self.tools / "appledeck-neptune-bad"
        other.mkdir()
        (other / "compatibilitytool.vdf").write_text("not vdf")
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"],
                         "appledeck_neptune_proton")
        self.assertTrue((root / "compatibilitytool.vdf").exists())

    def test_collision_with_dot_install_registration_blocks(self):
        self._schema2()
        # A third-party registration with '.' install_path but the same id.
        alien = self.tools / "imposter"
        alien.mkdir()
        (alien / "compatibilitytool.vdf").write_text(
            _vdf("appledeck_neptune_proton", "."))
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"],
                         "appledeck_neptune_proton")
        self.assertTrue((self.tools / "appledeck-neptune/compatibilitytool.vdf").exists())
        self.assertTrue((alien / "compatibilitytool.vdf").exists())

    def test_collision_with_symlinked_registration_blocks(self):
        self._schema2()
        real = Path(tempfile.mkdtemp()) / "linked"
        real.mkdir(parents=True)
        (real / "compatibilitytool.vdf").write_text(
            _vdf("appledeck_neptune_proton", "."))
        os.symlink(real, self.tools / "linked-tool")
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"],
                         "appledeck_neptune_proton")
        self.assertTrue((self.tools / "appledeck-neptune/compatibilitytool.vdf").exists())

    def test_symlinked_owned_root_left_untouched(self):
        real = Path(tempfile.mkdtemp()) / "appledeck-neptune"
        real.mkdir(parents=True)
        os.symlink(real, self.tools / "appledeck-neptune")
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"],
                         "appledeck_neptune_proton")

    def test_ge_key_alias_and_dir(self):
        ge_base = self.tools / "GE-Proton10-32-x86_64"
        ge_base.mkdir(parents=True)
        (ge_base / "runtime").mkdir()
        (ge_base / "proton").touch()
        (ge_base / "compatibilitytool.vdf").write_text(
            _vdf("GE-Proton10-32-x86_64", "runtime"))
        key = "ge:ge-proton10-32:x86_64"
        self._schema2(name="appledeck-neptune-ge-ge-proton10-32-x86_64",
                      tool_id="appledeck_neptune_ge_ge-proton10-32_x86_64",
                      base_key=key, base=str(ge_base),
                      tool_id_override=None,
                      base_id="GE-Proton10-32-x86_64")
        # schema2 marker tool_id must equal the GE alias (no _proton suffix)
        marker_path = (self.tools / "appledeck-neptune-ge-ge-proton10-32-x86_64"
                       / "runtimes/f3b0cd5a252c8e2581e29b1f/appledeck-neptune.json")
        marker = json.loads(marker_path.read_text())
        marker["tool_id"] = "appledeck_neptune_ge_ge-proton10-32_x86_64"
        marker_path.write_text(json.dumps(marker))
        self._write_config([
            ("42", {"name": "appledeck_neptune_ge_ge-proton10-32_x86_64",
                    "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["42"]["name"],
                         "GE-Proton10-32-x86_64")

    def test_atomic_failure_keeps_config_and_registration(self):
        root = self._schema2()
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        original = self.config.read_text()
        with patch.object(vdf, "atomic_write", side_effect=OSError("disk full")):
            self._run()
        self.assertEqual(self.config.read_text(), original)
        self.assertTrue((root / "compatibilitytool.vdf").exists())
        self.assertFalse((root / "compatibilitytool.vdf.appledeck-retired").exists())
        # Recoverable: the next run completes the migration.
        self._run()
        self.assertEqual(vdf.mappings(self.config.read_text())["0"]["name"], "proton_11")

    def test_idempotent_second_run(self):
        root = self._schema2()
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        self._run()
        first = self.config.read_text()
        listing = sorted(p.name for p in root.iterdir())
        with patch("sys.stderr", new_callable=io.StringIO) as err:
            self._run()
        self.assertNotIn(root.name, err.getvalue())
        self.assertEqual(self.config.read_text(), first)
        self.assertEqual(sorted(p.name for p in root.iterdir()), listing)

    def test_retired_name_collision_moves_root_out(self):
        root = self._schema2()
        (root / "compatibilitytool.vdf.appledeck-retired").write_text("x")
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
        ])
        self._run()
        self.assertFalse((self.tools / "appledeck-neptune").exists())
        self.assertTrue((self.steam / "appledeck-retired/appledeck-neptune").is_dir())

    def test_unknown_steam_files_untouched(self):
        self._schema2()
        other = self.tools / "GE-Proton9"
        other.mkdir()
        (other / "proton").touch()
        self._write_config([
            ("0", {"name": "appledeck_neptune_proton", "config": "", "priority": "75"}),
            ("99", {"name": "GE-Proton9", "config": "", "priority": "10"}),
        ])
        self._run()
        self.assertTrue((other / "proton").exists())
        self.assertEqual(vdf.mappings(self.config.read_text())["99"]["name"], "GE-Proton9")


if __name__ == "__main__":
    unittest.main()
