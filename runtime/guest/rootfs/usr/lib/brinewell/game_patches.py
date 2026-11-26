"""Explicit game-specific launch adjustments, independent of Proton generation."""

import errno
import fcntl
from contextlib import contextmanager
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import sys
import stat
import secrets

_loader = importlib.machinery.SourceFileLoader(
    "brinewell_graphics", str(Path(__file__).with_name("brinewell_graphics.py")))
_spec = importlib.util.spec_from_loader(_loader.name, _loader)
graphics = importlib.util.module_from_spec(_spec)
_loader.exec_module(graphics)


MODE_FILE = Path("/run/brinewell/graphics-backend")

PATCHES = {
    "1091500": "cyberpunk2077",
    "1174180": "rdr2",
    "223850": "3dmark",
    "2622380": "nightreign",
    "814380": "sekiro",
}


def profile_appid(profile):
    for appid, name in PATCHES.items():
        if profile in (appid, name):
            return appid
    raise ValueError("Unknown game patch profile: " + profile)


def patch_state_dir(appid):
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    return data / "brinewell/game-patches/files" / appid


def three_dmark_env(environment):
    environment = dict(environment)
    # Preserve the original quoting and spacing of caller-supplied Java options.
    original = environment.get("JAVA_TOOL_OPTIONS", "")
    additions = [option for option in ("-Dsun.java2d.d3d=false", "-Xint")
                 if option not in original.split()]
    environment["JAVA_TOOL_OPTIONS"] = " ".join([original, *additions]).strip()
    environment["FEX_DISKCACHE"] = "0"
    return environment


def three_dmark_arguments(arguments):
    arguments = list(arguments)
    if "--disable-gpu" not in arguments:
        arguments.append("--disable-gpu")
    systeminfo = next((value for value in reversed(arguments) if value.startswith("--systeminfo=")), None)
    if systeminfo is None:
        systeminfo = "--systeminfo=off"
        arguments.append(systeminfo)
    if systeminfo == "--systeminfo=off" and not any(
            value == "--gpuCount" or value.startswith("--gpuCount=") for value in arguments):
        arguments.append("--gpuCount=1")
    return arguments


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_record(path, record):
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(record, stream)
            stream.write("\n")
            stream.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def ensure_compiler(source, target, state):
    """Create a missing compiler, or update a previous copy still owned by us."""
    source, target, state = Path(source), Path(target), Path(state)
    if not source.is_file() or not target.parent.is_dir():
        return
    key = hashlib.sha256(os.fsencode(str(target.absolute()))).hexdigest()
    record_path = state / (key + ".json")
    if target.exists() and not record_path.exists():
        return
    state.mkdir(parents=True, exist_ok=True)
    with (state / "files.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        record = json.loads(record_path.read_text()) if record_path.exists() else None
        if target.is_symlink():
            return
        if target.exists() and (not record or record.get("target") != str(target.absolute())
                                or digest(target) != record.get("sha256")):
            return
        content = source.read_bytes()
        checksum = hashlib.sha256(content).hexdigest()
        if target.is_file() and digest(target) == checksum:
            return
        with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                stream.write(content)
                stream.close()
                temporary.chmod(source.stat().st_mode & 0o777)
                if target.exists():
                    temporary.replace(target)
                else:
                    try:
                        os.link(temporary, target)
                    except FileExistsError:
                        return
                write_record(record_path, {"target": str(target.absolute()), "sha256": checksum})
            finally:
                temporary.unlink(missing_ok=True)


RDR2_SETTINGS = Path("drive_c/users/steamuser/Documents/Rockstar Games/Red Dead Redemption 2/Settings/system.xml")
RDR2_SEED = b'''<?xml version="1.0" encoding="UTF-8"?>

<rage__fwuiSystemSettingsCollection>
  <version value="37" />
  <advancedGraphics>
    <API>kSettingAPI_DX12</API>
  </advancedGraphics>
</rage__fwuiSystemSettingsCollection>
'''


class PrefixNotReady(Exception):
    pass


@contextmanager
def rdr2_settings_dir(prefix, create):
    """Pin every directory from compatdata through Settings without following links."""
    prefix = Path(prefix)
    fds = []
    try:
        try:
            fds.append(os.open(prefix.parent, os.O_RDONLY | os.O_DIRECTORY))
        except FileNotFoundError:
            if create:
                raise PrefixNotReady from None
            raise
        names = (prefix.name, "drive_c", "users", "steamuser", "Documents",
                 "Rockstar Games", "Red Dead Redemption 2", "Settings")
        for index, name in enumerate(names):
            if create and index >= 5:
                try:
                    os.mkdir(name, 0o755, dir_fd=fds[-1])
                except FileExistsError:
                    pass
            try:
                fds.append(os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                   dir_fd=fds[-1]))
            except FileNotFoundError:
                if create and index < 5:
                    raise PrefixNotReady from None
                raise
        yield fds[-1]
    finally:
        for fd in reversed(fds):
            os.close(fd)


@contextmanager
def state_lock(state, nonblocking=False):
    with (state / "files.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | (fcntl.LOCK_NB if nonblocking else 0))
        yield


def seed_rdr2_settings(prefix, state, nonblocking=False):
    """Return False only while Proton's Documents directory is not ready."""
    prefix, state = Path(prefix), Path(state)
    target = prefix / RDR2_SETTINGS
    try:
        with rdr2_settings_dir(prefix, True) as settings_fd:
            state.mkdir(parents=True, exist_ok=True)
            key = hashlib.sha256(os.fsencode(str(target.absolute()))).hexdigest()
            with state_lock(state, nonblocking):
                record_path = state / (key + ".json")
                try:
                    os.stat("system.xml", dir_fd=settings_fd, follow_symlinks=False)
                except FileNotFoundError:
                    pass
                else:
                    return True
                flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW

                def write_seed(fd):
                    with os.fdopen(fd, "wb") as stream:
                        stream.write(RDR2_SEED)
                        stream.flush()
                        info = os.fstat(stream.fileno())
                    return {"target": str(target.absolute()),
                            "sha256": hashlib.sha256(RDR2_SEED).hexdigest(),
                            "st_dev": info.st_dev, "st_ino": info.st_ino,
                            "st_size": info.st_size, "st_mtime_ns": info.st_mtime_ns}

                tmp_name = ".system.xml.brinewell.{}.{}".format(os.getpid(), secrets.token_hex(16))
                fd = os.open(tmp_name, flags, 0o644, dir_fd=settings_fd)
                staged = True
                try:
                    record = write_seed(fd)
                    write_record(record_path, record)
                    try:
                        os.link(tmp_name, "system.xml", src_dir_fd=settings_fd, dst_dir_fd=settings_fd)
                    except FileExistsError:
                        record_path.unlink(missing_ok=True)
                    except OSError as error:
                        if error.errno not in (errno.EPERM, errno.ENOTSUP, errno.EXDEV, errno.EOPNOTSUPP):
                            raise
                        record_path.unlink(missing_ok=True)
                        os.unlink(tmp_name, dir_fd=settings_fd)
                        staged = False
                        try:
                            fd = os.open("system.xml", flags, 0o644, dir_fd=settings_fd)
                        except FileExistsError:
                            return True
                        record = write_seed(fd)
                        write_record(record_path, record)
                finally:
                    if staged:
                        try:
                            os.unlink(tmp_name, dir_fd=settings_fd)
                        except FileNotFoundError:
                            pass
        return True
    except PrefixNotReady:
        return False


def cleanup_rdr2_settings(prefix, state):
    prefix, state = Path(prefix), Path(state)
    if not state.is_dir():
        return
    target = prefix / RDR2_SETTINGS
    key = hashlib.sha256(os.fsencode(str(target.absolute()))).hexdigest()
    record_path = state / (key + ".json")
    with state_lock(state):
        if not record_path.exists():
            return
        record = json.loads(record_path.read_text())
        try:
            with rdr2_settings_dir(prefix, False) as settings_fd:
                try:
                    fd = os.open("system.xml", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                                 dir_fd=settings_fd)
                except OSError as error:
                    if error.errno != errno.ELOOP:
                        raise
                else:
                    with os.fdopen(fd, "rb") as stream:
                        info = os.fstat(stream.fileno())
                        if (stat.S_ISREG(info.st_mode)
                                and record.get("target") == str(target.absolute())
                                and all(record.get(name) == getattr(info, name)
                                        for name in ("st_dev", "st_ino", "st_size", "st_mtime_ns"))
                                and record.get("sha256") == hashlib.sha256(
                                    stream.read(info.st_size + 1)).hexdigest()):
                            current = os.stat("system.xml", dir_fd=settings_fd, follow_symlinks=False)
                            if (stat.S_ISREG(current.st_mode)
                                    and all(getattr(current, name) == getattr(info, name)
                                            for name in ("st_dev", "st_ino", "st_size", "st_mtime_ns"))):
                                os.unlink("system.xml", dir_fd=settings_fd)
        except FileNotFoundError:
            pass
        record_path.unlink(missing_ok=True)


def try_seed_rdr2(prefix, state, nonblocking=False):
    try:
        return seed_rdr2_settings(prefix, state, nonblocking=nonblocking)
    except BlockingIOError:
        raise
    except OSError as error:
        print("brinewell-game-launch: RDR2 seed failed: " + str(error), file=sys.stderr)
        return True


def remove_owned_files(state, prefix=None):
    state = Path(state)
    if not state.is_dir():
        return
    with (state / "files.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for record_path in state.glob("*.json"):
            record = json.loads(record_path.read_text())
            target = Path(record["target"])
            if prefix is not None and target != Path(prefix) / RDR2_SETTINGS:
                continue
            safe = prefix is None or target.resolve().is_relative_to(prefix)
            if safe and not target.is_symlink() and target.is_file() and digest(target) == record["sha256"]:
                target.unlink()
            record_path.unlink()


def prepare(profile, command, environment, cmdline):
    """Return argv, environment and the verified launch identity for one profile."""
    appid = profile_appid(profile)
    command, environment = list(command), dict(environment)
    app_ids = {environment[name] for name in ("STEAM_COMPAT_APP_ID", "SteamAppId", "SteamGameId")
               if environment.get(name)}
    if app_ids != {appid}:
        return command, environment, None
    install_value = environment.get("STEAM_COMPAT_INSTALL_PATH", "")
    prefix_value = environment.get("STEAM_COMPAT_DATA_PATH", "")
    if not os.path.isabs(install_value) or not os.path.isabs(prefix_value):
        return command, environment, None
    # Same qualification as the launch wrapper: a validated Proton game
    # launch under a tool root, supported architecture/backend, and no user
    # graphics override that would conflict with the Neptune payload.
    # The command may still carry the outer FEX/pv-adverb wrapper chain; the
    # Proton invocation begins after one of its "--" separators.
    starts = [0] + [i + 1 for i, value in enumerate(command) if value == "--"]
    info = None
    offset = 0
    for start in starts:
        info = graphics.proton_invocation(command[start:], environment)
        if info:
            offset = start
            break
    if not info:
        return command, environment, None
    tool, verb_index = info
    verb_index += offset
    fex = verb_index >= 2 and Path(command[verb_index - 2]).name == "FEXCompatTool"
    arch = graphics.select_arch(tool, environment, fex)
    if arch is None:
        return command, environment, None
    overridden = graphics.graphics_overridden(environment)
    if overridden and appid != "1174180":
        return command, environment, None
    install = Path(install_value).resolve()
    if verb_index + 1 >= len(command):
        return command, environment, None
    value = command[verb_index + 1]
    if not value.lower().endswith(".exe"):
        return command, environment, None
    executable = Path(value).resolve()
    if not executable.is_relative_to(install) or not executable.is_file():
        return command, environment, None
    index = verb_index + 1
    name = executable.name.lower()
    if ((appid == "1091500" and name != "redprelauncher.exe")
            or (appid == "1174180" and name not in ("playrdr2.exe", "rdr2.exe"))
            or (appid == "2622380" and name not in ("nightreign.exe", "start_protected_game.exe"))
            or (appid == "814380" and name != "sekiro.exe")):
        return command, environment, None
    try:
        mode = MODE_FILE.read_text().strip()
    except OSError:
        mode = ""
    mode = graphics.effective_mode(mode, appid, exe_path=value, install_dir=install_value)
    supported = mode in ("neptune", "mullion") and (mode != "neptune" or arch == "x86_64")
    deferred_seed = None
    if appid == "1174180":
        prefix = Path(prefix_value).resolve() / "pfx"
        arguments = [arg.lower() for arg in command[index + 1:]]
        vulkan = any(arg in ("-sgadriver=vulkan", "-vulkan") or
                     (arg == "-sgadriver" and arguments[i + 1:i + 2] == ["vulkan"])
                     for i, arg in enumerate(arguments))
        state = patch_state_dir(appid)
        if supported and not overridden and not vulkan:
            if not try_seed_rdr2(prefix, state):
                deferred_seed = {"prefix": str(prefix), "state": str(state)}
        else:
            try:
                cleanup_rdr2_settings(prefix, state)
            except OSError as error:
                print("brinewell-game-launch: RDR2 cleanup failed: " + str(error), file=sys.stderr)
    if not supported or overridden:
        return command, environment, None
    identity = {"appid": appid, "prefix": str(Path(prefix_value).resolve() / "pfx"),
                "executable": str(executable), "affinity": False}
    if deferred_seed:
        identity["deferred_seed"] = deferred_seed
    if appid == "1091500":
        if "--launcher-skip" not in command[index + 1:]:
            command.append("--launcher-skip")
    elif appid == "223850":
        environment = three_dmark_env(environment)
        if name == "3dmark.exe":
            command = command[:index + 1] + three_dmark_arguments(command[index + 1:])
            ensure_compiler(executable.with_name("d3dcompiler_47.dll"),
                            install / "dlc/time-spy-test/bin/x64/d3dcompiler_47.dll",
                            patch_state_dir(appid))
    elif appid == "2622380":
        identity["affinity"] = True
        identity["processes"] = ("nightreign.exe", "start_protected")
        identity["game"] = "nightreign.exe"
    elif appid == "814380":
        identity["affinity"] = True
        identity["processes"] = ("sekiro.exe",)
        identity["game"] = "sekiro.exe"
    return command, environment, identity
