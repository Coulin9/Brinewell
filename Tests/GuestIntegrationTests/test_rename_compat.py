import importlib.machinery
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
INIT = ROOT / "runtime/initramfs/init"
SPLCTL = ROOT / "runtime/guest/rootfs/usr/bin/splctl"
SCRIPTS = ROOT / "runtime/guest/rootfs/usr/lib/brinewell"
USER_UNITS = ROOT / "runtime/guest/rootfs/usr/lib/systemd/user"
GAMESCOPE_SESSION = ROOT / "runtime/guest/rootfs/usr/lib/steamos/gamescope-session"
MIGRATE_UNIT = "brinewell-migrate-user-state.service"

loader = importlib.machinery.SourceFileLoader("migrate_user_state", str(SCRIPTS / "migrate-user-state"))
spec = importlib.util.spec_from_loader(loader.name, loader)
migrate = importlib.util.module_from_spec(spec)
loader.exec_module(migrate)


def function(source, name):
    """The text of one shell function (name() { ... }) from a script."""
    match = re.search(r"(?ms)^" + re.escape(name) + r"\(\) \{.*?^\}", source)
    assert match, name
    return match.group(0)


def functions(source, *names):
    return "\n".join(function(source, name) for name in names)


def run_shell(script, shell="/bin/sh", check=True, **env):
    return subprocess.run([shell, "-c", script], env={**os.environ, **env},
                          check=check, capture_output=True, text=True)


class EspStateDirTests(unittest.TestCase):
    """A/B state follows the bootenv FILE, not the state directory."""
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.esp = Path(temporary.name)
        self.init_fn = function(INIT.read_text(), "esp_state_dir")
        self.splctl_fn = function(SPLCTL.read_text(), "esp_state_dir")

    def bootenv(self, directory, content="BOOT_ORDER=A B\nBOOT_A_LEFT=3\nBOOT_B_LEFT=3\n"):
        path = self.esp / directory
        path.mkdir(exist_ok=True)
        (path / "bootenv").write_text(content)

    def resolved(self, fn, shell):
        return run_shell(fn + f'\nesp_state_dir "{self.esp}"', shell=shell).stdout.strip()

    def test_new_file_wins_over_legacy_file(self):
        self.bootenv("brinewell")
        self.bootenv("appledeck", "BOOT_ORDER=B A\nBOOT_A_LEFT=1\nBOOT_B_LEFT=2\n")
        for fn, shell in ((self.init_fn, "/bin/sh"), (self.splctl_fn, "/bin/bash")):
            self.assertEqual(self.resolved(fn, shell), "brinewell")

    def test_legacy_file_is_authoritative(self):
        self.bootenv("appledeck")
        for fn, shell in ((self.init_fn, "/bin/sh"), (self.splctl_fn, "/bin/bash")):
            self.assertEqual(self.resolved(fn, shell), "appledeck")

    def test_empty_new_directory_does_not_win_over_legacy_file(self):
        (self.esp / "brinewell").mkdir()  # directory alone is not state
        self.bootenv("appledeck")
        for fn, shell in ((self.init_fn, "/bin/sh"), (self.splctl_fn, "/bin/bash")):
            self.assertEqual(self.resolved(fn, shell), "appledeck")

    def test_no_bootenv_defaults_to_new(self):
        (self.esp / "appledeck").mkdir()
        for fn, shell in ((self.init_fn, "/bin/sh"), (self.splctl_fn, "/bin/bash")):
            self.assertEqual(self.resolved(fn, shell), "brinewell")


class BootenvReadSaveTests(unittest.TestCase):
    """esp_read/save_state run with a stubbed mount against a fixture esp."""
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)
        self.fixture = self.tmp / "fixture"      # the esp content
        self.fixture.mkdir()
        self.mnt = self.tmp / "esp"              # the mountpoint init uses
        text = functions(INIT.read_text(), "esp_rw_mount", "esp_rw_umount",
                         "esp_state_dir", "esp_read", "save_state")
        # Redirect the fixture mountpoint AND init's fixed /tmp/bootenv scratch
        # file into the per-test directory so runs cannot touch the host.
        text = text.replace("/esp", str(self.mnt))
        text = text.replace("/tmp/bootenv", str(self.tmp / "bootenv-scratch"))
        self.script = (text + """
mount() {
    [ "$MOUNT_OK" = 0 ] && return 1
    eval "_t=\\${$#}"
    mkdir -p "$_t"
    for _e in "$ESPFIX"/*; do [ -e "$_e" ] || continue; cp -Rp "$_e" "$_t/"; done
    return 0
}
umount() { :; }
sync() { :; }
warn() { echo "WARN $*"; }
msg() { :; }
ESP_DEV=/dev/stub MAX_TRIES=3 STATE_READ=1
""")

    def bootenv(self, directory, order="A B", left_a=3, left_b=3):
        path = self.fixture / directory
        path.mkdir(exist_ok=True)
        (path / "bootenv").write_text(
            f"BOOT_ORDER={order}\nBOOT_A_LEFT={left_a}\nBOOT_B_LEFT={left_b}\n")

    def cycle(self, mount_ok=1):
        env = dict(ESPFIX=str(self.fixture), MOUNT_OK=str(mount_ok))
        script = self.script + """
esp_read
echo "ORDER=$BOOT_ORDER A=$BOOT_A_LEFT B=$BOOT_B_LEFT DIR=$STATE_DIR SR=$STATE_READ"
BOOT_A_LEFT=0; SLOT=A
save_state
"""
        return run_shell(script, **env).stdout

    def test_legacy_file_is_used_and_written_back(self):
        self.bootenv("appledeck", order="B A", left_a=1, left_b=2)
        (self.fixture / "brinewell").mkdir()  # empty dir must not win
        out = self.cycle()
        self.assertIn("ORDER=B A A=1 B=2 DIR=appledeck SR=1", out)
        saved = (self.mnt / "appledeck/bootenv").read_text()
        self.assertIn("BOOT_ORDER=B A", saved)
        self.assertIn("BOOT_A_LEFT=0", saved)
        self.assertIn("BOOT_B_LEFT=2", saved)
        self.assertFalse((self.mnt / "brinewell/bootenv").exists())

    def test_new_file_wins_and_is_written_back(self):
        self.bootenv("brinewell", order="A B", left_a=3, left_b=3)
        self.bootenv("appledeck", order="B A", left_a=1, left_b=2)
        out = self.cycle()
        self.assertIn("ORDER=A B A=3 B=3 DIR=brinewell SR=1", out)
        self.assertTrue((self.mnt / "brinewell/bootenv").is_file())

    def test_unreadable_state_is_not_persisted(self):
        self.bootenv("appledeck", order="B A", left_a=1, left_b=2)
        out = self.cycle(mount_ok=0)
        self.assertIn("SR=0", out)
        self.assertIn("NOT saved", out)
        self.assertFalse((self.mnt / "appledeck/bootenv").exists())
        self.assertFalse((self.mnt / "brinewell/bootenv").exists())

    def test_fresh_esp_writes_new_path(self):
        out = self.cycle()
        self.assertIn("DIR=brinewell SR=1", out)
        self.assertTrue((self.mnt / "brinewell/bootenv").is_file())


class SplctlReadFailureTests(unittest.TestCase):
    """load_state must not treat a mid-read I/O error as EOF: a failed read
    dies before save_state can write truncated state over the real counters."""
    CONTENT = "BOOT_ORDER=B A\nBOOT_A_LEFT=1\nBOOT_B_LEFT=2\n"

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)
        self.state = self.tmp / "bootenv"
        self.state.write_text(self.CONTENT)
        text = functions(SPLCTL.read_text(), "die", "load_state", "left",
                         "set_left", "other", "run_state_cmd", "save_state")
        self.script = text + f"\nMAX_TRIES=3 STATE_REL=stub/bootenv STATE={self.state}\n"

    def run_cmd(self, stub=""):
        return run_shell(self.script + stub + "\nrun_state_cmd set-primary B\n",
                         shell="/bin/bash", check=False)

    def test_failed_read_dies_without_writing(self):
        result = self.run_cmd("cat() { return 1; }\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot read", result.stderr)
        self.assertEqual(self.state.read_text(), self.CONTENT)
        self.assertFalse(self.state.with_name("bootenv.new").exists())

    def test_unreadable_file_dies_without_writing(self):
        self.state.chmod(0o000)
        self.addCleanup(lambda: self.state.exists() and self.state.chmod(0o644))
        result = self.run_cmd()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot read", result.stderr)
        self.state.chmod(0o644)
        self.assertEqual(self.state.read_text(), self.CONTENT)
        self.assertFalse(self.state.with_name("bootenv.new").exists())

    def test_successful_read_still_writes(self):
        result = self.run_cmd()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("BOOT_ORDER=B A", self.state.read_text())
        self.assertIn("BOOT_B_LEFT=3", self.state.read_text())


class ProvisionGateTests(unittest.TestCase):
    """The real provision() runs against stubbed devices; destructive commands
    are counted and must be unreachable for every non-fresh marker state."""
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)
        self.fixture = self.tmp / "esp"
        self.fixture.mkdir()
        self.count = self.tmp / "calls"
        self.root_fixture = self.tmp / "root"
        for path in (self.root_fixture / "usr/lib/steamos-efi",
                     self.root_fixture / "etc"):
            path.mkdir(parents=True)
        (self.root_fixture / "usr/lib/os-release").write_text('BUILD_ID=b1\n')
        (self.root_fixture / "etc/shadow").write_text('steamos:*:1::::::\n')
        bootconf = self.root_fixture / "usr/lib/steamos-efi/steamos-bootconf"
        bootconf.write_text('#!/bin/sh\n')
        bootconf.chmod(0o755)
        self.mnt = self.tmp / "check-esp"
        text = functions(INIT.read_text(), "prov_marker",
                         "prov_marker_state", "prov_partsets", "provision")
        # The initramfs has a real /esp; the test redirects the marker check's
        # default mountpoint into the fixture tree.
        text = text.replace("_mnt=${2:-/esp}", f"_mnt=${{2:-{self.mnt}}}")
        self.script = text + """
prov_fail() { echo "PROV_FAIL: $*"; exit 42; }
prov_say() { :; }
msg() { :; }
warn() { echo "WARN: $*"; }
getarg() { :; }
fx_find_port() { :; }
prov_check_disk() { :; }
prov_key() { echo "$1" | tr -d '-'; }
part_dev() { echo "/dev/stub-$1"; }
prov_uuid() { echo "uuid-$1"; }
cpio() { cat > /dev/null; echo x > provision.env; echo x > rootfs.caibx; }
prov_parse() {
    P_MACHINE_ID=mid P_DISK_GUID=guid P_STEAMOS_BUILDID=b1 P_STEAMOS_VERSION=v
    P_STEAMOS_BRANCH=x P_PASSWORD_HASH= P_HOSTNAME=h
}
recovery_copy() { echo "CALL recovery_copy" >> "$COUNT_FILE"; }
prov_cmd() { echo "CALL $*" >> "$COUNT_FILE"; return 0; }
mount() {
    eval "_t=\\${$#}"
    eval "_d=\\${$(( $# - 1 ))}"
    # Only the marker check's esp mount is failable; provisioning's later
    # partition mounts (part_dev devices) still succeed.
    [ "$_d" = "$ESP_DEV" ] && [ "$MOUNT_OK" = 0 ] && return 1
    mkdir -p "$_t"
    if [ "$_d" = "$ESP_DEV" ]; then
        for _e in "$ESP_FIXTURE"/*; do
            [ -e "$_e" ] || continue
            ln -sfn "$_e" "$_t/${_e##*/}"
        done
    elif [ "$2" = btrfs ]; then
        for _e in "$ROOT_FIXTURE"/*; do
            [ -e "$_e" ] || continue
            ln -sfn "$_e" "$_t/${_e##*/}"
        done
    fi
    return 0
}
umount() { :; }
sync() { :; }
ESP_DEV=/dev/esp-stub
PROV_NAMES="esp efi-A efi-B rootfs-A rootfs-B var-A var-B home"
PAYLOAD_DEV=/dev/null
"""

    def marker(self, directory, content="mid guid", mode=None):
        path = self.fixture / directory
        path.mkdir(exist_ok=True)
        marker = path / "provisioned"
        marker.write_text(content)
        if mode is not None:
            marker.chmod(mode)

    def provision(self, mount_ok=1, payload=True):
        find = "find_payload() { PAYLOAD_DEV=/dev/null; return %d; }" % (0 if payload else 1)
        env = dict(ESP_FIXTURE=str(self.fixture), ROOT_FIXTURE=str(self.root_fixture),
                   PROV_TMP=str(self.tmp / "prov"), COUNT_FILE=str(self.count),
                   MOUNT_OK=str(mount_ok))
        result = run_shell(find + "\n" + self.script + "\nprovision\n",
                           check=False, **env)
        calls = self.count.read_text() if self.count.exists() else ""
        return result, calls

    def assert_blocked(self, result, calls):
        self.assertIn("PROV_FAIL", result.stdout + result.stderr)
        self.assertNotIn("CALL", calls)

    def test_blank_disk_proceeds_to_format(self):
        result, calls = self.provision()
        self.assertNotIn("PROV_FAIL: esp provisioned", result.stdout)
        self.assertEqual(calls.count("CALL mkfs.fat"), 3)
        self.assertEqual(calls.count("CALL /bin/mke2fs"), 3)
        self.assertIn("CALL btrfstune", calls)

    def test_legacy_matching_marker_is_done(self):
        self.marker("appledeck")
        result, calls = self.provision()
        self.assertNotIn("PROV_FAIL", result.stdout)
        self.assertNotIn("CALL", calls)

    def test_new_matching_marker_is_done(self):
        self.marker("brinewell")
        result, calls = self.provision()
        self.assertNotIn("PROV_FAIL", result.stdout)
        self.assertNotIn("CALL", calls)

    def test_legacy_mismatching_marker_blocks(self):
        self.marker("appledeck", "other guid")
        self.assert_blocked(*self.provision())

    def test_empty_marker_blocks(self):
        self.marker("brinewell", "")
        self.assert_blocked(*self.provision())

    def test_literal_absent_marker_text_blocks(self):
        # A file containing the word "absent" is a present marker with a
        # wrong value, not a missing marker.
        self.marker("brinewell", "absent")
        self.assert_blocked(*self.provision())

    def test_directory_at_marker_path_blocks(self):
        # Anything at the marker path that is not a readable regular file
        # reads as "+?", which never equals the expected value.
        path = self.fixture / "brinewell"
        path.mkdir()
        (path / "provisioned").mkdir()
        self.assert_blocked(*self.provision())

    def test_unreadable_marker_blocks(self):
        self.marker("appledeck", "mid guid", mode=0o000)
        self.assert_blocked(*self.provision())

    def test_disagreeing_markers_block(self):
        self.marker("brinewell", "mid guid")
        self.marker("appledeck", "other guid")
        self.assert_blocked(*self.provision())

    def assert_proceeds_to_format(self, result, calls):
        self.assertNotIn("refusing to reformat", result.stdout + result.stderr)
        self.assertEqual(calls.count("CALL mkfs.fat"), 3)
        self.assertEqual(calls.count("CALL /bin/mke2fs"), 3)
        self.assertIn("CALL btrfstune", calls)

    def test_esp_mount_failure_proceeds_like_a_fresh_disk(self):
        # Fresh installer disks have an unformatted esp: an esp that cannot
        # be mounted reads as "no marker" (none), so provisioning continues
        # exactly like the blank-disk path even though a marker is actually
        # present underneath.
        self.marker("appledeck")
        self.assert_proceeds_to_format(*self.provision(mount_ok=0))

    def test_corrupt_fat_proceeds_like_a_fresh_disk(self):
        self.marker("brinewell")
        self.assert_proceeds_to_format(*self.provision(mount_ok=0))

    def test_no_payload_with_marker_reports_done(self):
        self.marker("appledeck", "other guid")
        result, calls = self.provision(payload=False)
        self.assertNotIn("PROV_FAIL", result.stdout)
        self.assertNotIn("CALL", calls)

    def test_no_payload_blank_disk_fails(self):
        result, calls = self.provision(payload=False)
        self.assertIn("PROV_FAIL", result.stdout)
        self.assertNotIn("CALL", calls)


class EtcRenameTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.newroot = Path(temporary.name) / "newroot"
        self.upper = self.newroot / "var/lib/overlays/etc/upper"
        self.upper.mkdir(parents=True)
        text = INIT.read_text()
        block = re.search(r"(?ms)^_etc_up=.*?^fi\n", text).group(0)
        self.script = ("msg() { :; }\nwarn() { echo \"$*\" >&2; }\n"
                       + block.replace("/newroot", str(self.newroot)))

    def rename(self):
        run_shell(self.script)

    def test_legacy_etc_dir_is_renamed(self):
        (self.upper / "appledeck").mkdir()
        (self.upper / "appledeck/mac-timezone").write_text("UTC")
        self.rename()
        self.assertFalse((self.upper / "appledeck").exists())
        self.assertEqual((self.upper / "brinewell/mac-timezone").read_text(), "UTC")
        self.rename()  # idempotent
        self.assertEqual((self.upper / "brinewell/mac-timezone").read_text(), "UTC")

    def test_existing_new_dir_keeps_both(self):
        (self.upper / "appledeck").mkdir()
        (self.upper / "brinewell").mkdir()
        self.rename()
        self.assertTrue((self.upper / "appledeck").is_dir())
        self.assertTrue((self.upper / "brinewell").is_dir())


class MigrateUserStateTests(unittest.TestCase):
    MARK = migrate.LEGACY_PAYLOAD_MARK

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name) / "home"
        self.config = self.home / ".config"
        self.data = self.home / ".local/share"
        self.state = self.home / ".local/state"
        for directory in (self.config, self.data, self.state):
            directory.mkdir(parents=True)

    def run_migrate(self):
        migrate.migrate(self.home, self.config, self.data, self.state)

    def dev_payload(self, base, name="neptune-dev", marked=False):
        payload = base / name
        payload.mkdir(parents=True)
        (payload / "payload.json").write_text("{}")
        if marked:
            (payload / self.MARK).touch()
        return payload

    def test_renames_xdg_dirs_and_quarantines_dev_payloads(self):
        (self.config / "appledeck/title-defaults.json").parent.mkdir()
        (self.config / "appledeck/title-defaults.json").write_text("{}")
        self.dev_payload(self.data / "appledeck")
        self.dev_payload(self.data / "appledeck", "neptune-dev.new")
        (self.data / "appledeck/gfx-path").mkdir(parents=True)
        (self.state / "appledeck/fex-eac").mkdir(parents=True)
        self.run_migrate()
        self.assertTrue((self.config / "brinewell/title-defaults.json").is_file())
        self.assertTrue((self.data / "brinewell/gfx-path").is_dir())
        self.assertTrue((self.state / "brinewell/fex-eac").is_dir())
        # Quarantined INSIDE the legacy directory before the parent rename:
        # no live payload may exist under the new name afterwards.
        for name in ("neptune-dev", "neptune-dev.new"):
            self.assertFalse((self.data / "brinewell" / name).exists())
            self.assertTrue((self.data / "brinewell" / (name + ".legacy-disabled")).is_dir())
            self.assertTrue((self.data / "brinewell" / (name + ".legacy-disabled") / self.MARK).exists())
        self.run_migrate()  # idempotent
        self.assertFalse((self.data / "brinewell/neptune-dev").exists())

    def test_conflicting_dirs_keep_both(self):
        (self.config / "appledeck").mkdir()
        (self.config / "brinewell").mkdir()
        self.dev_payload(self.data / "appledeck")
        (self.data / "brinewell").mkdir()
        self.run_migrate()
        self.assertTrue((self.config / "appledeck").is_dir())
        self.assertTrue((self.config / "brinewell").is_dir())
        # The data dir was not renamed; the payload was still quarantined
        # inside the legacy directory (it can never become active anyway).
        self.assertFalse((self.data / "appledeck/neptune-dev").exists())
        self.assertTrue((self.data / "appledeck/neptune-dev.legacy-disabled").is_dir())
        self.assertFalse((self.data / "brinewell/neptune-dev").exists())

    def test_failed_quarantine_blocks_the_parent_rename(self):
        # The payload can be marked but not moved (the legacy dir is not
        # writable): the parent must NOT be renamed, or the still-live legacy
        # payload would become the active payload under brinewell/.
        payload = self.dev_payload(self.data / "appledeck")
        legacy = self.data / "appledeck"
        legacy.chmod(0o555)
        self.addCleanup(lambda: legacy.exists() and legacy.chmod(0o755))
        self.run_migrate()
        self.assertTrue((payload / "payload.json").is_file())
        self.assertFalse((self.data / "brinewell").exists())
        self.assertFalse((self.data / "brinewell/neptune-dev").exists())
        # Once the quarantine can finish, the marked payload is disabled and
        # the parent rename completes.
        legacy.chmod(0o755)
        self.run_migrate()
        self.assertTrue((self.data / "brinewell/neptune-dev.legacy-disabled").is_dir())
        self.assertFalse((self.data / "brinewell/neptune-dev").exists())

    def test_interrupted_migration_quarantines_marked_payload(self):
        # Parent already renamed, payload still live but carrying the legacy
        # mark (quarantine was interrupted): the next run must finish it.
        self.dev_payload(self.data / "brinewell", marked=True)
        (self.data / "brinewell/neptune-dev.legacy-disabled").mkdir()
        self.run_migrate()
        self.assertFalse((self.data / "brinewell/neptune-dev").exists())
        disabled = sorted(p.name for p in self.data.glob("brinewell/neptune-dev.legacy-disabled*"))
        self.assertEqual(disabled, ["neptune-dev.legacy-disabled",
                                   "neptune-dev.legacy-disabled.1"])

    def test_fresh_dev_push_without_mark_stays_active(self):
        # A payload pushed by this release must not be disabled, even next to
        # .legacy-disabled remnants of an earlier migration.
        self.dev_payload(self.data / "brinewell")
        (self.data / "brinewell/neptune-dev.legacy-disabled").mkdir()
        self.run_migrate()
        self.assertTrue((self.data / "brinewell/neptune-dev/payload.json").is_file())

    def test_fixed_home_paths_migrate_when_xdg_differs(self):
        # The AppleDeck-era writers used fixed $HOME paths; they must be
        # migrated even when XDG_CONFIG_HOME points elsewhere.
        xdg_config = self.home / "xdg-config"
        xdg_state = self.home / "xdg-state"
        xdg_data = self.home / "xdg-data"
        for directory in (xdg_config, xdg_state, xdg_data):
            directory.mkdir(parents=True)
        (self.config / "appledeck/mac-clock24.json").parent.mkdir()
        (self.config / "appledeck/mac-clock24.json").write_text("{}")
        (self.data / "appledeck/neptune-dev").mkdir(parents=True)
        (self.state / "appledeck/fex-eac").mkdir(parents=True)
        (xdg_state / "appledeck/fex-eac").mkdir(parents=True)
        audio = xdg_config / "wireplumber/wireplumber.conf.d"
        audio.mkdir(parents=True)
        (audio / "99-appledeck-audio.conf").write_text(
            "# Generated by appledeck audio-layout from the VirtIO ALSA channel map.\n")
        migrate.migrate(self.home, xdg_config, xdg_data, xdg_state)
        self.assertTrue((self.config / "brinewell/mac-clock24.json").is_file())
        self.assertTrue((self.data / "brinewell/neptune-dev.legacy-disabled").is_dir())
        self.assertTrue((self.state / "brinewell/fex-eac").is_dir())
        self.assertTrue((xdg_state / "brinewell/fex-eac").is_dir())
        self.assertTrue((audio / "99-appledeck-audio.conf.legacy-disabled").is_file())

    def test_steam_markers_renamed(self):
        config = self.home / ".local/share/Steam/config"
        config.mkdir(parents=True)
        (config / "appledeck-shader-defaults").write_text("2\n")
        (config / "appledeck-idle-screensaver").write_text("1\n")
        self.run_migrate()
        self.assertEqual((config / "brinewell-shader-defaults").read_text(), "2\n")
        self.assertEqual((config / "brinewell-idle-screensaver").read_text(), "1\n")

    def test_generated_wireplumber_file_is_disabled(self):
        directory = self.config / "wireplumber/wireplumber.conf.d"
        directory.mkdir(parents=True)
        legacy = directory / "99-appledeck-audio.conf"
        legacy.write_text("# Generated by appledeck audio-layout from the VirtIO ALSA channel map.\n")
        user = directory / "99-appledeck-user.conf"
        user.write_text("# my own override\n")
        self.run_migrate()
        self.assertFalse(legacy.exists())
        self.assertTrue((directory / "99-appledeck-audio.conf.legacy-disabled").is_file())
        self.assertTrue(user.is_file())  # not a generated file: untouched

    def test_user_wireplumber_file_stays(self):
        directory = self.config / "wireplumber/wireplumber.conf.d"
        directory.mkdir(parents=True)
        legacy = directory / "99-appledeck-audio.conf"
        legacy.write_text("# hand-edited\n")
        self.run_migrate()
        self.assertTrue(legacy.is_file())


class MigrationOrderingTests(unittest.TestCase):
    """Every consumer of the migrated state orders after the oneshot unit."""
    CONSUMERS = [
        USER_UNITS / "brinewell-progress.service",
        USER_UNITS / "brinewell-nested-desktop.service",
        USER_UNITS / "steam.service.d/50-brinewell.conf",
        USER_UNITS / "wireplumber.service.d/50-brinewell.conf",
    ]

    def test_migration_unit_is_a_oneshot(self):
        text = (USER_UNITS / MIGRATE_UNIT).read_text()
        self.assertIn("Type=oneshot", text)
        self.assertIn("RemainAfterExit=yes", text)
        self.assertIn("ExecStart=/usr/lib/brinewell/migrate-user-state", text)

    def test_consumers_order_after_migration(self):
        for unit in self.CONSUMERS:
            text = unit.read_text()
            with self.subTest(unit=unit.name):
                self.assertIn(f"Wants={MIGRATE_UNIT}", text)
                self.assertIn(f"After={MIGRATE_UNIT}", text)
                # No leftover per-unit ExecStartPre wiring.
                self.assertNotIn("ExecStartPre=-/usr/lib/brinewell/migrate-user-state", text)
                self.assertNotIn("Requires=", text)

    def test_no_other_user_unit_runs_migration_inline(self):
        for unit in USER_UNITS.rglob("*"):
            if unit.is_file() and unit.name != MIGRATE_UNIT:
                self.assertNotIn("ExecStartPre=-/usr/lib/brinewell/migrate-user-state",
                                 unit.read_text(), unit.name)

    def test_pulled_in_for_both_session_paths(self):
        # gamescope session: progress is in gamescope-session.target.wants.
        # desktop/plasma path: progress is in plasma-session.target.wants and
        # nested-desktop orders after the migration itself.
        self.assertTrue((USER_UNITS / "gamescope-session.target.wants/brinewell-progress.service").exists())
        self.assertTrue((USER_UNITS / "plasma-session.target.wants/brinewell-progress.service").exists())


class GamescopeKeyFallbackTests(unittest.TestCase):
    def setUp(self):
        lines = [l for l in GAMESCOPE_SESSION.read_text().splitlines()
                 if l.startswith(":") and "APPLEDECK_GAMESCOPE_" in l]
        self.assertEqual(len(lines), 3)
        self.script = "\n".join(lines) + '\necho "$BRINEWELL_GAMESCOPE_BACKEND|$BRINEWELL_GAMESCOPE_MANGOAPP|$BRINEWELL_GAMESCOPE_EXTRA_ARGS"'

    def resolve(self, **env):
        return run_shell(self.script, shell="/bin/bash", **env).stdout.strip()

    def test_legacy_keys_apply(self):
        self.assertEqual(self.resolve(APPLEDECK_GAMESCOPE_BACKEND="sdl",
                                      APPLEDECK_GAMESCOPE_MANGOAPP="0",
                                      APPLEDECK_GAMESCOPE_EXTRA_ARGS="-W 1920"),
                         "sdl|0|-W 1920")

    def test_new_keys_win(self):
        self.assertEqual(self.resolve(BRINEWELL_GAMESCOPE_BACKEND="headless",
                                      APPLEDECK_GAMESCOPE_BACKEND="sdl"),
                         "headless||")


if __name__ == "__main__":
    unittest.main()
