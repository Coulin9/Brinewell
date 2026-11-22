import os
from pathlib import Path
import pty
import select
import signal
import socket
import subprocess
import time
import tty
import unittest

GAME_FRAME_SOCKET = "\0brinewell.game-frame.v1"
GAME_FRAME = b"game-frame 1 neptune 321 42 1920 1080 1 15 250000000\n"


@unittest.skipUnless(Path("/agent").is_file(), "run in the Linux builder with the guest agent mounted at /agent")
class AgentShutdownTests(unittest.TestCase):
    def setUp(self):
        self.ports = Path("/dev/virtio-ports")
        self.ports.mkdir(exist_ok=True)
        for name in ["fx.pad", "fx.progress", "fx.telemetry"]:
            (self.ports / name).unlink(missing_ok=True)
        self.children = []
        self.fds = []

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait()
        for fd in self.fds:
            os.close(fd)
        for name in ["fx.pad", "fx.progress", "fx.telemetry"]:
            (self.ports / name).unlink(missing_ok=True)

    def start(self, mode, port=True):
        master = None
        if port:
            master, slave = pty.openpty()
            self.fds += [master, slave]
            tty.setraw(slave)
            (self.ports / ("fx.pad" if mode == "pad" else "fx.progress")).symlink_to(os.ttyname(slave))
        child = subprocess.Popen(["/agent"] + ([mode] if mode else []))
        self.children.append(child)
        if port:
            self.assertTrue(select.select([master], [], [], 2)[0], "agent did not announce readiness")
            self.assertEqual(os.read(master, 100), b"hello\n" if mode == "pad" else b"ready\n")
        return child, master

    def stop(self, child, sig=signal.SIGTERM):
        child.send_signal(sig)
        self.assertEqual(child.wait(timeout=1), 0)

    def test_pad_exits_without_more_input(self):
        child, _ = self.start("pad")
        self.stop(child)

    def test_pad_exits_with_partial_line(self):
        child, master = self.start("pad")
        os.write(master, b"ev 1:304:")
        time.sleep(0.05)
        self.stop(child)

    def test_pad_accepts_fragmented_and_multiple_lines_then_sigint(self):
        child, master = self.start("pad")
        for data in [b"des", b"troy\r\nunknown\ndestroy\n"]:
            os.write(master, data)
        time.sleep(0.05)
        self.assertIsNone(child.poll())
        self.stop(child, signal.SIGINT)

    def test_stop_during_port_retry(self):
        child, _ = self.start("pad", port=False)
        time.sleep(0.05)
        self.stop(child)

    def test_progress_exits_on_sigterm(self):
        child, _ = self.start("")
        self.stop(child)

    def read_progress(self, master, length):
        data = b""
        deadline = time.monotonic() + 2
        while len(data) < length and time.monotonic() < deadline:
            if select.select([master], [], [], max(0, deadline - time.monotonic()))[0]:
                data += os.read(master, 4096)
        return data

    def test_progress_relays_multiple_senders_and_releases_socket_on_stop(self):
        child, master = self.start("")
        other = GAME_FRAME.replace(b"neptune 321", b"vulkan 322")
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as first, \
                socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as second:
            first.connect(GAME_FRAME_SOCKET)
            second.connect(GAME_FRAME_SOCKET)
            first.send(GAME_FRAME)
            second.send(other)
            self.assertEqual(self.read_progress(master, len(GAME_FRAME + other)), GAME_FRAME + other)
            self.stop(child)
            with self.assertRaises(OSError):
                first.send(GAME_FRAME)
        # Abstract names disappear when the receiver closes, without filesystem cleanup.
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as replacement:
            replacement.bind(GAME_FRAME_SOCKET)

    def test_progress_drops_malformed_and_truncated_datagrams(self):
        child, master = self.start("")
        # Version 1 retains its smaller limit even when version 2 needs more room.
        exact_limit = GAME_FRAME.replace(b"321", b"0" * (256 - len(GAME_FRAME)) + b"321", 1)
        self.assertEqual(len(exact_limit), 256)
        malformed = [b"ready\n", GAME_FRAME[:-1], GAME_FRAME + b"ready\n", b"\0" + GAME_FRAME,
                     GAME_FRAME.replace(b"neptune", b"unknown"), GAME_FRAME.replace(b"1 neptune", b"2 neptune"),
                     GAME_FRAME.replace(b"321", b"-1"), GAME_FRAME.replace(b"321", b"18446744073709551616"),
                     exact_limit + b"extra", b"x" * 4096]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
            sender.settimeout(1)
            sender.connect(GAME_FRAME_SOCKET)
            for packet in malformed:
                sender.send(packet)
            sender.send(GAME_FRAME)
            self.assertEqual(self.read_progress(master, len(GAME_FRAME)), GAME_FRAME)
        self.stop(child)

    def test_progress_preserves_real_frame_history_and_rejects_invalid_arrays(self):
        child, master = self.start("")
        prefix = b"game-frame 2 vulkan 321 42 1920 1080 1 "
        complete = prefix + b"128 256000000 1 1 " + b",".join([b"2000000"] * 128) + b"\n"
        unavailable = prefix + b"300 250000000 2 0 -\n"
        unfocused = b"game-frame 2 vulkan 321 42 1920 1080 0 0 0 3 0 -\n"
        self.assertGreater(len(complete), 256)
        self.assertLessEqual(len(complete), 4096)
        exact_limit = complete.replace(b"321", b"0" * (4096 - len(complete)) + b"321", 1)
        self.assertEqual(len(exact_limit), 4096)
        malformed = [
            prefix + b"2 3 1 1 1\n", prefix + b"2 3 1 1 1,2,\n",
            prefix + b"2 3 1 1 1,1\n", prefix + b"2 3 1 1 0,3\n",
            prefix + b"2 3 1 1 -1,4\n", prefix + b"2 3 1 1 +1,2\n",
            prefix + b"1 3 0 1 3\n", prefix + b"1 3 1 0 3\n",
            prefix + b"2 1 1 1 18446744073709551615,2\n",
            prefix + b"129 129 1 1 " + b",".join([b"1"] * 129) + b"\n",
            unfocused.replace(b"0 0 0 3", b"0 1 1 3"),
            exact_limit + b"extra",
        ]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
            sender.settimeout(1)
            sender.connect(GAME_FRAME_SOCKET)
            for packet in malformed:
                sender.send(packet)
            for packet in [complete, unavailable, unfocused]:
                sender.send(packet)
            expected = complete + unavailable + unfocused
            self.assertEqual(self.read_progress(master, len(expected)), expected)
        self.stop(child)

    def test_progress_boots_when_optional_game_frame_socket_is_unavailable(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as occupied:
            occupied.bind(GAME_FRAME_SOCKET)
            child, _ = self.start("")
            self.assertIsNone(child.poll())
            self.stop(child)

    def test_progress_stops_under_output_backpressure(self):
        child, _ = self.start("")
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
            sender.settimeout(0.1)
            sender.connect(GAME_FRAME_SOCKET)
            for _ in range(2000):
                try:
                    sender.send(GAME_FRAME)
                except socket.timeout:
                    break
            self.stop(child)

    def test_telemetry_publishes_native_power_and_overlay_snapshot_then_expires(self):
        power = Path("/run/power-monitor/power/cores_total")
        gpu_power = power.with_name("gfx")
        snapshot = Path("/run/brinewell-telemetry/snapshot")
        for path in [power, gpu_power, snapshot]:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.unlink(missing_ok=True)
        master, slave = pty.openpty()
        self.fds += [master, slave]
        tty.setraw(slave)
        (self.ports / "fx.telemetry").symlink_to(os.ttyname(slave))
        child = subprocess.Popen(["/agent", "telemetry"])
        self.children.append(child)

        self.assertTrue(select.select([master], [], [], 2)[0])
        command, request = os.read(master, 100).split()
        self.assertEqual(command, b"hardware")
        totals = b"3125000 42000 45000 250000 338000000 0 2147483648 1123 2532 8589934592 68719476736"
        cores = b"32 " + b" ".join(f"P 0 {core} 4380 2500 55000".encode() for core in range(32))
        reading = totals + b" " + cores
        os.write(master, b"hardware " + str(int(request) - 1).encode() + b" " + reading + b"\n")
        os.write(master, b"hardware " + request + b" " + totals + b" 2 P 0 0 4380 2500 55000\n")
        time.sleep(0.1)
        self.assertFalse(power.exists(), "a stale or malformed reply was published")
        self.assertFalse(snapshot.exists(), "a stale or malformed snapshot was published")
        os.write(master, b"hardware " + request + b" " + reading[:256])
        os.write(master, reading[256:512])
        os.write(master, reading[512:] + b"\n")
        deadline = time.monotonic() + 0.8
        while not snapshot.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(power.read_text(), "3.125000\n")
        self.assertEqual(gpu_power.read_text(), "0.250000\n")
        self.assertEqual(snapshot.read_bytes(),
                         b"2 0 338000000 45000 250000 2147483648 42000 3125000 1123 2532 8589934592 68719476736 "
                         + cores + b"\n")

        deadline = time.monotonic() + 4
        while (power.exists() or gpu_power.exists() or snapshot.exists()) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(power.exists(), "stopped telemetry left a stale native reading")
        self.assertFalse(gpu_power.exists(), "stopped telemetry left a stale GPU reading")
        self.assertFalse(snapshot.exists(), "stopped telemetry left a stale overlay snapshot")
        self.stop(child)


if __name__ == "__main__":
    unittest.main()
