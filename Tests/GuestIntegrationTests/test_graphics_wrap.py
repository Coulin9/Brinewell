import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPTS = Path(__file__).resolve().parents[2] / "runtime/guest/rootfs/usr/lib/brinewell"
loader = importlib.machinery.SourceFileLoader("graphics_wrap", str(SCRIPTS / "brinewell-graphics-wrap"))
spec = importlib.util.spec_from_loader(loader.name, loader)
wrap = importlib.util.module_from_spec(spec)
loader.exec_module(wrap)
graphics = wrap.graphics


def _elf(machine):
    data = bytearray(64)
    data[:4] = b"\x7fELF"
    struct.pack_into("<H", data, 18, machine)
    return bytes(data)


def _pe(machine):
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, machine)
    return bytes(data)


class GraphicsWrapTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        # Real SLR layout: <slr>/pressure-vessel-arm64/{bin,libexec/...}
        self.slr = self.root / "slr4"
        (self.slr / "pressure-vessel-arm64/bin").mkdir(parents=True)
        (self.slr / "pressure-vessel-arm64/bin/pressure-vessel-wrap").touch()
        self.bwrap = self.slr / "pressure-vessel-arm64/libexec/steam-runtime-tools-0/srt-bwrap"
        self.bwrap.parent.mkdir(parents=True)
        self.bwrap.touch()
        self.bwrap.chmod(0o755)
        self.pv = self.slr / "pressure-vessel-arm64/libexec/steam-runtime-tools-0/pv-adverb"
        self.pv.touch()
        # Selected tool: STEAM_COMPAT_TOOL_PATHS names the tool dir itself.
        self.tool = self.root / "common/Proton 11.0"
        self._make_tool(self.tool, "x86_64")
        # Packaged read-only payload + staged bridge symlinks (real links).
        self.payload = self.root / "payload"
        for arch in ("x86_64", "aarch64"):
            for sub in (f"{arch}-windows", f"{arch}-unix", f"bridge/{arch}"):
                (self.payload / sub).mkdir(parents=True)
            for name in ("d3d11", "dxgi", "d3d12", "nptunix"):
                (self.payload / f"{arch}-windows/{name}.dll").write_bytes(("p" + name).encode())
            (self.payload / f"{arch}-unix/nptunix.so").write_bytes(b"so")
            for suffix in ("dll", "so"):
                os.symlink(f"../../{arch}-{'windows' if suffix == 'dll' else 'unix'}/nptunix.{suffix}",
                           self.payload / f"bridge/{arch}/nptunix.{suffix}")
        # Steam creates STEAM_COMPAT_DATA_PATH before launch.
        self.compat = self.root / "compat/814380"
        self.compat.mkdir(parents=True)
        self.execed = []
        self.addCleanup(patch.stopall)
        patch.object(graphics, "multi_ring_opt_in", lambda: False).start()
        # Existing tests must never see a real home-dir dev payload.
        self.dev = self.root / "neptune-dev"
        patch.object(wrap, "DEV_PAYLOAD", self.dev).start()

    def _make_tool(self, tool, arch):
        tool.mkdir(parents=True, exist_ok=True)
        (tool / "proton").touch()
        (tool / "proton").chmod(0o755)
        wine = tool / "files/lib/wine"
        unix = wine / f"{arch}-unix"
        unix.mkdir(parents=True, exist_ok=True)
        (unix / "ntdll.so").write_bytes(_elf(62 if arch == "x86_64" else 183))
        dxvk = wine / "dxvk" / f"{arch}-windows"
        dxvk.mkdir(parents=True, exist_ok=True)
        (dxvk / "d3d11.dll").write_bytes(_pe(0x8664))
        (dxvk / "dxgi.dll").write_bytes(_pe(0x8664))
        vkd3d = wine / "vkd3d-proton" / f"{arch}-windows"
        vkd3d.mkdir(parents=True, exist_ok=True)
        (vkd3d / "d3d12.dll").write_bytes(_pe(0x8664))
        nvapi = wine / "nvapi" / f"{arch}-windows"
        nvapi.mkdir(parents=True, exist_ok=True)
        (nvapi / "nvapi64.dll").write_bytes(_pe(0x8664) + b"stock-nvapi64")

    def _exec(self, path, args):
        self.execed.append((path, list(args)))

    def _env(self, tool, extra=None):
        return {"STEAM_COMPAT_TOOL_PATHS": str(tool),
                "STEAM_COMPAT_APP_ID": "814380",
                "STEAM_COMPAT_DATA_PATH": str(self.compat),
                **(extra or {})}

    def run_wrap(self, cmd, env=None, mode="venus", tool=None, fex=True):
        tool = tool or self.tool
        argv = ["--args", "9", str(self.pv), "--opt=x", "--"]
        if fex:
            argv += ["FEXCompatTool"]
        argv += [str(tool / "proton"), *cmd]
        merged = self._env(tool, env)
        with patch.object(wrap, "_mode", lambda: mode), \
             patch.object(wrap, "PAYLOAD", self.payload), \
             patch.object(wrap, "CONTAINER_PAYLOAD",
                          Path("/run/host") / self.payload.relative_to(self.root)), \
             patch.object(wrap, "_delegate",
                          lambda: str(self.bwrap)), \
             patch.object(graphics, "PAYLOAD", self.payload), \
             patch.object(wrap.os, "execv", self._exec), \
             patch.dict(os.environ, merged, clear=True):
            wrap.main(argv)
        return self.execed[-1][1][1:]

    def game(self, verb="waitforexitandrun", mode="venus", **kw):
        return self.run_wrap([verb, "/game/game.exe", "--opt"], mode=mode, **kw)

    # --- delegate resolution ---

    def test_delegate_from_pressure_vessel_parent(self):
        exe = self.slr / "pressure-vessel-arm64/bin/pressure-vessel-wrap"
        self.assertEqual(wrap._delegate(exe), str(self.bwrap))

    def test_delegate_rejects_arg_derived_paths(self):
        # An argv-named pv-adverb sibling is never used as the delegate.
        fake = self.root / "fake/libexec/steam-runtime-tools-0"
        fake.mkdir(parents=True)
        (fake / "srt-bwrap").touch()
        exe = self.slr / "pressure-vessel-arm64/bin/pressure-vessel-wrap"
        self.assertEqual(wrap._delegate(exe), str(self.bwrap))
        self.assertNotEqual(wrap._delegate(exe), str(fake / "srt-bwrap"))

    def test_delegate_missing_for_real_pv_parent_fails(self):
        broken = self.root / "slr4-empty"
        (broken / "pressure-vessel-arm64/bin").mkdir(parents=True)
        (broken / "pressure-vessel-arm64/bin/pressure-vessel-wrap").touch()
        self.assertIsNone(wrap._delegate(
            broken / "pressure-vessel-arm64/bin/pressure-vessel-wrap"))

    # --- passthrough ---

    def test_venus_passes_tail_and_args_exact(self):
        argv = self.game()
        self.assertEqual(self.execed[-1][0], str(self.bwrap))
        # Venus injects nothing: the original argv passes through untouched.
        self.assertEqual(argv, ["--args", "9", str(self.pv), "--opt=x", "--",
                                "FEXCompatTool", str(self.tool / "proton"),
                                "waitforexitandrun", "/game/game.exe", "--opt"])

    def test_non_game_and_probe_verbs_pass_byte_identical(self):
        for tail in (["uname", "-a"], ["getcompatpath", "/game"],
                     ["runinprefix", "x"], ["--verb=run"]):
            argv = ["--args", "9", str(self.pv), "--opt=x", "--", *tail]
            with patch.object(wrap, "_mode", lambda: "neptune"), \
                 patch.object(wrap, "_delegate", lambda: str(self.bwrap)), \
                 patch.object(graphics, "PAYLOAD", self.payload), \
                 patch.object(wrap.os, "execv", self._exec), \
                 patch.dict(os.environ, self._env(self.tool), clear=True):
                wrap.main(argv)
            self.assertEqual(self.execed[-1][1], [str(self.bwrap), *argv])
        # argv without the recognized boundary also passes byte-identical.
        argv = ["--args", "9", str(self.pv)]
        with patch.object(wrap, "_delegate", lambda: str(self.bwrap)), \
             patch.object(wrap.os, "execv", self._exec), \
             patch.dict(os.environ, {}, clear=True):
            wrap.main(argv)
        self.assertEqual(self.execed[-1][1], [str(self.bwrap), *argv])

    def test_missing_identity_or_tool_membership_passthrough(self):
        # Full env control (no defaults): missing AppID, non-numeric AppID,
        # missing compatdata, and tool not an exact TOOL_PATHS member.
        full_envs = [
            {"STEAM_COMPAT_TOOL_PATHS": str(self.tool),
             "STEAM_COMPAT_DATA_PATH": "/c"},
            {"STEAM_COMPAT_TOOL_PATHS": str(self.tool),
             "STEAM_COMPAT_APP_ID": "abc", "STEAM_COMPAT_DATA_PATH": "/c"},
            {"STEAM_COMPAT_TOOL_PATHS": str(self.tool),
             "STEAM_COMPAT_APP_ID": "1"},
            {"STEAM_COMPAT_TOOL_PATHS": "/nonexistent",
             "STEAM_COMPAT_APP_ID": "1", "STEAM_COMPAT_DATA_PATH": "/c"},
            {"STEAM_COMPAT_TOOL_PATHS": str(self.tool.parent),
             "STEAM_COMPAT_APP_ID": "1", "STEAM_COMPAT_DATA_PATH": "/c"},
        ]
        argv_template = ["--args", "9", str(self.pv), "--opt=x", "--",
                         "FEXCompatTool", str(self.tool / "proton"),
                         "waitforexitandrun", "/game/game.exe"]
        for env in full_envs:
            self.execed.clear()
            with patch.object(wrap, "_mode", lambda: "neptune"),                  patch.object(wrap, "PAYLOAD", self.payload),                  patch.object(wrap, "_delegate", lambda: str(self.bwrap)),                  patch.object(graphics, "PAYLOAD", self.payload),                  patch.object(wrap.os, "execv", self._exec),                  patch.dict(os.environ, env, clear=True):
                wrap.main(argv_template)
            argv = self.execed[-1][1][1:]
            self.assertNotIn("--ro-bind", argv)
            self.assertNotIn("WINEDLLOVERRIDES", argv)

    def test_grouped_and_wildcard_overrides_no_extra(self):
        for value in ("d3d12,dxgi=n", "D3D11.DLL=b", "d3d*=n"):
            argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"],
                                 env={"WINEDLLOVERRIDES": value}, mode="neptune")
            self.assertNotIn("--ro-bind", argv)
            self.assertNotIn("--setenv", argv)
        argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"],
                             env={"PROTON_USE_WINED3D": "1"}, mode="neptune")
        self.assertNotIn("--setenv", argv)

    # --- injection ---

    def test_neptune_injects_binds_env_and_tag(self):
        argv = self.game(mode="neptune")
        self.assertEqual(argv[:2], ["--args", "9"])
        self.assertIn("--ro-bind", argv)
        self.assertIn(str(self.tool / "files/lib/wine/dxvk/x86_64-windows/d3d11.dll"), argv)
        self.assertIn(str(self.tool / "files/lib/wine/vkd3d-proton/x86_64-windows/d3d12.dll"), argv)
        self.assertEqual(argv[argv.index("BRINEWELL_NEPTUNE_BACKEND") + 1], "d3dmetal")
        self.assertEqual(argv[argv.index("WINEDLLPATH") + 1].split(":")[0],
                         "/run/host/" + str(self.payload.relative_to(self.root)) + "/bridge/x86_64")
        self.assertIn("nptunix=b", argv[argv.index("WINEDLLOVERRIDES") + 1])
        # Tail after the injected options is byte-identical.
        tail = argv[argv.index(str(self.pv)):]
        self.assertEqual(tail, [str(self.pv), "--opt=x", "--", "FEXCompatTool",
                                str(self.tool / "proton"), "waitforexitandrun",
                                "/game/game.exe", "--opt"])

    def test_mullion_tag(self):
        argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"], mode="mullion")
        self.assertEqual(argv[argv.index("BRINEWELL_NEPTUNE_BACKEND") + 1], "mullion")
        # The mode file decides the backend; a stray env value cannot steer it.
        argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"], mode="neptune",
                             env={"BRINEWELL_NEPTUNE_BACKEND": "mullion"})
        self.assertEqual(argv[argv.index("BRINEWELL_NEPTUNE_BACKEND") + 1], "d3dmetal")

    def test_ge_tool_outside_default_library(self):
        ge = self.root / "otherlib/compatibilitytools.d/GE-Proton10-32-x86_64"
        self._make_tool(ge, "x86_64")
        argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"], mode="neptune", tool=ge)
        self.assertIn("--ro-bind", argv)
        self.assertIn(str(ge / "files/lib/wine/dxvk/x86_64-windows/dxgi.dll"), argv)

    def test_explicit_proton_arch_selection(self):
        argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"],
                             env={"PROTON_USE_ARM64": "maybe"}, mode="neptune")
        self.assertNotIn("--ro-bind", argv)
        argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"],
                             env={"PROTON_USE_ARM64": "0"}, mode="neptune")
        self.assertIn("--ro-bind", argv)

    def test_dual_layout_fex_uses_x86(self):
        self._make_tool(self.tool, "aarch64")
        argv = self.game(mode="neptune", fex=True)
        self.assertIn(str(self.tool / "files/lib/wine/dxvk/x86_64-windows/d3d11.dll"), argv)

    def test_dual_layout_native_ambiguous_skips(self):
        self._make_tool(self.tool, "aarch64")
        argv = self.game(mode="neptune", fex=False)
        self.assertNotIn("--ro-bind", argv)

    def test_arm64_tool_d3dmetal_passes_through(self):
        arm = self.root / "common/Proton ARM64"
        self._make_tool(arm, "aarch64")
        argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"], mode="neptune",
                             tool=arm, fex=False)
        self.assertNotIn("--ro-bind", argv)

    def test_unsafe_bind_target_passthrough(self):
        target = self.tool / "files/lib/wine/dxvk/x86_64-windows/d3d11.dll"
        target.unlink()
        target.symlink_to("/etc/passwd")
        argv = self.game(mode="neptune")
        self.assertNotIn("--ro-bind", argv)
        self.assertNotIn("nptunix=b", argv)

    def test_missing_payload_passthrough(self):
        for link in (self.payload / "bridge/x86_64/nptunix.dll",
                     self.payload / "bridge/x86_64/nptunix.so"):
            link.unlink()
        argv = self.game(mode="neptune")
        self.assertNotIn("--ro-bind", argv)

    def test_switch_back_to_venus(self):
        self.game(mode="neptune")
        argv = self.game(mode="venus")
        self.assertNotIn("--ro-bind", argv)
        self.assertNotIn("BRINEWELL_NEPTUNE_BACKEND", argv)

    def test_neptune_leaves_fex_config_untouched(self):
        # fex-compat-tool consumes STEAM_COMPAT_FEX_CONFIG outside the
        # container; injecting it via bwrap never had an effect.
        argv = self.game(mode="neptune", env={"STEAM_COMPAT_FEX_CONFIG": "Opt6:1"})
        self.assertNotIn("STEAM_COMPAT_FEX_CONFIG", argv)

    def test_multi_ring_requires_boot_opt_in(self):
        argv = self.game(mode="neptune")
        self.assertNotIn("NPT_PERF", argv)
        with patch.object(graphics, "multi_ring_opt_in", lambda: True):
            argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"],
                                 mode="neptune", env={"NPT_PERF": "trace"})
        self.assertEqual(argv[argv.index("NPT_PERF") + 1], "trace,multi_ring")

    # --- dev Neptune payload ---

    def _make_dev_payload(self, marker=True):
        # Same layout as the packaged payload, with distinct file contents.
        for arch in ("x86_64", "aarch64"):
            for sub in (f"{arch}-windows", f"{arch}-unix", f"bridge/{arch}"):
                (self.dev / sub).mkdir(parents=True)
            for name in ("d3d11", "dxgi", "d3d12", "nptunix"):
                (self.dev / f"{arch}-windows/{name}.dll").write_bytes(("d" + name).encode())
            (self.dev / f"{arch}-unix/nptunix.so").write_bytes(b"devso")
            for suffix in ("dll", "so"):
                os.symlink(f"../../{arch}-{'windows' if suffix == 'dll' else 'unix'}/nptunix.{suffix}",
                           self.dev / f"bridge/{arch}/nptunix.{suffix}")
        if marker:
            (self.dev / "payload.json").write_text("{}")

    def test_dev_payload_absent_uses_packaged(self):
        argv = self.game(mode="neptune")
        self.assertIn("--ro-bind", argv)
        # All binds come from the packaged payload; no dev self-bind.
        self.assertIn(str(self.payload / "x86_64-windows/d3d11.dll"), argv)
        self.assertNotIn(str(self.root / "neptune-dev"), argv)
        self.assertEqual(argv[argv.index("WINEDLLPATH") + 1].split(":")[0],
                         "/run/host/" + str(self.payload.relative_to(self.root)) + "/bridge/x86_64")

    def test_dev_payload_injects_dev_sources(self):
        self._make_dev_payload()
        compat = self._make_prefix()
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        index = argv.index(str(self.dev))
        self.assertEqual(argv[index - 1:index + 2],
                         ["--ro-bind", str(self.dev), str(self.dev)])
        for name in ("d3d11", "dxgi", "d3d12"):
            self.assertIn(str(self.dev / f"x86_64-windows/{name}.dll"), argv)
        self.assertEqual(argv[argv.index("WINEDLLPATH") + 1].split(":")[0],
                         str(self.dev / "bridge/x86_64"))
        target = compat / "pfx/drive_c/windows/system32/nptunix.dll"
        self.assertEqual(target.read_bytes(),
                         (self.dev / "x86_64-windows/nptunix.dll").read_bytes())

    def test_dev_payload_removal_restores_packaged_nptunix(self):
        self._make_dev_payload()
        compat = self._make_prefix()
        self.game(mode="neptune", env=self._nptunix_env(compat))
        target = compat / "pfx/drive_c/windows/system32/nptunix.dll"
        self.assertEqual(target.read_bytes(), b"dnptunix")
        for child in sorted(self.dev.rglob("*"), reverse=True):
            if child.is_dir() and not child.is_symlink():
                child.rmdir()
            else:
                child.unlink()
        self.dev.rmdir()
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertEqual(target.read_bytes(), self._nptunix_payload())

    def test_dev_payload_without_marker_ignored(self):
        self._make_dev_payload(marker=False)
        argv = self.game(mode="neptune")
        self.assertIn(str(self.payload / "x86_64-windows/d3d11.dll"), argv)
        self.assertNotIn(str(self.dev), argv)
        self.assertEqual(argv[argv.index("WINEDLLPATH") + 1].split(":")[0],
                         "/run/host/" + str(self.payload.relative_to(self.root)) + "/bridge/x86_64")

    def test_dev_payload_venus_injects_nothing(self):
        self._make_dev_payload()
        argv = self.game(mode="venus")
        self.assertNotIn("--ro-bind", argv)
        self.assertNotIn("BRINEWELL_NEPTUNE_BACKEND", argv)

    # --- prefix nptunix.dll sync ---

    def _make_prefix(self, system32=True):
        compat = self.root / "compat/814380"
        if system32:
            (compat / "pfx/drive_c/windows/system32").mkdir(parents=True)
        else:
            compat.mkdir(parents=True, exist_ok=True)
        return compat

    def _nptunix_env(self, compat):
        return {"STEAM_COMPAT_DATA_PATH": str(compat)}

    def _nptunix_payload(self):
        return (self.payload / "x86_64-windows/nptunix.dll").read_bytes()

    def test_neptune_creates_missing_prefix_nptunix(self):
        compat = self._make_prefix()
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        target = compat / "pfx/drive_c/windows/system32/nptunix.dll"
        self.assertIn("--ro-bind", argv)
        self.assertIn("nptunix=b", argv[argv.index("WINEDLLOVERRIDES") + 1])
        self.assertTrue(target.is_file())
        self.assertFalse(target.is_symlink())
        self.assertEqual(target.read_bytes(), self._nptunix_payload())

    def test_neptune_replaces_dangling_nptunix_symlink(self):
        compat = self._make_prefix()
        target = compat / "pfx/drive_c/windows/system32/nptunix.dll"
        target.symlink_to("../old-runtimes/gone/nptunix.dll")
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertFalse(target.is_symlink())
        self.assertEqual(target.read_bytes(), self._nptunix_payload())

    def test_neptune_replaces_valid_nptunix_symlink(self):
        compat = self._make_prefix()
        other = compat / "old-nptunix.dll"
        other.write_bytes(b"old")
        target = compat / "pfx/drive_c/windows/system32/nptunix.dll"
        target.symlink_to(other)
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertFalse(target.is_symlink())
        self.assertEqual(target.read_bytes(), self._nptunix_payload())

    def test_neptune_identical_prefix_nptunix_untouched(self):
        compat = self._make_prefix()
        target = compat / "pfx/drive_c/windows/system32/nptunix.dll"
        target.write_bytes(self._nptunix_payload())
        before = target.stat()
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        after = target.stat()
        self.assertEqual((before.st_ino, before.st_mtime_ns),
                         (after.st_ino, after.st_mtime_ns))

    def test_venus_never_writes_prefix_nptunix(self):
        compat = self._make_prefix()
        argv = self.game(mode="venus", env=self._nptunix_env(compat))
        self.assertNotIn("--ro-bind", argv)
        self.assertFalse((compat / "pfx/drive_c/windows/system32/nptunix.dll").exists())

    def test_neptune_fresh_prefix_creates_chain(self):
        compat = self._make_prefix(system32=False)
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertIn("nptunix=b", argv[argv.index("WINEDLLOVERRIDES") + 1])
        target = compat / "pfx/drive_c/windows/system32/nptunix.dll"
        self.assertTrue(target.is_file())
        self.assertFalse(target.is_symlink())
        self.assertEqual(target.read_bytes(), self._nptunix_payload())

    def test_neptune_symlinked_pfx_refused(self):
        compat = self._make_prefix(system32=False)
        elsewhere = self.root / "elsewhere/pfx"
        elsewhere.mkdir(parents=True)
        (compat / "pfx").symlink_to(elsewhere)
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotIn("--ro-bind", argv)
        self.assertNotIn("WINEDLLOVERRIDES", argv)
        self.assertFalse((elsewhere / "drive_c/windows/system32/nptunix.dll").exists())

    def test_neptune_dangling_drive_c_symlink_refused(self):
        compat = self._make_prefix(system32=False)
        (compat / "pfx").mkdir()
        (compat / "pfx/drive_c").symlink_to("gone")
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotIn("--ro-bind", argv)
        self.assertNotIn("WINEDLLOVERRIDES", argv)
        self.assertFalse((compat / "pfx/drive_c/windows/system32").exists())

    def test_neptune_missing_compat_dir_refused(self):
        compat = self.root / "compat/nonexistent"
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotIn("--ro-bind", argv)
        self.assertNotIn("WINEDLLOVERRIDES", argv)
        self.assertFalse(compat.exists())

    def test_neptune_casefold_ioctl_failure_still_succeeds(self):
        compat = self._make_prefix(system32=False)
        with patch.object(wrap.fcntl, "ioctl",
                          side_effect=OSError(25, "Inappropriate ioctl for device")):
            argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        target = compat / "pfx/drive_c/windows/system32/nptunix.dll"
        self.assertEqual(target.read_bytes(), self._nptunix_payload())

    def test_neptune_compat_through_symlinked_ancestor_succeeds(self):
        real = self.root / "steamlibrary/steamapps/compatdata/814380"
        real.mkdir(parents=True)
        link = self.root / "linked-library"
        link.symlink_to(real.parent.parent.parent)
        compat = link / "steamapps/compatdata/814380"
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertIn("nptunix=b", argv[argv.index("WINEDLLOVERRIDES") + 1])
        target = real / "pfx/drive_c/windows/system32/nptunix.dll"
        self.assertTrue(target.is_file())
        self.assertEqual(target.read_bytes(), self._nptunix_payload())

    def test_neptune_symlinked_system32_dir_passes_through(self):
        compat = self._make_prefix(system32=False)
        elsewhere = self.root / "elsewhere/system32"
        elsewhere.mkdir(parents=True)
        (compat / "pfx/drive_c/windows").mkdir(parents=True)
        (compat / "pfx/drive_c/windows/system32").symlink_to(elsewhere)
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotIn("--ro-bind", argv)
        self.assertNotIn("WINEDLLOVERRIDES", argv)
        self.assertFalse((elsewhere / "nptunix.dll").exists())

    # --- per-game NGX payload (BRINEWELL_NGX_SR) ---

    def _ngx_payload(self, root=None, tag=b"x"):
        root = root or self.payload
        for name in ("nvngx", "nvapi64"):
            (root / f"x86_64-windows/{name}.dll").write_bytes(
                _pe(0x8664) + tag + name.encode())

    def _ngx_env(self, compat, extra=None):
        return {**self._nptunix_env(compat), "BRINEWELL_NGX_SR": "1", **(extra or {})}

    def _sys32(self, compat):
        return compat / "pfx/drive_c/windows/system32"

    def _nvngx(self, compat):
        return self._sys32(compat) / "nvngx.dll"

    def _nvapi64(self, compat):
        return self._sys32(compat) / "nvapi64.dll"

    def _tool_nvapi64(self, tool=None):
        return (tool or self.tool) / "files/lib/wine/nvapi/x86_64-windows/nvapi64.dll"

    def _ngx_marker(self, compat):
        return compat / ".brinewell-ngx-sr.json"

    def _ngx_stock(self, compat):
        self._nvapi64(compat).write_bytes(self._tool_nvapi64().read_bytes())

    def _setenvs(self, argv):
        return {b: argv[i + 2]
                for i, (a, b) in enumerate(zip(argv, argv[1:]))
                if a == "--setenv"}

    def _unsetenvs(self, argv):
        return {argv[i + 1] for i, a in enumerate(argv[:-1]) if a == "--unsetenv"}

    def _binds(self, argv):
        return [(argv[i + 1], argv[i + 2])
                for i, a in enumerate(argv[:-2]) if a == "--ro-bind"]

    def assertNotCandidate(self, argv):
        # The internal flags are set only on a candidate launch; refused or
        # non-opt-in launches may still pass --unsetenv for inherited values.
        for var in ("BRINEWELL_NGX_SR_CANDIDATE", "BRINEWELL_NGX_CORE_SHA256",
                    "BRINEWELL_NGX_NVAPI_SHA256", "BRINEWELL_NGX_SR_ACTIVE"):
            self.assertNotIn(var, self._setenvs(argv))

    def assertCandidate(self, argv, root=None):
        root = root or self.payload
        setenvs = self._setenvs(argv)
        self.assertEqual(setenvs["BRINEWELL_NGX_SR_CANDIDATE"], "1")
        self.assertEqual(setenvs["BRINEWELL_NGX_CORE_SHA256"],
                         wrap._sha256(root / "x86_64-windows/nvngx.dll"))
        self.assertEqual(setenvs["BRINEWELL_NGX_NVAPI_SHA256"],
                         wrap._sha256(root / "x86_64-windows/nvapi64.dll"))
        self.assertIn("BRINEWELL_NGX_SR_ACTIVE", self._unsetenvs(argv))
        self.assertIn((str(root / "x86_64-windows/nvapi64.dll"),
                       str(self._tool_nvapi64())), self._binds(argv))
        overrides = setenvs["WINEDLLOVERRIDES"]
        self.assertIn("nvngx=n", overrides)
        self.assertIn("nvapi64=n", overrides)

    def test_ngx_candidate_binds_copy_source_and_publishes_core(self):
        compat = self._make_prefix()
        self._ngx_payload()
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertCandidate(argv)
        nvngx = self._nvngx(compat)
        self.assertFalse(nvngx.is_symlink())
        self.assertEqual(nvngx.read_bytes(),
                         (self.payload / "x86_64-windows/nvngx.dll").read_bytes())
        marker = json.loads(self._ngx_marker(compat).read_text())
        self.assertEqual(sorted(marker["files"]), ["nvngx.dll"])
        # The wrapper never writes nvapi64.dll into the prefix.
        self.assertFalse(self._nvapi64(compat).exists())

    def test_ngx_stock_prefix_nvapi64_never_touched(self):
        compat = self._make_prefix()
        self._ngx_payload()
        self._ngx_stock(compat)
        stock = self._nvapi64(compat).read_bytes()
        self.assertCandidate(self.game(mode="neptune", env=self._ngx_env(compat)))
        self.assertEqual(self._nvapi64(compat).read_bytes(), stock)
        # Proton's setup unlink+copy cycle between launches leaves the
        # deployment intact: the bind still delivers the payload copy.
        self._nvapi64(compat).unlink()
        self._ngx_stock(compat)
        self.assertCandidate(self.game(mode="neptune", env=self._ngx_env(compat)))
        self.assertEqual(self._nvapi64(compat).read_bytes(), stock)
        self.assertEqual(self._nvngx(compat).read_bytes(),
                         (self.payload / "x86_64-windows/nvngx.dll").read_bytes())
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotCandidate(argv)
        self.assertEqual(self._nvapi64(compat).read_bytes(), stock)
        self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_stale_payload_in_prefix_never_removed(self):
        # An earlier generation may have left its nvapi64.dll in the prefix;
        # the wrapper never claims or deletes it, opt-in or opt-out.
        compat = self._make_prefix()
        self._ngx_payload()
        self._nvapi64(compat).write_bytes(_pe(0x8664) + b"old-gen-nvapi64")
        self.assertCandidate(self.game(mode="neptune", env=self._ngx_env(compat)))
        self.assertEqual(self._nvapi64(compat).read_bytes(),
                         _pe(0x8664) + b"old-gen-nvapi64")
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotCandidate(argv)
        self.assertEqual(self._nvapi64(compat).read_bytes(),
                         _pe(0x8664) + b"old-gen-nvapi64")
        self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_not_requested_leaves_everything_alone(self):
        compat = self._make_prefix()
        self._ngx_payload()
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertNotCandidate(argv)
        self.assertNotIn("nvngx", self._setenvs(argv)["WINEDLLOVERRIDES"])
        self.assertNotIn((str(self.payload / "x86_64-windows/nvapi64.dll"),
                          str(self._tool_nvapi64())), self._binds(argv))
        self.assertFalse(self._nvngx(compat).exists())
        self.assertFalse(self._ngx_marker(compat).exists())
        # A disabled value is not opt-in either.
        argv = self.game(mode="neptune",
                         env={**self._nptunix_env(compat), "BRINEWELL_NGX_SR": "0"})
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_missing_payload_keeps_base_injection(self):
        compat = self._make_prefix()
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertNotCandidate(argv)
        self.assertNotIn("nvngx", self._setenvs(argv)["WINEDLLOVERRIDES"])
        self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_invalid_payload_keeps_base_injection(self):
        compat = self._make_prefix()
        for name in ("nvngx", "nvapi64"):
            (self.payload / f"x86_64-windows/{name}.dll").write_bytes(b"not a pe")
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_missing_tool_slot_refused(self):
        compat = self._make_prefix()
        self._ngx_payload()
        self._tool_nvapi64().unlink()
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        (self._tool_nvapi64().parent).rmdir()
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_foreign_nvngx_never_touched(self):
        compat = self._make_prefix()
        self._ngx_payload()
        foreign = self._nvngx(compat)
        foreign.write_bytes(b"third party nvngx")
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertNotCandidate(argv)
        self.assertEqual(foreign.read_bytes(), b"third party nvngx")
        self.assertFalse(self._ngx_marker(compat).exists())

    def test_ngx_foreign__nvngx_provider_refused(self):
        compat = self._make_prefix()
        self._ngx_payload()
        (self._sys32(compat) / "_nvngx.dll").write_bytes(b"driver core")
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        self.assertFalse(self._ngx_marker(compat).exists())

    def test_ngx_symlinked_nvngx_refused(self):
        compat = self._make_prefix()
        self._ngx_payload()
        link = self._nvngx(compat)
        link.symlink_to("../old/nvngx.dll")
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertNotCandidate(argv)
        self.assertTrue(link.is_symlink())
        self.assertFalse(self._ngx_marker(compat).exists())

    def test_ngx_user_disables_nvapi_refused(self):
        compat = self._make_prefix()
        self._ngx_payload()
        for extra in ({"PROTON_DISABLE_NVAPI": "1"},
                      {"PROTON_ENABLE_NVAPI": "0"}):
            argv = self.game(mode="neptune", env=self._ngx_env(compat, extra))
            self.assertIn("--ro-bind", argv)
            self.assertNotCandidate(argv)
            self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_repeated_invocation_and_payload_update(self):
        compat = self._make_prefix()
        self._ngx_payload()
        self.assertCandidate(self.game(mode="neptune", env=self._ngx_env(compat)))
        self.assertCandidate(self.game(mode="neptune", env=self._ngx_env(compat)))
        self._ngx_payload(tag=b"v2-")
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertCandidate(argv)
        self.assertEqual(self._nvngx(compat).read_bytes(),
                         (self.payload / "x86_64-windows/nvngx.dll").read_bytes())

    def test_ngx_publish_failure_rolls_back(self):
        compat = self._make_prefix()
        self._ngx_payload()
        real_link = os.link

        def flaky(src, dst, **kw):
            if str(dst).endswith("nvngx.dll"):
                raise OSError(5, "simulated publish failure")
            return real_link(src, dst, **kw)

        with patch.object(wrap.os, "link", side_effect=flaky):
            argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        self.assertFalse(self._ngx_marker(compat).exists())
        self.assertFalse(list(self._sys32(compat).glob(".ngx-*")))

    def test_ngx_failed_update_restores_previous(self):
        compat = self._make_prefix()
        self._ngx_payload()
        self.game(mode="neptune", env=self._ngx_env(compat))
        v1 = self._nvngx(compat).read_bytes()
        v1_marker = self._ngx_marker(compat).read_text()
        self._ngx_payload(tag=b"v2-")
        real_replace = os.replace

        def flaky_marker(src, dst, **kw):
            if str(dst).endswith(".brinewell-ngx-sr.json"):
                raise OSError(5, "simulated marker commit failure")
            return real_replace(src, dst, **kw)

        with patch.object(wrap.os, "replace", side_effect=flaky_marker):
            argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertNotCandidate(argv)
        self.assertEqual(self._nvngx(compat).read_bytes(), v1)
        self.assertEqual(self._ngx_marker(compat).read_text(), v1_marker)
        self.assertFalse(list(self._sys32(compat).glob(".ngx-*")))
        # The next launch retries the update, and opt-out still cleans.
        self.assertCandidate(self.game(mode="neptune", env=self._ngx_env(compat)))
        self.assertEqual(self._nvngx(compat).read_bytes(),
                         (self.payload / "x86_64-windows/nvngx.dll").read_bytes())
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        self.assertFalse(self._ngx_marker(compat).exists())

    def test_ngx_corrupt_marker_safe_skip(self):
        compat = self._make_prefix()
        self._ngx_payload()
        foreign = self._nvngx(compat)
        foreign.write_bytes(b"foreign nvngx")
        outside = compat / "../escape.dll"
        outside.write_bytes(b"escape")
        escape_digest = wrap._sha256(outside)
        for bad in ("[]", "not json",
                    '{"files": ["nvngx.dll"]}',
                    '{"files": {"../escape.dll": "%s"}}' % escape_digest,
                    '{"files": {"nvngx.dll": "short"}}'):
            self._ngx_marker(compat).write_text(bad)
            argv = self.game(mode="neptune", env=self._nptunix_env(compat))
            self.assertIn("--ro-bind", argv)
            self.assertNotCandidate(argv)
            self.assertEqual(outside.read_bytes(), b"escape")
            self.assertEqual(foreign.read_bytes(), b"foreign nvngx")
        # A symlinked marker is ignored and removed, still without touching
        # foreign prefix files.
        record = compat / "marker-elsewhere.json"
        record.write_text('{"files": {"nvngx.dll": "%s"}}'
                          % wrap._sha256(foreign))
        self._ngx_marker(compat).symlink_to(record)
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertIn("--ro-bind", argv)
        self.assertTrue(record.is_file())
        self.assertFalse(self._ngx_marker(compat).exists())
        self.assertEqual(foreign.read_bytes(), b"foreign nvngx")

    def test_ngx_v1_marker_migrates(self):
        # A v1 marker records both files, but the prefix nvapi64.dll is
        # already the tool's stock copy: the legacy claim is dropped, our
        # nvngx claim still applies, and the marker is rewritten v2-style.
        compat = self._make_prefix()
        self._ngx_payload()
        v1_nvngx = _pe(0x8664) + b"v1-nvngx"
        self._nvngx(compat).write_bytes(v1_nvngx)
        self._ngx_stock(compat)
        stock = self._nvapi64(compat).read_bytes()
        files = {"files": {"nvngx.dll": wrap._sha256(self._nvngx(compat)),
                           "nvapi64.dll": wrap._sha256(self.payload / "x86_64-windows/nvapi64.dll")}}
        self._ngx_marker(compat).write_text(json.dumps(files))
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertCandidate(argv)
        self.assertEqual(self._nvapi64(compat).read_bytes(), stock)
        self.assertEqual(self._nvngx(compat).read_bytes(),
                         (self.payload / "x86_64-windows/nvngx.dll").read_bytes())
        marker = json.loads(self._ngx_marker(compat).read_text())
        self.assertEqual(sorted(marker["files"]), ["nvngx.dll"])

    def test_ngx_v1_marker_optout_removes_only_core(self):
        compat = self._make_prefix()
        self._ngx_payload()
        self._nvngx(compat).write_bytes(_pe(0x8664) + b"v1-nvngx")
        self._nvapi64(compat).write_bytes(_pe(0x8664) + b"v1-nvapi64")
        files = {"files": {"nvngx.dll": wrap._sha256(self._nvngx(compat)),
                           "nvapi64.dll": wrap._sha256(self._nvapi64(compat))}}
        self._ngx_marker(compat).write_text(json.dumps(files))
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        self.assertFalse(self._ngx_marker(compat).exists())
        # The old nvapi64 claim grants no deletion right over the file.
        self.assertEqual(self._nvapi64(compat).read_bytes(),
                         _pe(0x8664) + b"v1-nvapi64")

    def test_ngx_inherited_internal_env_cleared(self):
        compat = self._make_prefix()
        self._ngx_payload()
        inherited = {"BRINEWELL_NGX_SR_ACTIVE": "1",
                     "BRINEWELL_NGX_SR_CANDIDATE": "1",
                     "BRINEWELL_NGX_CORE_SHA256": "0" * 64,
                     "BRINEWELL_NGX_NVAPI_SHA256": "0" * 64}
        # An inherited set must not survive a non-opt-in launch.
        argv = self.game(mode="neptune",
                         env={**self._nptunix_env(compat), **inherited})
        self.assertIn("--ro-bind", argv)
        self.assertNotCandidate(argv)
        unsets = self._unsetenvs(argv)
        for var in inherited:
            self.assertIn(var, unsets)
        # A refused opt-in clears them too.
        foreign = self._nvngx(compat)
        foreign.write_bytes(b"third party nvngx")
        argv = self.game(mode="neptune",
                         env=self._ngx_env(compat, inherited))
        self.assertNotCandidate(argv)
        unsets = self._unsetenvs(argv)
        for var in inherited:
            self.assertIn(var, unsets)
        self.assertEqual(foreign.read_bytes(), b"third party nvngx")

    def test_ngx_user_modified_nvngx_kept_and_disowned(self):
        compat = self._make_prefix()
        self._ngx_payload()
        self.game(mode="neptune", env=self._ngx_env(compat))
        # The user replaced the managed file: next opt-in launch must not
        # overwrite it, and revoking the opt-in keeps it too.
        foreign = self._nvngx(compat)
        foreign.write_bytes(b"user replacement")
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertNotCandidate(argv)
        self.assertEqual(foreign.read_bytes(), b"user replacement")
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotCandidate(argv)
        self.assertEqual(foreign.read_bytes(), b"user replacement")
        self.assertFalse(self._ngx_marker(compat).exists())

    def test_ngx_cleanup_on_optout_backend_and_payload_loss(self):
        compat = self._make_prefix()
        self._ngx_stock(compat)
        stock = self._nvapi64(compat).read_bytes()
        for drop_opt_in, mode, keep_payload in (
                (True, "neptune", True), (False, "venus", True),
                (False, "neptune", False)):
            self._ngx_payload()
            self.game(mode="neptune", env=self._ngx_env(compat))
            self.assertTrue(self._nvngx(compat).is_file())
            env = self._nptunix_env(compat) if drop_opt_in else self._ngx_env(compat)
            if not keep_payload:
                for name in ("nvngx", "nvapi64"):
                    (self.payload / f"x86_64-windows/{name}.dll").unlink()
            argv = self.game(mode=mode, env=env)
            self.assertNotCandidate(argv)
            self.assertFalse(self._nvngx(compat).exists(), f"{mode} {keep_payload}")
            self.assertFalse(self._ngx_marker(compat).exists())
            self.assertEqual(self._nvapi64(compat).read_bytes(), stock)
            self._ngx_payload()

    def test_ngx_busy_prefix_delays_cleanup(self):
        compat = self._make_prefix()
        self._ngx_payload()
        self.game(mode="neptune", env=self._ngx_env(compat))
        with patch.object(wrap, "_ngx_prefix_busy", lambda c: True):
            argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotCandidate(argv)
        self.assertTrue(self._nvngx(compat).is_file())
        self.assertTrue(self._ngx_marker(compat).exists())
        argv = self.game(mode="neptune", env=self._nptunix_env(compat))
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        self.assertFalse(self._ngx_marker(compat).exists())

    def test_ngx_busy_lock_skips_install(self):
        compat = self._make_prefix()
        self._ngx_payload()
        import fcntl
        fd = os.open(compat / ".brinewell-ngx-sr.lock", os.O_CREAT | os.O_RDWR)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            argv = self.game(mode="neptune", env=self._ngx_env(compat))
        finally:
            os.close(fd)
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_user_override_conflict_disables_and_cleans(self):
        compat = self._make_prefix()
        self._ngx_payload()
        for conflict in ("nvngx=b", "nvapi64.dll=n", "_nvngx=n"):
            argv = self.game(mode="neptune",
                             env=self._ngx_env(compat,
                                               {"WINEDLLOVERRIDES": conflict}))
            self.assertIn("--ro-bind", argv)
            self.assertNotCandidate(argv)
            self.assertFalse(self._nvngx(compat).exists())
        # A wildcard override refuses base Neptune injection entirely.
        argv = self.game(mode="neptune",
                         env=self._ngx_env(compat,
                                           {"WINEDLLOVERRIDES": "nv*=n"}))
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        # An already-managed prefix is cleaned when the user takes over.
        self.game(mode="neptune", env=self._ngx_env(compat))
        argv = self.game(mode="neptune",
                         env=self._ngx_env(compat, {"WINEDLLOVERRIDES": "nvngx=b"}))
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        self.assertFalse(self._ngx_marker(compat).exists())

    def test_ngx_dev_payload_sources(self):
        self._make_dev_payload()
        self._ngx_payload(root=self.dev, tag=b"dev-")
        compat = self._make_prefix()
        argv = self.game(mode="neptune", env=self._ngx_env(compat))
        self.assertCandidate(argv, root=self.dev)
        self.assertEqual(self._nvngx(compat).read_bytes(),
                         (self.dev / "x86_64-windows/nvngx.dll").read_bytes())

    def test_ngx_never_for_arm64(self):
        arm = self.root / "common/Proton ARM64"
        self._make_tool(arm, "aarch64")
        self._ngx_payload()
        compat = self._make_prefix()
        argv = self.run_wrap(["waitforexitandrun", "/game/x.exe"], mode="neptune",
                             tool=arm, fex=False, env=self._ngx_env(compat))
        self.assertNotIn("--ro-bind", argv)
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())

    def test_ngx_mullion_cleans_managed_prefix(self):
        compat = self._make_prefix()
        self._ngx_payload()
        self.game(mode="neptune", env=self._ngx_env(compat))
        argv = self.game(mode="mullion", env=self._ngx_env(compat))
        self.assertNotCandidate(argv)
        self.assertFalse(self._nvngx(compat).exists())
        self.assertFalse(self._ngx_marker(compat).exists())

if __name__ == "__main__":
    unittest.main()
