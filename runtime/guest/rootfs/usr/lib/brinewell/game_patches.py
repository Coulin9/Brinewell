"""Explicit game-specific launch adjustments, independent of Proton generation."""

import fcntl
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import tempfile

_loader = importlib.machinery.SourceFileLoader(
    "brinewell_graphics", str(Path(__file__).with_name("brinewell_graphics.py")))
_spec = importlib.util.spec_from_loader(_loader.name, _loader)
graphics = importlib.util.module_from_spec(_spec)
_loader.exec_module(graphics)


MODE_FILE = Path("/run/brinewell/graphics-backend")

PATCHES = {
    "1091500": "cyberpunk2077",
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


def remove_owned_files(state):
    state = Path(state)
    if not state.is_dir():
        return
    with (state / "files.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for record_path in state.glob("*.json"):
            record = json.loads(record_path.read_text())
            target = Path(record["target"])
            if not target.is_symlink() and target.is_file() and digest(target) == record["sha256"]:
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
    if graphics.graphics_overridden(environment):
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
            or (appid == "2622380" and name not in ("nightreign.exe", "start_protected_game.exe"))
            or (appid == "814380" and name != "sekiro.exe")):
        return command, environment, None
    try:
        mode = MODE_FILE.read_text().strip()
    except OSError:
        mode = ""
    mode = graphics.effective_mode(mode, appid, exe_path=value, install_dir=install_value)
    if mode not in ("neptune", "mullion") or (mode == "neptune" and arch != "x86_64"):
        return command, environment, None
    identity = {"appid": appid, "prefix": str(Path(prefix_value).resolve() / "pfx"),
                "executable": str(executable), "affinity": False}
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
