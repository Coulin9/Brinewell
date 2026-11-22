#!/usr/bin/env python3
"""Regression checks for the privileged installer's download and path boundary."""
import importlib.util
import io
import hashlib
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).parents[1] / 'runtime/guest/rootfs/usr/lib/brinewell/uu-install.py'
spec = importlib.util.spec_from_file_location('uu_installer', SOURCE)
installer = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(installer)


class InstallerBoundaryTests(unittest.TestCase):
    def test_official_payload_url_is_narrow(self):
        good = 'https://uurouter.gdl.netease.com/uuplugin/steam-deck-plugin-x86_64/v14.12.2/uu.tar.gz?key1=example'
        self.assertTrue(installer.trusted_payload_url(good))
        for url in (good.replace('https:', 'http:'), good.replace('.netease.com', '.netease.com.attacker.test'),
                    good.replace('/uu.tar.gz', '/../install.sh'), good.replace('https://', 'https://user:password@'),
                    good.replace('steam-deck-plugin-x86_64', 'another-product')):
            self.assertFalse(installer.trusted_payload_url(url))

    def test_payload_rejects_links_traversal_and_wrong_architecture(self):
        for bad in ('../uuplugin', 'symlink', 'arm64'):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / 'fixture.tar.gz'
                header = b'\x7fELF\x02\x01' + bytes(12) + (b'\xb7\x00' if bad == 'arm64' else b'\x3e\x00')
                with tarfile.open(archive, 'w:gz') as package:
                    for name in installer.MEMBERS:
                        info = tarfile.TarInfo('../uuplugin' if bad == '../uuplugin' and name == 'uuplugin' else name)
                        if bad == 'symlink' and name == 'uuplugin':
                            info.type = tarfile.SYMTYPE
                            info.linkname = '/etc/passwd'
                            package.addfile(info)
                        else:
                            content = b'log_level=info\n' if name == 'uu.conf' else header
                            info.size = len(content)
                            package.addfile(info, io.BytesIO(content))
                data = archive.read_bytes()
                metadata = ('https://uurouter.gdl.netease.com/uuplugin/steam-deck-plugin-x86_64/v14.12.2/uu.tar.gz,'
                            + hashlib.md5(data).hexdigest() + ',')
                def download(url, destination, _maximum):
                    destination.write_bytes(metadata.encode() if url == installer.METADATA else data)
                with patch.object(installer, 'download', side_effect=download), self.assertRaises(RuntimeError):
                    installer.fetch_payload(root)
                self.assertFalse((root.parent / 'uuplugin').exists())

    def test_directory_symlink_and_writable_path_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / 'target'
            target.mkdir()
            link = root / 'link'
            link.symlink_to(target, target_is_directory=True)
            with self.assertRaises(RuntimeError):
                installer.owned_directory(link)
            target.chmod(0o777)
            with self.assertRaises(RuntimeError):
                installer.owned_directory(target)

    def test_absent_service_is_never_reported_installed(self):
        with patch.object(installer, 'run') as run:
            run.return_value.returncode = 0
            run.return_value.stdout = '0\ninactive\n'
            self.assertFalse(installer.active())


    def test_launcher_is_recognized_as_starting(self):
        with patch.object(installer, 'run') as run, \
             patch.object(Path, 'read_bytes', return_value=b'/bin/bash\0/usr/lib/brinewell/uu-launch\0'), \
             patch.object(Path, 'read_text', return_value='0::/system.slice/uuplugin.service'):
            run.return_value.returncode = 0
            run.return_value.stdout = '120\nactive\n'
            self.assertEqual(installer.service_state(), 'starting')

    def test_legacy_monitor_tracks_its_child_across_cgroups(self):
        with tempfile.TemporaryDirectory() as temporary:
            proc = Path(temporary)
            monitor = proc / '1424'
            child = proc / '1504'
            monitor.mkdir()
            child.mkdir()
            (monitor / 'cmdline').write_bytes(b'/bin/bash\0/home/uu/uu/uuplugin_monitor.sh\0')
            (monitor / 'cgroup').write_text('0::/system.slice/uuplugin.service\n')
            (child / 'cmdline').write_bytes(b'/tmp/uu/uuplugin\0/tmp/uu/uu.conf\0')
            (child / 'cgroup').write_text('0::/xuplugin-steam-deck\n')
            def path(value):
                return proc / str(value).removeprefix('/proc/') if str(value) != '/proc' else proc
            with patch.object(installer, 'Path', side_effect=path), patch.object(installer, 'run') as run:
                run.return_value.returncode = 0
                run.return_value.stdout = '1424\nactive\n'
                for parent, expected in ((1424, 'running'), (999, 'starting')):
                    with self.subTest(parent=parent):
                        (child / 'status').write_text(f'Name:\tFEX\nPPid:\t{parent}\n')
                        self.assertEqual(installer.service_state(), expected)
                (child / 'status').unlink()
                self.assertEqual(installer.service_state(), 'starting')

    def test_status_waits_briefly_for_launcher_exec(self):
        with patch.object(installer, 'service_state', side_effect=['starting', 'running']), \
             patch.object(installer.time, 'sleep') as sleep, \
             patch.object(installer, 'binding', return_value=None), \
             patch.object(installer, 'emit') as emit:
            self.assertTrue(installer.report_status())
            sleep.assert_called_once_with(0.2)
            self.assertTrue(emit.call_args.args[1].startswith('installed '))

    def test_stopped_service_and_start_timeout_are_not_hidden(self):
        with patch.object(installer, 'service_state', return_value='stopped'), \
             patch.object(installer, 'UNIT') as unit, \
             patch.object(installer.time, 'sleep') as sleep, \
             patch.object(installer, 'emit') as emit:
            unit.exists.return_value = True
            self.assertFalse(installer.report_status())
            sleep.assert_not_called()
            self.assertTrue(emit.call_args.args[1].startswith('error '))
        with patch.object(installer, 'service_state', return_value='starting'), \
             patch.object(installer.time, 'monotonic', side_effect=[0, 0, 3]), \
             patch.object(installer.time, 'sleep') as sleep, \
             patch.object(installer, 'emit') as emit:
            self.assertFalse(installer.report_status())
            sleep.assert_called_once_with(0.2)
            self.assertEqual(emit.call_args.args[1], 'error UU 正在启动，请稍后刷新')


if __name__ == '__main__':
    unittest.main()
