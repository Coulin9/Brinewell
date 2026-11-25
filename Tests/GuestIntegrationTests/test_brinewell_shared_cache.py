import importlib.machinery
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOTFS = Path(__file__).resolve().parents[2] / 'runtime/guest/rootfs'
SCRIPT = ROOTFS / 'usr/lib/brinewell/brinewell-shared-cache'
loader = importlib.machinery.SourceFileLoader('shared_cache', str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
cache = importlib.util.module_from_spec(spec)
loader.exec_module(cache)


class SharedCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.mount = self.base / 'mount'
        self.mount.mkdir()
        self.home = self.base / 'home'
        self.home.mkdir()
        self.info = self.base / 'mountinfo'
        self.meta = self.base / 'meta'
        self.sid = '0123456789abcdef' * 2
        self.write_mounts()

    def write_mounts(self, home_fs='ext4', extra=''):
        self.info.write_text(f'1 0 8:1 / / rw - ext4 /dev/root rw\n'
                             f'2 1 8:2 / {self.home} rw - {home_fs} /dev/home rw\n'
                             f'3 1 0:5 / {self.mount} rw - virtiofs brinewell-share rw\n' + extra)

    def run_setup(self):
        calls = []
        def bind(source, target):
            calls.append((os.fstat(source), os.fstat(target)))
        with mock.patch.object(cache, 'metadata', return_value=(self.sid, self.mount)), \
             mock.patch.object(cache.pwd, 'getpwnam', return_value=mock.Mock(pw_uid=123, pw_gid=456)), \
             mock.patch.object(cache.os, 'fchown') as chown:
            self.assertEqual(cache.setup(self.meta, self.info, self.home, bind), 0)
        return calls, chown

    def test_root_nested_and_first_boot_bind(self):
        for layout in ('root', 'nested', 'empty'):
            with self.subTest(layout=layout), tempfile.TemporaryDirectory() as tmp:
                self.mount = Path(tmp).resolve() / 'mount'
                self.mount.mkdir()
                self.write_mounts()
                apps = self.mount / ('SteamLibrary/steamapps' if layout == 'nested' else 'steamapps')
                if layout != 'empty':
                    apps.mkdir(parents=True)
                calls, chown = self.run_setup()
                source = self.home / 'steamos/.local/share/brinewell/shared-shadercache' / (self.sid + ('-SteamLibrary' if layout == 'nested' else ''))
                self.assertEqual(len(calls), 1)
                self.assertEqual((calls[0][0].st_dev, calls[0][0].st_ino), (source.stat().st_dev, source.stat().st_ino))
                self.assertEqual((calls[0][1].st_dev, calls[0][1].st_ino), ((apps / 'shadercache').stat().st_dev, (apps / 'shadercache').stat().st_ino))
                self.assertTrue(all(call.args[1:] == (123, 456) for call in chown.call_args_list))

    def test_metadata_validation(self):
        self.assertIsNone(cache.metadata(self.meta))
        def write(sid, mode, mount):
            self.meta.write_text(f'ID={sid}\nMODE={mode}\nMOUNT={mount}\n')
        good = '/run/media/bw-' + self.sid[:8]
        write(self.sid, 'rw', good)
        self.assertEqual(cache.metadata(self.meta), (self.sid, Path(good)))
        for sid, mode, mount in [(self.sid, 'ro', good), ('X' * 32, 'rw', good), (self.sid, 'rw', '/tmp/no')]:
            write(sid, mode, mount)
            self.assertIsNone(cache.metadata(self.meta))
        self.meta.unlink()
        self.meta.symlink_to('/dev/null')
        self.assertIsNone(cache.metadata(self.meta))

    def test_missing_ambiguous_and_symlinks(self):
        (self.mount / 'unrelated').touch()
        calls, _ = self.run_setup()
        self.assertFalse(calls)
        (self.mount / 'steamapps').mkdir()
        (self.mount / 'SteamLibrary/steamapps').mkdir(parents=True)
        calls, _ = self.run_setup()
        self.assertFalse(calls)
        (self.mount / 'SteamLibrary/steamapps').rmdir()
        (self.mount / 'steamapps/shadercache').symlink_to(self.home)
        calls, _ = self.run_setup()
        self.assertFalse(calls)

    def test_nonlocal_home_refused_before_mutation(self):
        (self.mount / 'steamapps').mkdir()
        for fs in ('virtiofs', 'tmpfs', 'ramfs', 'overlay', 'nfs', 'fuse.test'):
            self.write_mounts(home_fs=fs)
            with mock.patch.object(cache.os, 'fchmod') as chmod:
                calls, chown = self.run_setup()
                self.assertFalse(calls)
                chmod.assert_not_called()
                chown.assert_not_called()

    def test_unexpected_mount_refused(self):
        target = self.mount / 'steamapps/shadercache'
        target.mkdir(parents=True)
        self.write_mounts(extra=f'4 3 0:9 / {target} rw - tmpfs tmpfs rw\n')
        calls, _ = self.run_setup()
        self.assertFalse(calls)
        self.assertFalse((self.home / 'steamos').exists())

    def test_already_bound(self):
        target = self.mount / 'steamapps/shadercache'
        target.mkdir(parents=True)
        source = self.home / 'steamos/.local/share/brinewell/shared-shadercache' / self.sid
        source.mkdir(parents=True)
        self.write_mounts(extra=f'4 3 8:2 /cache {target} rw - ext4 /dev/home rw\n')
        actual = cache.os.fstat
        def fake(fd):
            result = actual(fd)
            return source.stat() if result.st_ino == target.stat().st_ino else result
        with mock.patch.object(cache.os, 'fstat', side_effect=fake):
            calls, chown = self.run_setup()
        self.assertFalse(calls)
        chown.assert_not_called()

    def test_service_enabled_and_ordered(self):
        system = ROOTFS / 'usr/lib/systemd/system'
        unit = (system / 'brinewell-shared-cache.service').read_text()
        self.assertIn('Before=display-manager.service', unit)
        self.assertNotIn('Before=user@.service', unit)
        self.assertIn('After=brinewell-shared-cache.service', (system / 'user@.service.d/50-brinewell-shared-cache.conf').read_text())
        self.assertEqual(os.readlink(system / 'multi-user.target.wants/brinewell-shared-cache.service'), '../brinewell-shared-cache.service')
        self.assertEqual(SCRIPT.stat().st_mode & 0o777, 0o755)
