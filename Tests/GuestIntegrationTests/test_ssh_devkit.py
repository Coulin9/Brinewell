import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
INIT = ROOT / "runtime/initramfs/init"
SSHD_UNIT = "/usr/lib/systemd/system/sshd.service"
WANTS_LINK = "systemd/system/multi-user.target.wants/sshd.service"
DEVKIT_HELPER = """#!/bin/bash
SYSTEMD_SERVICES=(
    sshd
    xrdp
    steamos-devkit-service
)
for service in "${SYSTEMD_SERVICES[@]}"; do
    systemctl "$1" --now "$service.service"
done
"""


class SSHDevkitTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "newroot"
        self.run = Path(temporary.name) / "run"
        (self.run / "brinewell").mkdir(parents=True)
        (self.run / "systemd/system").mkdir(parents=True)
        unit = self.root / SSHD_UNIT.lstrip("/")
        unit.parent.mkdir(parents=True)
        unit.touch()
        self.devkit = self.root / "usr/bin/steamos-polkit-helpers/steamos-devkit-mode"
        self.devkit.parent.mkdir(parents=True)
        self.devkit.write_text(DEVKIT_HELPER)

    def boot(self, policy):
        # Run the initramfs SSH policy against an isolated guest tree.
        section = INIT.read_text().split("# ---------------------------------------------------------------- SSH switch\n")[1]
        section = section.split("# ---------------------------------------------------------------- switch_root\n")[0]
        section = section.replace("/newroot", "$TEST_ROOT").replace("/run/", "$TEST_RUN/")
        shell = """
msg() { :; }
warn() { echo "$*" >&2; }
bind_ro() { cp "$1" "$2"; }
"""
        return subprocess.run(
            ["/bin/sh", "-c", shell + section],
            env={**os.environ, "SSH": policy, "TEST_ROOT": str(self.root), "TEST_RUN": str(self.run)},
            check=True, capture_output=True, text=True,
        )

    def test_devkit_toggles_its_services_without_touching_sshd(self):
        persistent = self.root / "etc" / WANTS_LINK
        persistent.parent.mkdir(parents=True)
        persistent.symlink_to(SSHD_UNIT)
        self.boot("1")
        self.assertEqual(os.readlink(persistent), SSHD_UNIT)
        self.assertFalse((self.run / WANTS_LINK).is_symlink())

        systemctl = self.root / "usr/bin/systemctl"
        systemctl.write_text('#!/bin/sh\nprintf "%s\\n" "$*"\n')
        systemctl.chmod(0o755)
        for action in ("enable", "disable"):
            with self.subTest(action=action):
                result = subprocess.check_output(
                    ["/bin/bash", self.devkit, action],
                    env={**os.environ, "PATH": f"{systemctl.parent}:{os.environ['PATH']}"},
                    text=True,
                )
                self.assertEqual(result.splitlines(), [
                    f"{action} --now xrdp.service",
                    f"{action} --now steamos-devkit-service.service",
                ])

    def test_missing_persistent_link_is_enabled_for_this_boot(self):
        self.boot("1")
        self.assertEqual(os.readlink(self.run / WANTS_LINK), SSHD_UNIT)
        self.assertFalse((self.root / "etc" / WANTS_LINK).is_symlink())

    def test_disabled_policy_still_masks_ssh_and_leaves_devkit_alone(self):
        self.boot("0")
        self.assertEqual(os.readlink(self.run / "systemd/system/sshd.service"), "/dev/null")
        self.assertEqual(os.readlink(self.run / "systemd/system/sshd-unix-local.socket"), "/dev/null")
        self.assertEqual(self.devkit.read_text(), DEVKIT_HELPER)

    def test_unset_policy_leaves_guest_enablement_alone(self):
        self.boot("")
        self.assertFalse((self.run / WANTS_LINK).is_symlink())
        self.assertEqual(self.devkit.read_text(), DEVKIT_HELPER)

    def test_unknown_devkit_layout_keeps_stock_helper_and_warns(self):
        unknown_layout = DEVKIT_HELPER.replace("SYSTEMD_SERVICES=(", "OTHER_SERVICES=(")
        self.devkit.write_text(unknown_layout)
        result = self.boot("1")
        self.assertIn("could not separate sshd", result.stderr)
        self.assertEqual(self.devkit.read_text(), unknown_layout)


if __name__ == "__main__":
    unittest.main()
