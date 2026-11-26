import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPTS = Path(__file__).resolve().parents[2] / "runtime/guest/rootfs/usr/lib/brinewell"
spec = importlib.util.spec_from_file_location("game_patch_settings", SCRIPTS / "game_patch_settings.py")
settings = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(SCRIPTS), *sys.path]):
    spec.loader.exec_module(settings)
    import brinewell_vdf as vdf


class GamePatchSettingsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.steam = self.root / "Steam"
        self.state = self.root / "state/neptune"
        self.config = self.steam / "userdata/42/config/localconfig.vdf"
        self.config.parent.mkdir(parents=True)
        self.config.write_text('"UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "apps" {} } } } }')

    def options(self, appid="1091500"):
        values, _, _ = vdf.vdf_entries(self.config.read_text())
        path = tuple(part.lower() for part in (*settings.APP_PATH, appid, "LaunchOptions"))
        return values.get(path, (None,))[0]

    def set_options(self, value):
        path = (*settings.APP_PATH, "1091500", "LaunchOptions")
        self.config.write_text(vdf.vdf_set(self.config.read_text(), path, value))

    def test_all_supported_patches_are_defaults(self):
        settings.synchronize(self.steam, self.state)
        self.assertEqual(settings.PATCHES["1174180"], "rdr2")
        self.assertEqual(self.options("1174180"),
                         "/usr/lib/brinewell/brinewell-game-launch --profile rdr2 -- %command%")
        for appid in settings.PATCHES:
            self.assertEqual(self.options(appid), settings.launch_options(appid, None))
        before = self.config.read_bytes()
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.config.read_bytes(), before)

    def test_previous_disabled_switches_do_not_disable_defaults(self):
        settings.save_settings(self.state, {"schema": 1, "enabled": [], "accounts": {}})
        settings.synchronize(self.steam, self.state)
        self.assertNotIn("enabled", settings.load_settings(self.state))
        for appid in settings.PATCHES:
            self.assertEqual(self.options(appid), settings.launch_options(appid, None))

    def test_preserves_existing_environment_and_game_arguments(self):
        previous = 'FOO="two words" gamemoderun %command% --intro-skip'
        self.set_options(previous)
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), previous.replace("%command%", settings.launch_options("1091500", None)))

    def test_plain_arguments_are_appended_to_the_original_steam_command(self):
        self.set_options('-language "en US"')
        settings.synchronize(self.steam, self.state)
        self.assertTrue(self.options().endswith('%command% -language "en US"'))

    def test_user_changes_revoke_ownership(self):
        settings.synchronize(self.steam, self.state)
        self.set_options("%command% --user-change")
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), "%command% --user-change")

    def test_complex_shell_options_are_logged_and_preserved(self):
        previous = '%command% && printf finished'
        self.set_options(previous)
        with patch.object(settings, "print", create=True) as report:
            settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), previous)
        self.assertTrue(report.called)

    def test_command_placeholder_inside_an_argument_is_preserved(self):
        previous = 'wrapper "run %command% now"'
        self.set_options(previous)
        with patch.object(settings, "print", create=True) as report:
            settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), previous)
        self.assertTrue(report.called)

    def test_invalid_account_configuration_does_not_block_other_accounts(self):
        invalid = self.steam / "userdata/1/config/localconfig.vdf"
        invalid.parent.mkdir(parents=True)
        original = '"UserLocalConfigStore" {'
        invalid.write_text(original)
        with patch.object(settings, "print", create=True) as report:
            settings.synchronize(self.steam, self.state)
        self.assertEqual(invalid.read_text(), original)
        self.assertTrue(report.called)
        for appid in settings.PATCHES:
            self.assertEqual(self.options(appid), settings.launch_options(appid, None))

    def test_interrupted_write_resumes_from_original_options(self):
        original = "--intro-skip"
        self.set_options(original)
        settings.save_settings(self.state, {
            "schema": 2, "accounts": {"42": {
                "1091500": {"previous": original, "applied": settings.launch_options("1091500", original), "pending": True},
            }},
        })
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), settings.launch_options("1091500", original))

    def test_removing_our_launcher_counts_as_a_user_edit(self):
        previous = "--intro-skip"
        self.set_options(previous)
        settings.synchronize(self.steam, self.state)
        self.set_options(previous)
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), previous)

    def test_unrelated_game_options_are_preserved(self):
        path = (*settings.APP_PATH, "42", "LaunchOptions")
        self.config.write_text(vdf.vdf_set(self.config.read_text(), path, "--user-option"))
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options("42"), "--user-option")

    def legacy_options(self, appid, previous):
        return settings.launch_options(appid, previous).replace(
            settings.LAUNCHER, settings.LEGACY_LAUNCHER)

    def test_legacy_applied_record_is_rewritten_to_the_new_path(self):
        # A record/localconfig pair written by an AppleDeck release is still
        # the managed wrapper; the next sync rewrites it to the new path.
        previous = "--intro-skip"
        legacy = self.legacy_options("1091500", previous)
        settings.save_settings(self.state, {
            "schema": 2, "accounts": {"42": {
                "1091500": {"previous": previous, "applied": legacy},
            }},
        })
        self.set_options(legacy)
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), settings.launch_options("1091500", previous))
        record = settings.load_settings(self.state)["accounts"]["42"]["1091500"]
        self.assertEqual(record["applied"], settings.launch_options("1091500", previous))
        self.assertEqual(record["previous"], previous)

    def test_legacy_applied_record_resumes_after_interrupted_write(self):
        original = "--intro-skip"
        settings.save_settings(self.state, {
            "schema": 2, "accounts": {"42": {
                "1091500": {"previous": original,
                            "applied": self.legacy_options("1091500", original),
                            "pending": True},
            }},
        })
        self.set_options(original)
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), settings.launch_options("1091500", original))

    def test_interrupted_rename_window_resumes_from_prior_applied(self):
        # The ownership record was persisted with the NEW applied value while
        # localconfig.vdf still holds the OLD applied wrapper value: the vdf
        # write never happened. This must resume, not detach.
        original = "--intro-skip"
        legacy = self.legacy_options("1091500", original)
        applied = settings.launch_options("1091500", original)
        settings.save_settings(self.state, {
            "schema": 2, "accounts": {"42": {
                "1091500": {"previous": original, "applied": applied,
                            "pending": True, "priorApplied": legacy},
            }},
        })
        self.set_options(legacy)
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), applied)
        record = settings.load_settings(self.state)["accounts"]["42"]["1091500"]
        self.assertNotIn("detached", record)
        self.assertNotIn("pending", record)

    def test_pending_record_still_detaches_on_real_user_edit(self):
        original = "--intro-skip"
        legacy = self.legacy_options("1091500", original)
        settings.save_settings(self.state, {
            "schema": 2, "accounts": {"42": {
                "1091500": {"previous": original,
                            "applied": settings.launch_options("1091500", original),
                            "pending": True, "priorApplied": legacy},
            }},
        })
        self.set_options("--user-edit %command%")
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), "--user-edit %command%")

    def test_legacy_launcher_value_is_never_double_wrapped(self):
        # No ownership record: the value is the user's and stays untouched
        # (it still runs through the /usr/lib/appledeck compat symlink).
        value = self.legacy_options("1091500", None)
        self.set_options(value)
        with patch.object(settings, "print", create=True) as report:
            settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), value)
        self.assertTrue(report.called)

    def test_repeated_interruptions_still_resume(self):
        # Two consecutive localconfig.vdf write failures then a success: the
        # observed localconfig value (priorApplied) is re-stored every cycle,
        # so the record stays resumable and the options are still rewritten.
        original = "--intro-skip"
        self.set_options(original)
        real_write = vdf.atomic_write
        remaining = [2]

        def flaky(path, text):
            if Path(path) == self.config and remaining[0] > 0:
                remaining[0] -= 1
                raise OSError("interrupted write")
            return real_write(path, text)

        with patch.object(vdf, "atomic_write", flaky):
            for _ in range(2):
                with self.assertRaises(OSError):
                    settings.synchronize(self.steam, self.state)
            settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), settings.launch_options("1091500", original))
        record = settings.load_settings(self.state)["accounts"]["42"]["1091500"]
        self.assertNotIn("detached", record)
        self.assertNotIn("pending", record)
        self.assertNotIn("priorApplied", record)

    def test_pending_record_without_prior_observation_detaches_on_clear(self):
        # The record knows previous="--intro-skip" but has no priorApplied key:
        # an empty LaunchOptions value was never observed mid-write, so the
        # empty field is a user edit, not a resumable state.
        original = "--intro-skip"
        settings.save_settings(self.state, {
            "schema": 2, "accounts": {"42": {
                "1091500": {"previous": original,
                            "applied": settings.launch_options("1091500", original),
                            "pending": True},
            }},
        })
        self.set_options("")
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), "")
        record = settings.load_settings(self.state)["accounts"]["42"]["1091500"]
        self.assertTrue(record.get("detached"))

    def test_user_deleted_options_detach(self):
        settings.synchronize(self.steam, self.state)
        self.set_options("")
        settings.synchronize(self.steam, self.state)
        self.assertEqual(self.options(), "")
        record = settings.load_settings(self.state)["accounts"]["42"]["1091500"]
        self.assertTrue(record.get("detached"))

    def test_default_policy_needs_no_persisted_switches(self):
        self.config.unlink()
        settings.synchronize(self.steam, self.state)
        self.assertEqual(settings.load_settings(self.state), {"schema": 2, "accounts": {}})


if __name__ == "__main__":
    unittest.main()
