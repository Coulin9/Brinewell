import importlib.machinery
import importlib.util
import json
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
        self.sock_path = str(self.root / "resolve.sock")
        env_patcher = patch.dict(os.environ, {"BRINEWELL_GRAPHICS_RESOLVE_SOCK": self.sock_path})
        self.addCleanup(env_patcher.stop)
        env_patcher.start()
        self.host, self.guest = socket.socketpair()
        self.addCleanup(self.host.close)
        self.addCleanup(self.guest.close)
        self.f = self.guest.makefile("rwb", buffering=0)
        self.result = []
        patcher = patch.object(control, "STATE", str(self.state))
        self.addCleanup(patcher.stop)
        patcher.start()
        hdr_patcher = patch.object(control, "HDR_STATE", str(self.root / "hdr/display.json"))
        self.addCleanup(hdr_patcher.stop)
        hdr_patcher.start()
        self.thread = threading.Thread(
            target=lambda: self.result.append(control.serve(self.f)), daemon=True)
        self.host_file = self.host.makefile("rwb", buffering=0)
        self.host.settimeout(10)
        self.thread.start()
        # First line is always readiness.
        self.assertEqual(self.host_file.readline().strip(), b"ready")

    def connect_resolver(self, appid):
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(client.close)
        client.settimeout(3)
        client.connect(self.sock_path)
        client.sendall(appid + b"\n")
        return client

    def test_override_parent_permissions_are_preserved(self):
        self.assertEqual(self.root.stat().st_mode & 0o7777, 0o700)

    def test_idle_client_burst_is_bounded_and_host_remains_responsive(self):
        with patch.object(control, "CLIENT_TIMEOUT", 10):
            clients = []
            for _ in range(control.MAX_CLIENT_CONNS + 1):
                client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.addCleanup(client.close)
                client.settimeout(3)
                client.connect(self.sock_path)
                clients.append(client)
            self.assertEqual(clients[-1].recv(64), b"venus\n")
            self.host_file.write(b"backend venus\nhdr 1 576 576 50 480\n")
            self.assertEqual(self.host_file.readline(), b"applied venus\n")
            self.assertEqual(self.host_file.readline(), b"applied hdr\n")

    def test_automatic_resolution_multiplexes_and_ignores_stale(self):
        self.host_file.write(b"backend automatic\n")
        self.assertEqual(self.host_file.readline(), b"applied automatic\n")
        self.assertEqual(self.state.read_text(), "automatic\n")
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o644)
        self.assertEqual(os.stat(self.sock_path).st_mode & 0o777, 0o666)
        clients = [self.connect_resolver(str(i).encode()) for i in (1, 2, 3)]
        requests = [self.host_file.readline().decode().split() for _ in clients]
        self.assertEqual([r[2] for r in requests], ["1", "2", "3"])
        for request in requests:
            self.assertRegex(request[1], r"^[0-9a-f]{32}$")
        self.host_file.write(b"resolved 00000000000000000000000000000000 neptune\n"
                             b"backend venus\nhdr 1 576 576 50 480\n")
        self.assertEqual(self.host_file.readline(), b"applied venus\n")
        self.assertEqual(self.host_file.readline(), b"applied hdr\n")
        for index, mode in ((2, "venus"), (0, "neptune"), (1, "mullion")):
            self.host_file.write(f"resolved {requests[index][1]} {mode}\n".encode())
            self.assertEqual(clients[index].recv(64), (mode + "\n").encode())
        self.assertEqual(self.state.read_text(), "venus\n")

    def test_probe_directives_are_forwarded(self):
        for token in ("probe", "probe_nomullion"):
            client = self.connect_resolver(b"42")
            request = self.host_file.readline().split()
            self.host_file.write(b"resolved " + request[1] + b" " + token.encode() + b"\n")
            self.assertEqual(client.recv(64), token.encode() + b"\n")

    def test_invalid_timeout_and_disconnect(self):
        for appid in (b"0", b"01", b"4294967296", b"-1", b"x"):
            self.assertEqual(self.connect_resolver(appid).recv(64), b"venus\n")
        with patch.object(control, "RESOLVE_TIMEOUT", 0.1):
            client = self.connect_resolver(b"1")
            request = self.host_file.readline().split()
            self.assertEqual(client.recv(64), b"venus\n")
            self.host_file.write(b"resolved " + request[1] + b" neptune\n")
        client = self.connect_resolver(b"2")
        self.assertEqual(self.host_file.readline().split()[2], b"2")
        self.host_file.close()
        self.host.close()
        self.assertEqual(client.recv(64), b"venus\n")
        self.thread.join(3)
        self.assertEqual(self.result, [2])
        self.assertFalse(os.path.exists(self.sock_path))

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

    def test_hdr_atomic_publication_and_validation(self):
        target = Path(control.HDR_STATE)
        self.host_file.write(b"hdr 1 576 576 50 480\n")
        self.assertEqual(self.host_file.readline(), b"applied hdr\n")
        self.assertEqual(json.loads(target.read_text()), {
            "supported": True, "max_nits": 576, "max_fall_nits": 576,
            "min_nits_10000ths": 50, "sdr_nits": 480})
        self.assertEqual(target.stat().st_mode & 0o777, 0o644)
        self.assertEqual(target.parent.stat().st_mode & 0o777, 0o755)
        inode = target.stat().st_ino
        self.host_file.write(b"hdr 2 10 10 0 10\nhdr 1 -1 10 0 10\n"
                             b"hdr 1 +1 10 0 10\nhdr 0 0 0 0 0\n")
        self.assertEqual(self.host_file.readline(), b"applied hdr\n")
        self.assertNotEqual(target.stat().st_ino, inode)
        self.assertFalse(json.loads(target.read_text())["supported"])
        self.assertEqual(list(target.parent.iterdir()), [target])

    def test_hdr_uint16_bounds(self):
        target = Path(control.HDR_STATE)
        for index in range(4):
            values = [0, 0, 0, 0]
            values[index] = 65536
            self.host_file.write(("hdr 1 " + " ".join(map(str, values)) + "\n").encode())
        self.host_file.write(b"hdr 1 65535 65535 65535 65535\n")
        self.assertEqual(self.host_file.readline(), b"applied hdr\n")
        self.assertEqual(json.loads(target.read_text()), {
            "supported": True, "max_nits": 65535, "max_fall_nits": 65535,
            "min_nits_10000ths": 65535, "sdr_nits": 65535})
        self.host_file.write(b"backend venus\n")
        self.assertEqual(self.host_file.readline(), b"applied venus\n")

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
        self.thread.join(timeout=5)
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
