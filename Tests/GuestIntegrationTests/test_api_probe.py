import errno
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[2] / "runtime/guest/tools/brinewell-api-probe.c"


class ApiProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "probe"
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(SOURCE),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.root = Path(self.tempdir.name)

    def check(self, args, backend, code):
        result = subprocess.run([str(self.binary), "--json", *map(str, args)], capture_output=True)
        self.assertEqual(result.returncode, code, result.stderr)
        parsed = json.loads(result.stdout)
        self.assertEqual(parsed["backend"], backend)
        return parsed["evidence"]

    def test_individual_dependencies(self):
        for name, backend, code in (("d3d12.dll", "neptune", 0),
                                    ("d3d11.dll", "neptune", 0),
                                    ("d3d10.dll", "mullion", 3),
                                    ("d3d10_1.dll", "mullion", 3),
                                    ("d3d10core.dll", "mullion", 3),
                                    ("d3d9.dll", "venus", 1),
                                    ("vulkan-1.dll", "venus", 1),
                                    ("opengl32.dll", "venus", 1),
                                    ("", "venus", 1)):
            with self.subTest(name=name):
                exe = self.root / "game.exe"
                exe.write_bytes(name.upper().encode())
                evidence = self.check(["check", exe], backend, code)
                if backend != "venus":
                    self.assertIn(name, evidence[0].lower())

    def test_priority_within_and_across_files(self):
        a, b = self.root / "a.exe", self.root / "b.exe"
        a.write_bytes(b"d3d10.dll\0d3d11.dll")
        self.check(["check", a], "neptune", 0)
        for first, second in ((b"d3d10.dll", b"d3d11.dll"),
                              (b"d3d11.dll", b"d3d10.dll")):
            a.write_bytes(first)
            b.write_bytes(second)
            self.check(["check", a, b], "neptune", 0)
            self.check(["scan", self.root], "neptune", 0)

    def test_unaligned_utf16(self):
        exe = self.root / "game.exe"
        exe.write_bytes(b"x" + "D3D10CORE.DLL".encode("utf-16le"))
        self.check(["check", exe], "mullion", 3)

    def test_symlinks_are_not_followed(self):
        outside = self.root / "outside"
        outside.mkdir()
        exe = outside / "game.exe"
        exe.write_bytes(b"d3d12.dll")
        install = self.root / "install"
        install.mkdir()
        (install / "linked.exe").symlink_to(exe)
        (install / "linked-dir").symlink_to(outside)
        self.check(["check", install / "linked.exe"], "venus", 1)
        self.check(["scan", install], "venus", 1)
        self.check(["scan", install / "linked-dir"], "venus", 1)

    def test_invalid_utf8_filename_produces_valid_json(self):
        filename = os.fsencode(self.root) + b"/bad_\xff_game.exe"
        try:
            with open(filename, "wb") as handle:
                handle.write(b"d3d12.dll")
        except OSError as error:
            if error.errno == errno.EILSEQ:
                self.skipTest("filesystem rejects non-UTF-8 filenames")
            raise
        result = subprocess.run([str(self.binary), "--json", "scan", str(self.root)],
                                capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = json.loads(result.stdout.decode("utf-8"))
        self.assertEqual(parsed["backend"], "neptune")
        self.assertIn("bad_\ufffd_game.exe", parsed["evidence"][0])

    def test_usage(self):
        result = subprocess.run([str(self.binary)], capture_output=True)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
