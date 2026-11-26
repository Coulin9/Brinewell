import importlib.machinery
import importlib.util
import io
import json
import errno
import struct
from pathlib import Path
import tempfile
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[2] / 'runtime/guest/rootfs/usr/lib/brinewell/admin-control'
loader = importlib.machinery.SourceFileLoader('admin_control', str(PATH))
spec = importlib.util.spec_from_loader(loader.name, loader)
admin = importlib.util.module_from_spec(spec)
loader.exec_module(admin)


def result(output=b'', rc=0):
    return mock.Mock(stdout=output, returncode=rc)


class AdminTests(unittest.TestCase):
    def test_unit_parses_output_even_on_failure(self):
        with mock.patch.object(admin, 'run', return_value=result(b'LoadState=not-found\nActiveState=inactive\n', 1)):
            self.assertEqual(admin.unit('sshd.socket'), ['not-found', 'inactive', '', ''])

    def test_password_states(self):
        for text, expected in [(b'steamos P x', 'set'), (b'steamos NP', 'unset'),
                               (b'steamos L', 'locked'), (b'other P', 'unknown'), (b'', 'unknown')]:
            with self.subTest(text=text), mock.patch.object(admin, 'run', return_value=result(text)):
                self.assertEqual(admin.password_state(), expected)

    def test_tcp_listeners(self):
        with tempfile.TemporaryDirectory() as directory:
            tcp, tcp6, bind = (Path(directory) / n for n in ('tcp', 'tcp6', 'bind'))
            tcp.write_text('header\n')
            tcp6.write_text('header\n')
            def check(path, address, expected, state='0A', inode='42'):
                tcp.write_text('header\n')
                tcp6.write_text('header\n')
                path.write_text(f'header\n0: {address}:0016 0:0 {state} 0 0 0 0 0 {inode}\n')
                self.assertEqual(admin.is_listening_on_port_22(str(tcp), str(tcp6), str(bind), lambda i: i == '42'), expected)
            for address, expected in [('00000000', True), ('027FA8C0', True), ('0100007F', False), ('037FA8C0', False)]:
                check(tcp, address, expected)
            check(tcp, '00000000', False, state='01')
            check(tcp, '00000000', False, inode='0')
            check(tcp, '00000000', False, inode='43')
            check(tcp6, '00000000000000000000000000000000', True)
            bind.write_text('1')
            check(tcp6, '00000000000000000000000000000000', False)
            check(tcp6, '0000000000000000FFFF0000027FA8C0', True)
            check(tcp6, '00000000000000000000000001000000', False)

    @mock.patch.object(admin, 'password_state', return_value='set')
    @mock.patch.object(admin, 'shadow_stat', return_value=(1, 2, 3, 4))
    def test_password_validation_and_dedup(self, shadow, state):
        instance = admin.Admin()
        with mock.patch.object(admin.pwd, 'getpwnam'), mock.patch.object(admin, 'run', return_value=result()) as execute, \
             mock.patch.object(admin, 'unit', return_value=['loaded', 'inactive', 'dead', 'disabled']), \
             mock.patch.object(admin, 'listening', return_value=False):
            for n, value in enumerate(['', 'x' * 257, 'a\nb', 'a\rb', 'a\0b', 'a:b']):
                reply = instance.request({'type': 'setPassword', 'id': str(n), 'password': value})
                self.assertEqual(reply['code'], 'invalidPassword')
            req = {'type': 'setPassword', 'id': 'valid', 'password': ' space 密碼 '}
            self.assertTrue(instance.request(req)['ok'])
            self.assertTrue(instance.request({'type': 'setPassword', 'id': 'valid', 'password': 'different'})['ok'])
            execute.assert_called_once_with(['/usr/sbin/chpasswd'], 'steamos: space 密碼 \n'.encode())

    @mock.patch.object(admin, 'password_state', return_value='set')
    def test_shadow_revision(self, state):
        with mock.patch.object(admin, 'shadow_stat', side_effect=[(1,), (2,)]), \
             mock.patch.object(admin, 'unit', return_value=['loaded', 'inactive', 'dead', 'disabled']), \
             mock.patch.object(admin, 'listening', return_value=False):
            instance = admin.Admin()
            self.assertEqual(instance.snapshot()['password'], {'state': 'set', 'rev': 2})

    @mock.patch.object(admin, 'password_state', return_value='set')
    @mock.patch.object(admin, 'shadow_stat', return_value=None)
    def test_ssh_modes_and_masking(self, shadow, state):
        instance = admin.Admin()
        service = ['loaded', 'inactive', 'dead', 'disabled']
        absent = ['not-found', 'inactive', 'dead', '']
        with mock.patch.object(admin, 'listening', return_value=False), mock.patch.object(admin, 'run', return_value=result()) as execute:
            with mock.patch.object(admin, 'unit', side_effect=[service, absent, ['loaded', 'active', 'running', 'enabled'], absent]):
                self.assertTrue(instance.request({'type': 'setSSH', 'id': 'a', 'enabled': True})['ok'])
                execute.assert_called_with(['systemctl', 'enable', '--now', 'sshd.service'])
            execute.reset_mock()
            with mock.patch.object(admin, 'unit', side_effect=[['loaded', 'inactive', 'dead', 'masked-runtime'], absent]):
                self.assertEqual(instance.request({'type': 'setSSH', 'id': 'b', 'enabled': True})['code'], 'unitMasked')
                execute.assert_not_called()
            with mock.patch.object(admin, 'unit', side_effect=[absent, ['loaded', 'active', 'listening', 'enabled'], absent, ['loaded', 'active', 'listening', 'enabled']]):
                self.assertTrue(instance.request({'type': 'setSSH', 'id': 'c', 'enabled': True})['ok'])
                execute.assert_called_with(['systemctl', 'enable', '--now', 'sshd.socket'])
            execute.reset_mock()
            with mock.patch.object(admin, 'unit', side_effect=[['loaded', 'active', 'running', 'enabled'], absent, ['loaded', 'inactive', 'dead', 'enabled'], absent]):
                self.assertEqual(instance.request({'type': 'setSSH', 'id': 'd', 'enabled': False})['code'], 'sshPartiallyManaged')
                self.assertEqual(execute.call_args_list, [mock.call(['systemctl', 'stop', 'sshd.service']), mock.call(['systemctl', 'disable', 'sshd.service'])])

    @mock.patch.object(admin, 'password_state', return_value='locked')
    @mock.patch.object(admin, 'shadow_stat', return_value=None)
    def test_locked_refused(self, shadow, state):
        instance = admin.Admin()
        with mock.patch.object(admin.pwd, 'getpwnam'), mock.patch.object(admin, 'run') as execute:
            self.assertEqual(instance.set_password('secret', False), (False, 'accountLocked'))
            execute.assert_not_called()


class AdditionalAdminTests(unittest.TestCase):
    def setUp(self):
        for name, value in [('password_state', 'set'), ('shadow_stat', (1,)),
                            ('unit', ['loaded', 'inactive', 'dead', 'disabled']),
                            ('listening', False)]:
            patcher = mock.patch.object(admin, name, return_value=value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.instance = admin.Admin()

    def test_password_unlock_and_revision_on_failure(self):
        for rc, final, expected in [(0, 'set', True), (1, 'locked', False),
                                    (0, 'unknown', False)]:
            with self.subTest(rc=rc, final=final):
                self.instance.rev = 1
                with mock.patch.object(admin.pwd, 'getpwnam'), \
                     mock.patch.object(admin, 'password_state', side_effect=['locked', 'locked', final, final]), \
                     mock.patch.object(admin, 'shadow_stat', return_value=(2,)), \
                     mock.patch.object(admin, 'run', side_effect=[result(), result(rc=rc)]) as run:
                    reply = self.instance.request({'type': 'setPassword', 'id': str(rc) + final,
                                                   'password': '秘密 test', 'confirmUnlock': True})
                self.assertEqual(reply['ok'], expected)
                self.assertEqual(reply['snapshot']['password'], {'state': final, 'rev': 2})
                self.assertEqual(self.instance.shadow, (2,))
                self.assertNotIn('秘密 test', repr(reply) + repr(self.instance.completed))
                self.assertEqual(run.call_count, 2)

    def test_utf8_length_validation(self):
        with mock.patch.object(admin.pwd, 'getpwnam'), mock.patch.object(admin, 'run') as run:
            self.assertEqual(self.instance.set_password('密' * 86, False), (False, 'invalidPassword'))
            run.assert_not_called()

    def test_ssh_matrix_and_disable_order(self):
        loaded = lambda active, file: ['loaded', active, 'running', file]
        absent = ['not-found', 'inactive', 'dead', '']
        for active, file in [('active', 'enabled'), ('active', 'disabled'),
                             ('inactive', 'enabled'), ('inactive', 'static')]:
            with self.subTest(active=active, file=file), mock.patch.object(admin, 'unit', side_effect=[loaded(active, file), absent]):
                ssh = self.instance.snapshot()['ssh']
                self.assertEqual((ssh['serviceActive'], ssh['serviceUnitFile']), (active, file))
        for masked in ['masked', 'masked-runtime']:
            with self.subTest(masked=masked), mock.patch.object(admin, 'unit', side_effect=[loaded('inactive', masked), absent]), \
                 mock.patch.object(admin, 'run') as run:
                self.assertEqual(self.instance.request({'type': 'setSSH', 'id': masked, 'enabled': True})['code'], 'unitMasked')
                run.assert_not_called()
        with mock.patch.object(admin, 'unit', side_effect=[absent, absent]), mock.patch.object(admin, 'run') as run:
            self.assertEqual(self.instance.request({'type': 'setSSH', 'id': 'missing', 'enabled': True})['code'], 'unitNotFound')
            run.assert_not_called()
        before = [loaded('active', 'enabled'), loaded('active', 'enabled')]
        after = [loaded('inactive', 'disabled'), loaded('inactive', 'disabled')]
        with mock.patch.object(admin, 'unit', side_effect=before + after), mock.patch.object(admin, 'run') as run:
            self.assertTrue(self.instance.request({'type': 'setSSH', 'id': 'off', 'enabled': False})['ok'])
            self.assertEqual(run.call_args_list, [mock.call(['systemctl', verb, 'sshd.' + target])
                             for target in ('socket', 'service') for verb in ('stop', 'disable')])
        with mock.patch.object(admin, 'unit', side_effect=before + before), mock.patch.object(admin, 'run'):
            self.assertEqual(self.instance.request({'type': 'setSSH', 'id': 'fail', 'enabled': False})['code'], 'sshApplyFailed')
        with mock.patch.object(admin, 'unit', side_effect=[loaded('inactive', 'static'), loaded('inactive', 'masked')] * 2), \
             mock.patch.object(admin, 'run') as run:
            self.assertTrue(self.instance.request({'type': 'setSSH', 'id': 'static', 'enabled': False})['ok'])
            self.assertEqual(run.call_count, 2)
            self.assertTrue(all(call.args[0][1] == 'stop' for call in run.call_args_list))
        with mock.patch.object(admin, 'unit', side_effect=[loaded('inactive', 'disabled'), absent] * 2), mock.patch.object(admin, 'run'):
            self.assertEqual(self.instance.request({'type': 'setSSH', 'id': 'fail-on', 'enabled': True})['code'], 'sshApplyFailed')

    def test_watch_filter_overflow_and_unavailable(self):
        watch = admin.ShadowWatch.__new__(admin.ShadowWatch)
        watch.fd = 42
        def event(mask, name=b''):
            name = name + b'\0' if name else name
            return struct.pack('iIII', 1, mask, 0, len(name)) + name
        with mock.patch.object(admin.os, 'read', return_value=event(admin.WATCH_MASK, b'other')):
            self.assertFalse(watch.events())
        with mock.patch.object(admin.os, 'read', return_value=event(0x100, b'shadow')):
            self.assertTrue(watch.events())
        with mock.patch.object(admin.os, 'read', return_value=event(0x4000)), mock.patch.object(watch, 'open') as reopen:
            self.assertTrue(watch.events())
            reopen.assert_called_once()
        with mock.patch.object(admin.os, 'read', side_effect=OSError):
            self.assertFalse(watch.events())
        with mock.patch.object(admin.ctypes, 'CDLL', side_effect=AttributeError):
            watch.fd = -1
            watch.open()
            self.assertEqual(watch.fd, -1)

    def test_password_unknown_and_request_validation(self):
        with mock.patch.object(admin.pwd, 'getpwnam'), mock.patch.object(admin, 'run') as run:
            with mock.patch.object(admin, 'password_state', return_value='unknown'):
                reply = self.instance.request({'type': 'setPassword', 'id': 'unknown', 'password': 'x'})
                self.assertEqual(reply['code'], 'passwordApplyFailed')
                self.assertEqual(reply['snapshot']['password']['state'], 'unknown')
            for value in (None, 0, 1, 'true'):
                reply = self.instance.request({'type': 'setPassword', 'id': str(value), 'password': 'x', 'confirmUnlock': value})
                self.assertEqual(reply['code'], 'invalidRequest')
            for command in ('setSSH', 'setPassword'):
                for value in (None, 1, 'wrong'):
                    reply = self.instance.request({'type': command, 'id': 'target', 'targetInstance': value, 'enabled': True, 'password': 'x'})
                    self.assertEqual(reply['code'], 'invalidRequest')
            run.assert_not_called()

    def test_ssh_result_validation_and_masked_stop(self):
        loaded = ['loaded', 'inactive', 'dead', 'disabled']
        absent = ['not-found', 'inactive', 'dead', '']
        for after, rc, expected in [(['loaded', 'active', 'running', 'static'], 0, 'sshPartiallyManaged'),
                                     (['loaded', 'active', 'running', 'enabled'], 1, 'sshPartiallyManaged'),
                                     (['', '', '', ''], 0, 'sshApplyFailed'),
                                     (['loaded', 'inactive', 'dead', 'enabled'], 0, 'sshApplyFailed')]:
            other = ['', '', '', ''] if not after[0] else absent
            with self.subTest(after=after, rc=rc), mock.patch.object(admin, 'unit', side_effect=[loaded, absent, after, other]), mock.patch.object(admin, 'run', return_value=result(rc=rc)):
                reply = self.instance.request({'type': 'setSSH', 'id': 'on', 'enabled': True, 'targetInstance': self.instance.instance})
                self.assertEqual(reply['code'], expected)
        masked = ['masked', 'active', 'running', 'masked']
        with mock.patch.object(admin, 'unit', side_effect=[masked, absent, ['masked', 'inactive', 'dead', 'masked'], absent]), mock.patch.object(admin, 'run') as run:
            self.assertTrue(self.instance.request({'type': 'setSSH', 'id': 'off', 'enabled': False})['ok'])
            run.assert_called_once_with(['systemctl', 'stop', 'sshd.service'])
        with mock.patch.object(admin, 'unit', side_effect=[loaded, absent, ['', '', '', ''], ['', '', '', '']]), mock.patch.object(admin, 'run'):
            self.assertEqual(self.instance.request({'type': 'setSSH', 'id': 'off2', 'enabled': False})['code'], 'sshApplyFailed')

    def test_socket_owner_scans_descriptors(self):
        def listdir(path):
            return ['123', '456'] if path == '/proc' else ['3']
        def opened(path, **kwargs):
            return io.StringIO('other\n' if '/123/' in path else 'sshd\n')
        with mock.patch.object(admin.os, 'listdir', side_effect=listdir), mock.patch('builtins.open', side_effect=opened), mock.patch.object(admin.os, 'readlink', return_value='socket:[42]'):
            self.assertTrue(admin.socket_owner('42'))
            self.assertFalse(admin.socket_owner('43'))
        with mock.patch.object(admin.os, 'listdir', side_effect=PermissionError):
            self.assertFalse(admin.socket_owner('42'))

    def test_timeout(self):
        with mock.patch.object(admin.subprocess, 'run', return_value=result()) as run:
            admin.run(['systemctl'])
            self.assertEqual(run.call_args.kwargs['timeout'], 3.0)

    def test_nonblocking_queue_and_recursive_json(self):
        clock = [0.0]
        writes = []
        calls = [0]
        payload = b'[' * 2000 + b'0' + b']' * 2000 + b'\n' + b'{"type":"getSnapshot","id":"ok"}\n'
        reads = iter([payload, b''])
        def write(fd, data):
            calls[0] += 1
            if calls[0] <= 2:
                raise BlockingIOError(errno.EAGAIN, 'blocked')
            n = min(20, len(data))
            writes.append(data[:n])
            return n
        def select(readers, writers, errors, timeout):
            clock[0] += 0.1
            if clock[0] < 0.2:
                return [10], [], []
            if clock[0] > 20:
                return [10], [], []
            return [], writers, []
        with mock.patch.object(admin, 'ShadowWatch', return_value=mock.Mock(fd=-1)), \
             mock.patch('builtins.open', return_value=io.StringIO('boot\n')), \
             mock.patch.object(admin.select, 'select', side_effect=select), \
             mock.patch.object(admin.os, 'read', side_effect=lambda fd, n: next(reads)), \
             mock.patch.object(admin.os, 'write', side_effect=write), \
             mock.patch.object(admin.time, 'monotonic', side_effect=lambda: clock[0]):
            admin.serve_loop(10, self.instance, 5.0)
        messages = [json.loads(line) for line in b''.join(writes).splitlines() if line.endswith(b'}')]
        self.assertEqual(messages[0]['type'], 'hello')
        self.assertTrue(any(m.get('id') == 'ok' for m in messages))


if __name__ == '__main__':
    unittest.main()
