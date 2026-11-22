import importlib.machinery
import importlib.util
import os
from pathlib import Path
import select
import socket
import sys
import tempfile
import threading
import time
import tty
import unittest
from unittest.mock import patch


SCRIPTS = Path(__file__).resolve().parents[2] / "runtime/guest/rootfs/usr/lib/brinewell"
loader = importlib.machinery.SourceFileLoader("graphics_control", str(SCRIPTS / "steam-graphics-control"))
spec = importlib.util.spec_from_loader(loader.name, loader)
control = importlib.util.module_from_spec(spec)
loader.exec_module(control)


class GraphicsControlTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.state = self.root / "graphics-backend"
        self.host, self.guest = socket.socketpair()
        self.addCleanup(self.host.close)
        self.addCleanup(self.guest.close)
        self.f = self.guest.makefile("rwb", buffering=0)
        self.result = []
        patcher = patch.object(control, "STATE", str(self.state))
        self.addCleanup(patcher.stop)
        patcher.start()
        self.thread = threading.Thread(
            target=lambda: self.result.append(control.serve(self.f)), daemon=True)
        self.host_file = self.host.makefile("rwb", buffering=0)
        self.host.settimeout(10)
        self.thread.start()
        # First line is always readiness.
        self.assertEqual(self.host_file.readline().strip(), b"ready")

    def test_backend_apply_ack_and_eof_restart(self):
        self.host_file.write(b"backend mullion\n")
        self.host_file.flush()
        self.assertEqual(self.host_file.readline().strip(), b"applied mullion")
        self.assertEqual(self.state.read_text().strip(), "mullion")
        # EOF (host closed the port) exits nonzero so systemd restarts the
        # service and it re-announces readiness on the next descriptor.
        self.host_file.close()
        self.host.close()
        self.thread.join(timeout=5)
        self.assertEqual(self.result, [2])

    def test_rejects_invalid_and_overlong_requests(self):
        self.host_file.write(b"backend bogus\n")
        self.host_file.write(b"backend venus extra\n")
        self.host_file.write(b"drop everything\n")
        # Overlong line is drained to its newline and rejected.
        self.host_file.write(b"backend " + b"x" * (control.MAX_LINE + 10) + b"\n")
        self.host_file.write(b"backend neptune\n")
        self.host_file.flush()
        self.assertEqual(self.host_file.readline().strip(), b"applied neptune")
        self.assertEqual(self.state.read_text().strip(), "neptune")

    def test_backend_rewrite_is_atomic_per_request(self):
        for mode in ("venus", "neptune", "mullion", "venus"):
            self.host_file.write(b"backend " + mode.encode() + b"\n")
        self.host_file.flush()
        for mode in ("venus", "neptune", "mullion", "venus"):
            self.assertEqual(self.host_file.readline().strip(),
                             b"applied " + mode.encode())
        self.assertEqual(self.state.read_text().strip(), "venus")

    def test_main_over_real_character_device(self):
        # The virtio port is a character device; drive main() through a pty.
        self.host_file.close()
        self.host.close()
        master, slave = os.openpty()
        tty.setraw(master)
        tty.setraw(slave)
        result = []
        thread = threading.Thread(
            target=lambda: result.append(control.main(os.ttyname(slave))),
            daemon=True)
        thread.start()
        try:
            def read_line(timeout=10):
                buf = b""
                deadline = time.monotonic() + timeout
                while not buf.endswith(b"\n"):
                    left = deadline - time.monotonic()
                    self.assertGreater(left, 0, "timed out waiting for a line")
                    ready, _, _ = select.select([master], [], [], left)
                    self.assertTrue(ready, "timed out waiting for a line")
                    chunk = os.read(master, 4096)
                    if not chunk:
                        break
                    buf += chunk
                return buf

            self.assertEqual(read_line().strip(), b"ready")
            os.write(master, b"backend venus\n")
            self.assertEqual(read_line().strip(), b"applied venus")
            self.assertEqual(self.state.read_text(), "venus\n")
        finally:
            os.close(master)
            os.close(slave)
        # Closing the master is host EOF for the service: nonzero exit.
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(result, [2])


if __name__ == "__main__":
    unittest.main()
