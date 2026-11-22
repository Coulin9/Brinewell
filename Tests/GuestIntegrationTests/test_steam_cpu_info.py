import base64
import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "runtime/guest/rootfs/usr/lib/brinewell/steam-cpu-info"
loader = importlib.machinery.SourceFileLoader("steam_cpu_info", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
cpu_info = importlib.util.module_from_spec(spec)
loader.exec_module(cpu_info)

def cmdline(model, frequency):
    encoded = base64.b64encode(model.encode()).decode()
    return f"quiet brinewell.cpu_model_b64={encoded} brinewell.cpu_max_mhz={frequency}"


class SteamCPUInfoTests(unittest.TestCase):
    def test_model_and_frequency_are_parsed_independently(self):
        self.assertEqual(cpu_info.host_cpu_info(cmdline("Apple M5 Max", 4608)), ("Apple M5 Max", "4608"))
        self.assertEqual(cpu_info.host_cpu_info(cmdline("Apple M5 Max", -1)), ("Apple M5 Max", ""))
        self.assertEqual(cpu_info.host_cpu_info(cmdline("bad\nmodel", 4608)), ("", "4608"))
        self.assertEqual(cpu_info.host_cpu_info("brinewell.cpu_model_b64=! brinewell.cpu_max_mhz=99999"), ("", ""))

    def test_prepares_steam_start_and_clears_stale_frequency(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = Path(directory) / "brinewell-steam-cpu.env"
            cpu_info.configure_frequency("4608", environment)
            self.assertEqual(environment.read_text(), "CPU_MHZ=4608\n")
            inode = environment.stat().st_ino
            cpu_info.configure_frequency("4608", environment)
            self.assertEqual(environment.stat().st_ino, inode)
            cpu_info.configure_frequency("", environment)
            self.assertFalse(environment.exists())

    def test_selects_gamepad_window_without_using_localized_title(self):
        gamepad_url = "about:blank?browserType=4&useragent=Valve%20Steam%20Gamepad"
        target = "A123456789ABCDEF" * 2
        pages = [
            {"id": "B" * 32, "type": "page", "url": "https://steamloopback.host/routes/settings"},
            {"id": "C" * 32, "type": "page", "url": gamepad_url.replace("browserType=4", "browserType=2")},
            {"id": "D" * 32, "type": "page", "url": "https://example.org/?" + gamepad_url.split("?")[1]},
            {"id": target, "type": "page", "url": gamepad_url, "title": "Steam 大屏幕模式"},
        ]
        self.assertIsNone(cpu_info.gamepad_target(pages[:-1]))
        self.assertEqual(cpu_info.gamepad_target(pages), target)


if __name__ == "__main__":
    unittest.main()
