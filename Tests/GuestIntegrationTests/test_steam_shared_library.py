from pathlib import Path
import importlib.machinery
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from urllib.parse import quote


ROOTFS = Path(__file__).resolve().parents[2] / "runtime/guest/rootfs"
SCRIPTS = ROOTFS / "usr/lib/brinewell"


class SharedLibraryHookTests(unittest.TestCase):
    def test_service_prepares_before_title_defaults(self):
        service = (ROOTFS / "usr/lib/systemd/user/steam.service.d/50-brinewell.conf").read_text()
        prepare = "ExecStartPre=-/usr/bin/timeout 12 /usr/lib/brinewell/steam-shared-library prepare"
        self.assertIn(prepare, service)
        self.assertLess(service.index(prepare), service.index("steam-title-defaults"))

    def test_watch_notifies_before_signin_guard(self):
        client = (SCRIPTS / "steam-client").read_text()
        watch = client.split('if [[ ${1:-} == --watch ]]; then', 1)[1].split('\nfi\n', 1)[0]
        self.assertLess(watch.index("steam-shared-library notify"), watch.index("[[ -f $signin_marker"))

    def test_desktop_prepares_and_notifies_before_exec(self):
        client = (SCRIPTS / "steam-client").read_text()
        desktop = client.split('if [[ $desktop == 1 ]]; then', 1)[1].split('\nfi\n', 1)[0]
        self.assertLess(desktop.index("steam-shared-library prepare"), desktop.index("steam-shared-library notify"))
        self.assertLess(desktop.index("steam-shared-library notify"), desktop.index("exec /usr/bin/steam"))


# Load the installed, extensionless guest script as a module without running main.
sys.path.insert(0, str(SCRIPTS))
loader = importlib.machinery.SourceFileLoader('steam_shared_library', str(SCRIPTS / 'steam-shared-library'))
spec = importlib.util.spec_from_loader(loader.name, loader)
shared = importlib.util.module_from_spec(spec)
loader.exec_module(shared)


class SharedLibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.mount = self.base / 'share with space'
        self.mount.mkdir()
        self.meta = self.base / 'share.meta'
        self.mountinfo = self.base / 'mountinfo'
        self.steam = self.base / 'Steam'
        self.state = self.base / 'state'
        self.state.mkdir()
        self.sid = '0123456789abcdef' * 2
        self.share = {'share_id': self.sid, 'mount_path': str(self.mount),
                      'label': 'My Library', 'mode': 'rw'}
        self.env = mock.patch.dict(os.environ, BRINEWELL_STEAM_ROOT=str(self.steam),
                                   BRINEWELL_STATE_DIR=str(self.state),
                                   BRINEWELL_BOOT_ID='test-boot',
                                   BRINEWELL_STEAM_RUNNING='0')
        self.env.start()
        self.addCleanup(self.env.stop)

    def write_meta(self, sid=None, label='Library', **changes):
        sid = self.sid if sid is None else sid
        fields = {'ID': sid, 'LABEL_HEX': label.encode().hex(),
                  'MOUNT': '/run/media/brinewell-share-' + sid, 'MODE': 'rw'}
        fields.update(changes)
        self.meta.write_text(''.join(f'{k}={v}\n' for k, v in fields.items()))
        return shared.load_share_meta(self.meta)

    def mount_line(self, fs='virtiofs', source='brinewell-share', opts='rw', superopts='rw'):
        path = str(self.mount).replace(' ', r'\040')
        return f'1 0 0:1 / {path} {opts} - {fs} {source} {superopts}\n'

    def registry_text(self, *entries):
        blocks = ['"0" { "path" "/home/steam" "contentid" "1" }']
        for index, path, contentid in entries:
            blocks.append(f'"{index}" {{ "path" {shared.vdf.vdf_quote(str(path))} "contentid" "{contentid}" }}')
        return '"libraryfolders"\n{\n // keep formatting\n ' + '\n '.join(blocks) + '\n}\n'

    def write_registry(self, text):
        master = self.steam / 'config/libraryfolders.vdf'
        master.parent.mkdir(parents=True, exist_ok=True)
        master.write_text(text)
        return master

    def owned(self, status='confirmed', contentid='123', sid=None):
        return {'share_id': sid or self.sid, 'mount_path': str(self.mount),
                'library_path': str(self.mount), 'contentid': contentid, 'status': status}

    def ledger(self, owned=None, suppressed=None):
        shared.save_ledger(self.state, {'version': 1, 'owned': owned, 'suppressed': suppressed})

    def prepare(self, share=None, library=None):
        if share is None:
            share = self.share
        if library is None:
            library = str(self.mount)
        with mock.patch.object(shared, 'eligible_share', return_value=(share, library)):
            return shared.cmd_prepare()

    def test_metadata_labels_and_validation(self):
        for label in ('ASCII', '中文文件夹', ' a\x01b\x7f ', '中' * 17, ''):
            with self.subTest(label=label):
                result = self.write_meta(label=label)
                expected = ''.join(c for c in label if ord(c) >= 32 and ord(c) != 127).strip()
                expected = expected.encode()[:48].decode('utf-8', errors='ignore') or 'Shared Folder'
                self.assertEqual(result['label'], expected)
        for sid in ('a' * 31, 'A' * 32, 'g' * 32, '../' + 'a' * 29):
            with self.subTest(sid=sid):
                self.assertIsNone(self.write_meta(sid=sid))
        for changes in ({'MOUNT': '/tmp/other'}, {'LABEL_HEX': 'a'},
                        {'LABEL_HEX': 'zz'}, {'LABEL_HEX': 'ff'}, {'MODE': 'bad'}):
            with self.subTest(changes=changes):
                self.assertIsNone(self.write_meta(**changes))
        self.meta.unlink()
        self.assertIsNone(shared.load_share_meta(self.meta))

    def test_mountinfo_validation_and_overmounts(self):
        good = self.mount_line()
        bad = self.mount_line(fs='tmpfs')
        self.mountinfo.write_text(bad + good)
        self.assertTrue(shared.verify_mountinfo(self.mount, self.mountinfo))
        self.mountinfo.write_text(good + bad)
        self.assertFalse(shared.verify_mountinfo(self.mount, self.mountinfo))
        for line in (bad, self.mount_line(source='other'), self.mount_line(opts='ro'),
                     self.mount_line(superopts='ro'), self.mount_line(opts='rw,noexec'),
                     self.mount_line(superopts='rw,noexec')):
            with self.subTest(line=line):
                self.mountinfo.write_text(line)
                self.assertFalse(shared.verify_mountinfo(self.mount, self.mountinfo))
        link = self.base / 'link'
        link.symlink_to(self.mount)
        self.mountinfo.write_text(good.replace(str(self.mount).replace(' ', r'\040'), str(link)))
        self.assertFalse(shared.verify_mountinfo(link, self.mountinfo))
        with mock.patch.object(shared, 'load_share_meta', return_value={**self.share, 'mode': 'ro'}), \
             mock.patch.object(shared, 'verify_mountinfo') as verify:
            self.assertEqual(shared.eligible_share({}), (None, None))
            verify.assert_not_called()

    def test_root_selection_matrix(self):
        choose = lambda ledger={}: shared.select_library_root(self.mount, ledger, self.sid)
        self.assertEqual(choose(), str(self.mount))
        for name in ('.DS_Store', '._.DS_Store', '.Spotlight-V100', '.Trashes'):
            (self.mount / name).touch()
        self.assertEqual(choose(), str(self.mount))
        (self.mount / 'document').touch()
        child = self.mount / 'SteamLibrary'
        self.assertEqual(choose(), str(child))
        child.mkdir()
        (child / 'unrelated').touch()
        self.assertIsNone(choose())
        (child / 'unrelated').unlink()
        (child / 'steamapps').mkdir()
        self.assertEqual(choose(), str(child))
        (self.mount / 'steamapps').mkdir()
        self.assertIsNone(choose())
        (child / 'libraryfolder.vdf').write_text('"libraryfolder" { "contentid" "123" }')
        self.assertEqual(choose({'owned': {**self.owned(), 'library_path': str(child)}}), str(child))
        self.assertIsNone(choose({'owned': {**self.owned(), 'library_path': str(child), 'contentid': '999'}}))

    def test_root_libraryfolder_and_symlinks(self):
        folder = self.mount / 'libraryfolder.vdf'
        folder.write_text('"libraryfolder" { "contentid" "123" }')
        self.assertEqual(shared.select_library_root(self.mount, {}, self.sid), str(self.mount))
        folder.unlink()
        target = self.base / 'target'
        target.mkdir()
        for name in ('steamapps', 'libraryfolder.vdf', 'SteamLibrary'):
            with self.subTest(name=name):
                link = self.mount / name
                link.symlink_to(target)
                self.assertIsNone(shared.select_library_root(self.mount, {}, self.sid))
                link.unlink()

    def test_probe_success_and_failures_cleanup(self):
        self.assertTrue(shared.probe_writable_target(str(self.mount), str(self.mount)))
        self.assertEqual(list(self.mount.iterdir()), [])
        for target, kwargs in (('subprocess.run', {'side_effect': PermissionError('no exec')}),
                               ('subprocess.run', {'return_value': subprocess.CompletedProcess([], 1)}),
                               ('os.symlink', {'side_effect': OSError('symlink')}),
                               ('os.link', {'side_effect': OSError('hardlink')}),
                               ('os.statvfs', {'side_effect': OSError('statvfs')})):
            with self.subTest(target=target, kwargs=kwargs), mock.patch.object(shared, 'log'):
                with mock.patch(target, **kwargs):
                    self.assertFalse(shared.probe_writable_target(str(self.mount), str(self.mount)))
                self.assertEqual(list(self.mount.iterdir()), [])
        real = os.statvfs(self.mount)
        fake = mock.Mock(f_blocks=real.f_blocks, f_frsize=real.f_frsize,
                         f_bsize=real.f_bsize, f_bavail=0)
        with mock.patch.object(shared.os, 'statvfs', return_value=fake), mock.patch.object(shared, 'log') as log:
            self.assertTrue(shared.probe_writable_target(str(self.mount), str(self.mount)))
            self.assertIn('no available space', str(log.call_args))
            self.assertEqual(fake.f_bavail, 0)
        self.assertEqual(list(self.mount.iterdir()), [])

    def test_initialization_preserves_existing_and_avoids_collisions(self):
        label = 'quotes " and \\ slash'
        with mock.patch.object(shared.secrets, 'randbits', side_effect=[0, 7, 42]):
            cid = shared.ensure_library_initialized(str(self.mount), label, {'7'})
        self.assertEqual(cid, '42')
        self.assertTrue(0 < int(cid) < 2**63)
        self.assertTrue((self.mount / 'steamapps').is_dir())
        folder = self.mount / 'libraryfolder.vdf'
        self.assertEqual(shared.read_libraryfolder(folder), {'contentid': cid, 'label': label})
        original = folder.read_text().replace('quotes', 'edited')
        folder.write_text(original)
        self.assertEqual(shared.ensure_library_initialized(str(self.mount), 'new', set()), cid)
        self.assertEqual(folder.read_text(), original)
        folder.unlink()
        folder.symlink_to(self.base / 'elsewhere')
        self.assertIsNone(shared.ensure_library_initialized(str(self.mount), label, set()))

    def test_real_pid_detection_and_fifo_readiness(self):
        dot = self.base / '.steam'
        dot.mkdir()
        pidfile = dot / 'steam.pid'
        proc = self.base / 'proc'
        (proc / '123').mkdir(parents=True)
        (proc / '123/comm').write_text('steam\n')
        original = self.registry_text(('1', self.mount, '123'))
        master = self.write_registry(original)
        self.ledger(self.owned())
        with mock.patch.dict(os.environ, {'HOME': str(self.base)}), \
             mock.patch.dict(os.environ, {'BRINEWELL_STEAM_RUNNING': ''}), \
             mock.patch.object(shared, 'Path', side_effect=lambda p: proc if p == '/proc' else Path(p)) as path:
            del os.environ['BRINEWELL_STEAM_RUNNING']
            path.home.return_value = self.base
            pidfile.write_text('corrupt pid\n')
            self.assertEqual(shared.get_steam_state(self.steam, dot), 'unknown')
            with mock.patch.object(shared, 'eligible_share', return_value=(None, None)):
                self.assertEqual(shared.cmd_prepare(), 0)
            self.assertEqual(master.read_text(), original)
            self.assertEqual(shared.load_ledger(self.state)['owned'], self.owned())

            pidfile.write_text('123\n')
            self.assertEqual(shared.get_steam_state(self.steam, dot), 'running')
            pipe = dot / 'steam.pipe'
            self.assertFalse(shared.is_steam_ipc_ready(self.steam, dot))
            pipe.touch()
            self.assertFalse(shared.is_steam_ipc_ready(self.steam, dot))
            pipe.unlink()
            os.mkfifo(pipe)
            self.assertTrue(shared.is_steam_ipc_ready(self.steam, dot))

    def test_notify_waits_through_steam_bootstrap_turnover(self):
        self.prepare()
        cid = shared.load_ledger(self.state)['owned']['contentid']
        dot = self.base / '.steam'
        dot.mkdir()
        pidfile = dot / 'steam.pid'
        pipe = dot / 'steam.pipe'
        proc = self.base / 'proc'
        for pid in ('123', '456'):
            (proc / pid).mkdir(parents=True)
        master = self.steam / 'config/libraryfolders.vdf'
        polls = []
        calls = []

        def sleep(_):
            if calls:
                return
            polls.append(shared.get_steam_state(self.steam, dot))
            if len(polls) == 1:
                (proc / '123/comm').write_text('steam\n')
                pidfile.write_text('123\n')
            elif len(polls) == 2:
                pidfile.unlink()
                (proc / '123/comm').unlink()
            elif len(polls) == 3:
                (proc / '456/comm').write_text('steam\n')
                pidfile.write_text('456\n')
            elif len(polls) == 4:
                os.mkfifo(pipe)
            elif len(polls) == 5:
                self.write_registry(self.registry_text())

        def steam_run(argv, **kwargs):
            calls.append(argv)
            master.write_text(self.registry_text(('1', self.mount, cid)))
            return subprocess.CompletedProcess(argv, 0)

        with mock.patch.dict(os.environ, BRINEWELL_NOTIFY_TIMEOUT='2', BRINEWELL_NOTIFY_INTERVAL='0.01',
                             BRINEWELL_STEAM_BIN='steam', BRINEWELL_STEAM_DOT_DIR=str(dot)), \
             mock.patch.object(shared, 'eligible_share', return_value=(self.share, str(self.mount))), \
             mock.patch.object(shared, 'Path', side_effect=lambda p: proc if p == '/proc' else Path(p)), \
             mock.patch.object(shared.time, 'sleep', side_effect=sleep), \
             mock.patch.object(shared.subprocess, 'run', side_effect=steam_run):
            del os.environ['BRINEWELL_STEAM_RUNNING']
            self.assertEqual(shared.cmd_notify(), 0)
        self.assertEqual(polls, ['stopped', 'running', 'stopped', 'running', 'running'])
        self.assertEqual(calls, [['steam', '-ifrunning', 'steam://addlibraryfolder/' + quote(str(self.mount), safe='')]])
        owned = shared.load_ledger(self.state)['owned']
        self.assertEqual((owned['status'], owned['contentid']), ('confirmed', cid))

    def test_first_boot_notify_and_confirm(self):
        self.prepare()
        master = self.steam / 'config/libraryfolders.vdf'
        self.assertFalse(master.exists())
        self.assertEqual(shared.load_ledger(self.state)['owned']['status'], 'pending')
        cid = shared.load_ledger(self.state)['owned']['contentid']
        dot = self.base / '.steam'
        dot.mkdir()
        os.mkfifo(dot / 'steam.pipe')
        calls = []
        def steam_run(argv, **kwargs):
            calls.append(argv)
            master.write_text(self.registry_text(('1', self.mount, cid)))
            return subprocess.CompletedProcess(argv, 0)
        # Steam creates its initial registry after notify has already started waiting.
        def sleep(_):
            if not master.exists():
                self.write_registry(self.registry_text())
        with mock.patch.object(shared, 'eligible_share', return_value=(self.share, str(self.mount))), \
             mock.patch.dict(os.environ, BRINEWELL_STEAM_RUNNING='1', BRINEWELL_NOTIFY_TIMEOUT='2',
                             BRINEWELL_NOTIFY_INTERVAL='0.01', BRINEWELL_STEAM_BIN='steam', BRINEWELL_STEAM_DOT_DIR=str(dot)), \
             mock.patch.object(shared.time, 'sleep', side_effect=sleep), \
             mock.patch.object(shared.subprocess, 'run', side_effect=steam_run):
            self.assertEqual(shared.cmd_notify(), 0)
        self.assertEqual(calls, [['steam', '-ifrunning', 'steam://addlibraryfolder/' + quote(str(self.mount), safe='')]])
        owned = shared.load_ledger(self.state)['owned']
        self.assertEqual((owned['status'], owned['contentid']), ('confirmed', cid))

    def test_stale_cleanup_absent_ro_and_running(self):
        for transition in ('absent', 'ro', 'running'):
            with self.subTest(transition=transition):
                original = self.registry_text(('1', self.mount, '123'), ('2', '/unrelated', '456'))
                master = self.write_registry(original)
                self.ledger(self.owned())
                sentinel = self.mount / 'user-data'
                sentinel.write_text('untouched')
                with mock.patch.object(shared, 'eligible_share', return_value=(None, None)), \
                     mock.patch.dict(os.environ, BRINEWELL_STEAM_RUNNING='1' if transition == 'running' else '0'):
                    shared.cmd_prepare()
                if transition == 'running':
                    self.assertEqual(master.read_text(), original)
                    self.assertIsNotNone(shared.load_ledger(self.state)['owned'])
                else:
                    self.assertEqual(master.read_text(), shared.vdf.vdf_remove(original, ['libraryfolders', '1']))
                    self.assertIsNone(shared.load_ledger(self.state)['owned'])
                self.assertEqual(sentinel.read_text(), 'untouched')

    def test_user_owned_legacy_conflict_and_suppression(self):
        master = self.write_registry(self.registry_text(('1', self.mount, '123'), ('2', '/run/brinewell/shared', '2')))
        original = master.read_text()
        with mock.patch.object(shared, 'log') as log:
            self.prepare()
            self.assertIn('legacy', str(log.call_args_list))
        self.assertIsNone(shared.load_ledger(self.state)['owned'])
        with mock.patch.object(shared, 'eligible_share', return_value=(None, None)):
            shared.cmd_prepare()
        self.assertEqual(master.read_text(), original)
        self.ledger(self.owned(contentid='different'))
        with mock.patch.object(shared, 'eligible_share', return_value=(None, None)):
            shared.cmd_prepare()
        self.assertEqual(master.read_text(), original)
        self.ledger(self.owned())
        master.write_text(self.registry_text())
        self.prepare()
        self.assertEqual(shared.load_ledger(self.state)['suppressed']['share_id'], self.sid)
        self.prepare()
        self.assertIsNone(shared.load_ledger(self.state)['owned'])
        with mock.patch.object(shared, 'eligible_share', return_value=(None, None)):
            shared.cmd_prepare()
        self.assertIsNone(shared.load_ledger(self.state)['suppressed'])
        self.prepare()
        self.assertEqual(shared.load_ledger(self.state)['owned']['status'], 'pending')

    def test_same_name_swap_removes_old_and_prepares_new(self):
        master = self.write_registry(self.registry_text(('1', self.mount, '123')))
        self.ledger(self.owned(sid='f' * 32))
        self.prepare()
        self.assertNotIn(str(self.mount), master.read_text())
        owned = shared.load_ledger(self.state)['owned']
        self.assertEqual((owned['share_id'], owned['status']), (self.sid, 'pending'))

    def test_registry_mirror_topologies_and_malformed(self):
        for topology in ('symlink', 'hardlink', 'independent', 'malformed'):
            with self.subTest(topology=topology):
                original = self.registry_text(('1', self.mount, '123'), ('2', '/other', '456'))
                master = self.write_registry(original)
                mirror = self.steam / 'steamapps/libraryfolders.vdf'
                mirror.parent.mkdir(parents=True, exist_ok=True)
                if os.path.lexists(mirror):
                    mirror.unlink()
                if topology == 'symlink':
                    mirror.symlink_to('../config/libraryfolders.vdf')
                elif topology == 'hardlink':
                    os.link(master, mirror)
                else:
                    mirror.write_text('broken {' if topology == 'malformed' else original + '// mirror-only\n')
                before = mirror.read_text()
                result = shared.remove_owned_entry_from_registries(self.steam, self.owned())
                if topology == 'malformed':
                    self.assertFalse(result)
                    self.assertEqual(master.read_text(), original)
                    self.assertEqual(mirror.read_text(), before)
                else:
                    self.assertTrue(result)
                    self.assertNotIn(str(self.mount), master.read_text())
                    self.assertNotIn(str(self.mount), mirror.read_text())
                    if topology == 'symlink':
                        self.assertTrue(mirror.is_symlink())
                    elif topology == 'hardlink':
                        self.assertTrue(os.path.samefile(master, mirror))
                    else:
                        self.assertIn('// mirror-only', mirror.read_text())

    def test_invalid_contentids_and_collision(self):
        folder = self.mount / 'libraryfolder.vdf'
        for cid in ('0', 'abc', '9223372036854775808', '123'):
            with self.subTest(cid=cid):
                folder.write_text(f'"libraryfolder" {{ "contentid" "{cid}" }}')
                self.assertIsNone(shared.ensure_library_initialized(str(self.mount), 'x', {'123'}))

    def test_failed_probe_does_not_notify(self):
        with mock.patch.object(shared, 'probe_writable_target', return_value=False):
            self.prepare()
        with mock.patch.object(shared, 'eligible_share', return_value=(self.share, str(self.mount))), \
             mock.patch.object(shared.subprocess, 'run') as run, \
             mock.patch.dict(os.environ, BRINEWELL_NOTIFY_TIMEOUT='0.1'):
            shared.cmd_notify()
            run.assert_not_called()

    def test_same_share_conflict_preserves_state(self):
        original = self.registry_text(('1', self.mount, '123'))
        master = self.write_registry(original)
        suppressed = {'share_id': 'other'}
        self.ledger(self.owned(), suppressed)
        with mock.patch.object(shared, 'eligible_share', return_value=(self.share, None)):
            shared.cmd_prepare()
        self.assertEqual(master.read_text(), original)
        self.assertEqual(shared.load_ledger(self.state)['suppressed'], suppressed)
        self.assertIsNotNone(shared.load_ledger(self.state)['owned'])

    def test_same_share_conflict_preserves_suppression_without_ownership(self):
        suppressed = {'share_id': self.sid, 'library_path': str(self.mount)}
        self.ledger(None, suppressed)
        (self.mount / 'steamapps').mkdir()
        (self.mount / 'SteamLibrary/steamapps').mkdir(parents=True)
        with mock.patch.object(shared, 'load_share_meta', return_value=self.share), \
             mock.patch.object(shared, 'verify_mountinfo', return_value=True):
            self.assertEqual(shared.cmd_prepare(), 0)
        self.assertEqual(shared.load_ledger(self.state)['suppressed'], suppressed)
        self.assertIsNone(shared.load_ledger(self.state)['owned'])

    def test_unreadable_boot_id_fails_closed(self):
        with mock.patch.dict(os.environ, BRINEWELL_BOOT_ID=''):
            self.assertIsNone(shared.get_boot_id())
            self.prepare()
            owned = shared.load_ledger(self.state)['owned']
            self.assertNotIn('prepared_boot_id', owned)
            with mock.patch.object(shared, 'eligible_share', return_value=(self.share, str(self.mount))), \
                 mock.patch.object(shared.subprocess, 'run') as run, \
                 mock.patch.dict(os.environ, BRINEWELL_NOTIFY_TIMEOUT='0.1'):
                self.assertEqual(shared.cmd_notify(), 0)
                run.assert_not_called()
        with mock.patch.dict(os.environ):
            del os.environ['BRINEWELL_BOOT_ID']
            with mock.patch.object(shared.Path, 'read_text', side_effect=OSError('unreadable')):
                self.assertIsNone(shared.get_boot_id())

    def test_notify_alarm_stops_stalled_inspection_and_releases_lock(self):
        def stalled(_ledger):
            shared.signal.raise_signal(shared.signal.SIGALRM)
        with mock.patch.object(shared, 'eligible_share', side_effect=stalled), \
             mock.patch.dict(os.environ, BRINEWELL_NOTIFY_TIMEOUT='0.1'), \
             mock.patch.object(shared, 'log') as log:
            self.assertEqual(shared.cmd_notify(), 0)
            self.assertIn('notify deadline exceeded', str(log.call_args_list))
        with open(self.state / 'steam-shared-library.lock', 'a') as lock:
            shared.fcntl.flock(lock, shared.fcntl.LOCK_EX | shared.fcntl.LOCK_NB)

    def test_notify_rejects_multiple_matching_registry_entries(self):
        self.prepare()
        cid = shared.load_ledger(self.state)['owned']['contentid']
        self.write_registry(self.registry_text(('1', self.mount, cid), ('2', self.mount, cid)))
        with mock.patch.object(shared, 'eligible_share', return_value=(self.share, str(self.mount))), \
             mock.patch.object(shared.subprocess, 'run') as run, \
             mock.patch.object(shared, 'log') as log, \
             mock.patch.dict(os.environ, BRINEWELL_NOTIFY_TIMEOUT='0.1'):
            self.assertEqual(shared.cmd_notify(), 0)
            run.assert_not_called()
            self.assertIn('ambiguous Steam registry library path', str(log.call_args_list))

    def test_reverse_and_unsupported_symlinks(self):
        master = self.write_registry(self.registry_text(('1', self.mount, '123')))
        mirror = self.steam / 'steamapps/libraryfolders.vdf'
        mirror.parent.mkdir(parents=True)
        mirror.write_text(master.read_text())
        master.unlink()
        master.symlink_to('../steamapps/libraryfolders.vdf')
        self.assertTrue(shared.remove_owned_entry_from_registries(self.steam, self.owned()))
        self.assertTrue(master.is_symlink())
        self.assertNotIn(str(self.mount), mirror.read_text())
        master.unlink()
        other = self.base / 'other'
        other.write_text(self.registry_text(('1', self.mount, '123')))
        master.symlink_to(other)
        before = other.read_text()
        self.assertFalse(shared.remove_owned_entry_from_registries(self.steam, self.owned()))
        self.assertEqual(other.read_text(), before)

    def test_oversized_meta_and_lock_timeout(self):
        self.meta.write_text('x' * 65537)
        self.assertIsNone(shared.load_share_meta(self.meta))
        lockpath = self.state / 'steam-shared-library.lock'
        with open(lockpath, 'a') as lock:
            shared.fcntl.flock(lock, shared.fcntl.LOCK_EX)
            with mock.patch.dict(os.environ, BRINEWELL_LOCK_TIMEOUT='0.01'):
                self.assertEqual(shared.cmd_prepare(), 0)

    def test_mismatched_registration_not_claimed(self):
        self.prepare()
        master = self.write_registry(self.registry_text(('1', self.mount, '999')))
        self.prepare()
        self.assertIsNone(shared.load_ledger(self.state)['owned'])
        self.assertIn('999', master.read_text())

    def test_initramfs_validation_shell(self):
        init = ROOTFS.parents[1] / 'initramfs/init'
        subprocess.run(['sh', '-n', str(init)], check=True)
        text = init.read_text()
        validation = text.split('SHARE_ID_VALID=1\n', 1)[1].split('case $SHARE_MODE in', 1)[0]
        for sid, label, expected_id, expected_label in (
                ('a' * 32, 'e4b8ad', '1', 'e4b8ad'), ('../', 'aa', '0', 'aa'),
                ('A' * 32, 'a', '0', ''), ('a' * 31, 'zz', '0', ''),
                ('1' * 32, 'ab' * 97, '1', '')):
            with self.subTest(sid=sid, label=label):
                script = 'SHARE_ID=$1; SHARE_LABEL_HEX=$2; SHARE_ID_VALID=1\n' + validation + '\nprintf "%s|%s" "$SHARE_ID_VALID" "$SHARE_LABEL_HEX"'
                result = subprocess.run(['sh', '-c', script, 'sh', sid, label], check=True, capture_output=True, text=True)
                self.assertEqual(result.stdout, expected_id + '|' + expected_label)


if __name__ == "__main__":
    unittest.main()
