import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import time
import tty
import unittest


PORT = Path("/dev/virtio-ports/fx.clipboard")
AGENT = Path("/clipboard")


@unittest.skipUnless(AGENT.is_file() and shutil.which("Xvfb") and shutil.which("xclip"),
                     "run with the clipboard agent at /clipboard in a Linux container with Xvfb and xclip")
class GamescopeClipboardTests(unittest.TestCase):
    def setUp(self):
        PORT.parent.mkdir(exist_ok=True)
        self.master, slave = pty.openpty()
        self.addCleanup(os.close, self.master)
        self.addCleanup(os.close, slave)
        tty.setraw(slave)
        PORT.symlink_to(os.ttyname(slave))
        self.addCleanup(PORT.unlink)
        self.displays = [self.start_display(), self.start_display()]
        self.agent = self.start(
            [str(AGENT), "--gamescope"],
            env={**os.environ, "DISPLAY": self.displays[0], "STEAM_GAME_DISPLAY_0": self.displays[1]},
        )
        self.pending = bytearray()
        self.assertEqual(self.read_frame(), b"R")

    def start(self, argv, **options):
        process = subprocess.Popen(argv, **options)
        self.addCleanup(self.stop, process)
        return process

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for stream in [process.stdin, process.stdout]:
            if stream:
                stream.close()

    def start_display(self):
        server = self.start(["Xvfb", "-displayfd", "1", "-nolisten", "tcp"], stdout=subprocess.PIPE)
        self.assertTrue(select.select([server.stdout], [], [], 3)[0])
        number = server.stdout.readline().strip()
        self.assertTrue(number.isdigit())
        return ":" + number.decode()

    def read_frame(self):
        deadline = time.monotonic() + 3
        while b"\xfe" not in self.pending:
            remaining = max(0, deadline - time.monotonic())
            self.assertTrue(select.select([self.master], [], [], remaining)[0], "no clipboard frame")
            self.pending.extend(os.read(self.master, 65536))
        frame, _, rest = self.pending.partition(b"\xfe")
        self.pending = rest
        self.assertEqual(frame[:1], b"\xff")
        return frame[1:]

    def copy(self, display, text):
        owner = self.start(
            ["xclip", "-display", display, "-selection", "clipboard", "-in", "-quiet"],
            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        owner.stdin.write(text)
        owner.stdin.close()

    def paste(self, display, expected):
        deadline = time.monotonic() + 3
        while True:
            result = subprocess.run(
                ["xclip", "-display", display, "-selection", "clipboard", "-out"],
                capture_output=True, timeout=2,
            )
            if result.stdout == expected:
                return
            self.assertLess(time.monotonic(), deadline, f"selection missing on {display}")
            time.sleep(0.02)

    def test_both_displays_share_unicode_and_maximum_size_without_echo(self):
        small = "宿主\n第二行 🎮".encode()
        full = "界".encode() * 21845 + b"x"
        for value in [small, full]:
            os.write(self.master, b"\xffT" + value + b"\xfe")
            for display in self.displays:
                self.paste(display, value)
            self.assertFalse(select.select([self.master], [], [], 0.1)[0], "host selection echoed")

        for index, value in enumerate([small, full]):
            # Use different content from the last host update on each display.
            value = value[:-1] + b"y" if value == full else value
            self.copy(self.displays[index], value)
            self.assertEqual(self.read_frame(), b"T" + value)
            self.paste(self.displays[1 - index], value)
            self.assertFalse(select.select([self.master], [], [], 0.1)[0], "mirrored selection echoed")


if __name__ == "__main__":
    unittest.main()
