#!/usr/bin/env python3
"""Exercise UU stop behavior against process snapshots without sending signals."""
import importlib.machinery
import importlib.util
from pathlib import Path
import shutil
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).parents[1] / 'runtime/guest/rootfs/usr/lib/brinewell/uu-stop'
loader = importlib.machinery.SourceFileLoader('uu_stop', str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
stopper = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
loader.exec_module(stopper)

GROUP = '0::/system.slice/uuplugin.service\n'
OTHER_GROUP = '0::/system.slice/other.service\n'


class StopTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.proc = Path(self.temporary.name)
        (self.proc / 'self').mkdir()
        (self.proc / 'self/cgroup').write_text(GROUP)
        self.elapsed = 0

    def process(self, pid, program, group=GROUP):
        directory = self.proc / str(pid)
        directory.mkdir()
        (directory / 'cmdline').write_bytes(b'FEX\0/tmp/uu/' + program.encode() + b'\0')
        (directory / 'cgroup').write_text(group)

    def run_stop(self, arguments, on_signal=None, on_wait=None):
        def proc_path(value):
            return self.proc / str(value).removeprefix('/proc').lstrip('/')

        def sleep(duration):
            self.elapsed += duration
            if on_wait:
                on_wait()

        with patch.object(stopper, 'Path', side_effect=proc_path), \
             patch.object(stopper.sys, 'argv', [str(SOURCE), *arguments]), \
             patch.object(stopper.os, 'getpid', return_value=9000), \
             patch.object(stopper.os, 'kill', side_effect=on_signal) as kill, \
             patch.object(stopper.time, 'monotonic', side_effect=lambda: self.elapsed), \
             patch.object(stopper.time, 'sleep', side_effect=sleep):
            stopper.main()
        return kill

    def test_missing_mainpid_still_waits_for_guardian(self):
        for arguments in ([], [''], ['101']):
            with self.subTest(arguments=arguments):
                self.elapsed = 0
                self.process(102, 'xuplugin-guardian')
                kill = self.run_stop(arguments, on_wait=lambda: shutil.rmtree(self.proc / '102'))
                kill.assert_not_called()
                self.assertGreater(self.elapsed, 0)
                self.assertFalse((self.proc / '102').exists())

    def test_normal_stop_signals_main_and_waits_for_guardian_only(self):
        self.process(101, 'uuplugin')
        self.process(102, 'xuplugin-guardian')
        self.process(103, 'unrelated')
        self.process(104, 'uuplugin', OTHER_GROUP)
        kill = self.run_stop(['101'],
                             on_signal=lambda *_: shutil.rmtree(self.proc / '101'),
                             on_wait=lambda: shutil.rmtree(self.proc / '102'))
        kill.assert_called_once_with(101, signal.SIGTERM)
        self.assertGreater(self.elapsed, 0)
        self.assertTrue((self.proc / '103').exists())
        self.assertTrue((self.proc / '104').exists())

    def test_unrelated_mainpid_is_not_signalled(self):
        self.process(101, 'unrelated', OTHER_GROUP)
        self.process(102, 'xuplugin-guardian')
        kill = self.run_stop(['101'], on_wait=lambda: shutil.rmtree(self.proc / '102'))
        kill.assert_not_called()
        self.assertGreater(self.elapsed, 0)
        self.assertTrue((self.proc / '101').exists())

    def test_main_in_another_cgroup_is_signalled_and_both_processes_are_awaited(self):
        self.process(101, 'uuplugin', OTHER_GROUP)
        self.process(102, 'xuplugin-guardian')
        self.process(104, 'xuplugin-guardian', OTHER_GROUP)

        def finish_one_process():
            # The guardian can finish first, while the explicitly tracked main
            # process is still shutting down outside the service cgroup.
            remaining = self.proc / ('102' if (self.proc / '102').exists() else '101')
            shutil.rmtree(remaining)

        kill = self.run_stop(['101'], on_wait=finish_one_process)
        kill.assert_called_once_with(101, signal.SIGTERM)
        self.assertGreaterEqual(self.elapsed, 0.4)
        self.assertFalse((self.proc / '101').exists())
        self.assertFalse((self.proc / '102').exists())
        self.assertTrue((self.proc / '104').exists())

    def test_exit_race_still_waits_for_guardian(self):
        self.process(101, 'uuplugin')
        self.process(102, 'xuplugin-guardian')

        def exit_before_signal(*_):
            shutil.rmtree(self.proc / '101')
            raise ProcessLookupError

        self.run_stop(['101'], on_signal=exit_before_signal,
                      on_wait=lambda: shutil.rmtree(self.proc / '102'))
        self.assertGreater(self.elapsed, 0)
        self.assertFalse((self.proc / '102').exists())

    def test_stuck_guardian_has_bounded_wait_without_signalling_other_processes(self):
        self.process(102, 'xuplugin-guardian')
        kill = self.run_stop([''])
        kill.assert_not_called()
        self.assertGreaterEqual(self.elapsed, 50)
        self.assertLess(self.elapsed, 50.3)
        self.assertTrue((self.proc / '102').exists())

    def test_invalid_mainpid_never_signals_process_groups_or_init(self):
        for value in ('0', '1', '-1', 'invalid'):
            with self.subTest(value=value):
                self.run_stop([value]).assert_not_called()
                self.assertEqual(self.elapsed, 0)


if __name__ == '__main__':
    unittest.main()
