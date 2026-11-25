"""Shared qualification checks for Brinewell per-launch graphics routing.

Used by brinewell-graphics-wrap (bubblewrap delegation) and
game_patches.prepare (LaunchOptions stage) so both agree on whether a
command tail is a real Proton game launch and which architecture/backend
it selects.
"""

import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time

LIVE_MODES = ("automatic", "venus", "neptune", "mullion")
CONCRETE_MODES = ("venus", "neptune", "mullion")
RESOLUTION_TOKENS = CONCRETE_MODES + ("probe", "probe_nomullion")
DEFAULT_RESOLVE_SOCK = "/run/brinewell/graphics-resolve.sock"


def valid_app_id(app_id):
    return (isinstance(app_id, str) and 1 <= len(app_id) <= 10
            and app_id[0] in "123456789"
            and all("0" <= c <= "9" for c in app_id)
            and int(app_id) <= 4294967295)


def resolve_backend_for_app(app_id, sock_path=None, timeout=6.0):
    if not valid_app_id(app_id):
        return "venus"
    if sock_path is None:
        sock_path = os.environ.get("BRINEWELL_GRAPHICS_RESOLVE_SOCK", DEFAULT_RESOLVE_SOCK)
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout)
            client.connect(sock_path)
            client.sendall((app_id + "\n").encode("ascii"))
            data = b""
            while b"\n" not in data and len(data) < 64:
                chunk = client.recv(64 - len(data))
                if not chunk:
                    break
                data += chunk
            line, separator, _ = data.partition(b"\n")
            if separator:
                mode = line.decode("ascii")
                if mode in RESOLUTION_TOKENS:
                    return mode
    except (OSError, ValueError, UnicodeError):
        pass
    return "venus"


def probe_local_backend(exe_path=None, install_dir=None, allow_mullion=True,
                        probe_bin=None, timeout=3.0):
    probe_bin = probe_bin or os.environ.get(
        "BRINEWELL_API_PROBE_BIN", "/usr/lib/brinewell/brinewell-api-probe")
    deadline = time.monotonic() + timeout
    try:
        if not install_dir or not os.path.isabs(install_dir):
            return "venus"
        install_dir = os.path.normpath(install_dir)
        if os.path.islink(install_dir) or not os.path.isdir(install_dir):
            return "venus"
        install = Path(install_dir).resolve(strict=True)
    except (OSError, RuntimeError, ValueError):
        return "venus"

    executable = None
    try:
        if exe_path and os.path.isabs(exe_path):
            candidate = Path(exe_path).resolve(strict=True)
            if candidate.is_file() and candidate.is_relative_to(install):
                executable = candidate
    except (OSError, RuntimeError, ValueError):
        pass

    def run(operation, path):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        try:
            result = subprocess.run([probe_bin, "--json", operation, str(path)],
                                    capture_output=True, timeout=remaining, check=False)
            backend = json.loads(result.stdout).get("backend")
            return backend if backend in CONCRETE_MODES else None
        except (OSError, subprocess.TimeoutExpired, ValueError, AttributeError):
            return None

    backend = run("check", executable) if executable else "venus"
    if backend == "venus" and install:
        backend = run("scan", install)
    if backend not in CONCRETE_MODES or (backend == "mullion" and not allow_mullion):
        return "venus"
    return backend


def effective_mode(live_mode, app_id=None, sock_path=None, timeout=6.0,
                   exe_path=None, install_dir=None, probe_bin=None, probe_timeout=3.0):
    if live_mode in CONCRETE_MODES:
        return live_mode
    if live_mode == "automatic" and valid_app_id(app_id):
        mode = resolve_backend_for_app(app_id, sock_path=sock_path, timeout=timeout)
        if mode in ("probe", "probe_nomullion"):
            return probe_local_backend(exe_path, install_dir, mode == "probe",
                                       probe_bin, probe_timeout)
        if mode in CONCRETE_MODES:
            return mode
    return "venus"

PAYLOAD = Path("/usr/lib/brinewell/neptune")
CONTAINER_PAYLOAD = Path("/run/host/usr/lib/brinewell/neptune")
GRAPHICS_DLLS = ("d3d11", "dxgi", "d3d12")
VERBS = ("run", "waitforexitandrun")
ELF_MACHINE = {"x86_64": 62, "aarch64": 183}
# PE machine types: x64 also covers ARM64EC modules (Proton installs ARM64EC
# graphics DLLs in the aarch64 slot).
PE_MACHINES = {"x86_64": {0x8664}, "aarch64": {0xAA64, 0xA641, 0x8664}}


def _real(path):
    try:
        return Path(os.path.realpath(path))
    except OSError:
        return None


def tool_paths(environ=None):
    """Absolute STEAM_COMPAT_TOOL_PATHS entries only; no implied defaults."""
    environ = os.environ if environ is None else environ
    return [p for p in environ.get("STEAM_COMPAT_TOOL_PATHS", "").split(":") if p]


def proton_invocation(argv, environ=None):
    """Return (tool_dir, verb_index) for a real Proton game launch tail.

    Recognizes only `/.../<tool>/proton <verb>` or
    `FEXCompatTool /.../<tool>/proton <verb>` with a numeric AppID and an
    absolute compatdata path. The tool directory itself must equal a
    STEAM_COMPAT_TOOL_PATHS entry (each entry is one tool's directory).
    """
    environ = os.environ if environ is None else environ
    if not argv:
        return None
    i = 0
    if Path(argv[0]).name == "FEXCompatTool":
        if len(argv) < 2:
            return None
        i = 1
    proton = argv[i] if i < len(argv) else None
    if not proton or Path(proton).name != "proton" or not os.path.isabs(proton):
        return None
    if not os.access(proton, os.X_OK):
        return None
    verb = argv[i + 1] if i + 1 < len(argv) else ""
    if verb not in VERBS:
        return None
    if verb == "run" and (i + 2 >= len(argv) or argv[i + 2].startswith("--")):
        return None
    app_id = (environ.get("STEAM_COMPAT_APP_ID") or environ.get("SteamGameId")
              or environ.get("SteamAppId") or "")
    if not app_id.isdecimal():
        return None
    compat = environ.get("STEAM_COMPAT_DATA_PATH", "")
    if not compat or not os.path.isabs(compat):
        return None
    tool = _real(Path(proton).parent)
    if tool is None:
        return None
    for entry in tool_paths(environ):
        root = _real(entry)
        if root is not None and tool == root:
            return tool, i + 1
    return None


def _elf_machine(path):
    try:
        with open(path, "rb") as handle:
            data = handle.read(20)
        if data[:4] != b"\x7fELF" or len(data) < 20:
            return None
        return struct.unpack("<H", data[18:20])[0]
    except OSError:
        return None


def _pe_machine(path):
    try:
        with open(path, "rb") as handle:
            data = handle.read(4096)
        if data[:2] != b"MZ" or len(data) < 0x40:
            return None
        offset = struct.unpack("<I", data[0x3c:0x40])[0]
        if offset + 6 > len(data) or data[offset:offset + 4] != b"PE\0\0":
            return None
        return struct.unpack("<H", data[offset + 4:offset + 6])[0]
    except OSError:
        return None


def _arch_ok(tool, arch):
    """Proof the tool actually carries this architecture's wine layout."""
    wine = Path(tool) / "files/lib/wine"
    ntdll = wine / f"{arch}-unix/ntdll.so"
    if _elf_machine(ntdll) != ELF_MACHINE[arch]:
        return False
    pe = wine / f"dxvk/{arch}-windows/{GRAPHICS_DLLS[0]}.dll"
    return pe.is_file() and _pe_machine(pe) in PE_MACHINES[arch]


def _available_arch(tool):
    return [arch for arch in ("x86_64", "aarch64") if _arch_ok(tool, arch)]


def select_arch(tool, environ=None, fex=False):
    """Return the architecture the launch will actually use, or None.

    Mirrors Proton's condition: a valid explicit PROTON_USE_ARM64 wins;
    a malformed value is unknown rather than assumed ARM. For a dual-arch
    tool, FEX launches run the x86 side; a native-ARM tool (executable
    files/bin-arm64/wine with PROTON_USE_ARM64 absent or '1') runs arm64;
    anything else is ambiguous and skipped.
    """
    environ = os.environ if environ is None else environ
    explicit = environ.get("PROTON_USE_ARM64", "").strip().lower()
    tool = Path(tool)
    if explicit:
        if explicit in ("1", "true", "yes"):
            return "aarch64" if _arch_ok(tool, "aarch64") else None
        if explicit in ("0", "false", "no"):
            return "x86_64" if _arch_ok(tool, "x86_64") else None
        return None
    available = _available_arch(tool)
    if len(available) == 1:
        return available[0]
    if not available:
        # Native arm64 Proton without a dxvk per-arch layout.
        if (tool / "files/bin-arm64/wine").is_file() and \
                os.access(tool / "files/bin-arm64/wine", os.X_OK):
            return "aarch64"
        return None
    # Dual layout.
    if fex:
        return "x86_64"
    native_arm = tool / "files/bin-arm64/wine"
    if native_arm.is_file() and os.access(native_arm, os.X_OK):
        return "aarch64"
    return None


def graphics_overridden(environ=None):
    """True when the user already controls the graphics DLL stack."""
    environ = os.environ if environ is None else environ
    if environ.get("PROTON_USE_WINED3D"):
        return True
    for entry in environ.get("WINEDLLOVERRIDES", "").split(";"):
        left = entry.strip().split("=")[0]
        for key in left.split(","):
            key = key.strip().lower()
            if key.endswith(".dll"):
                key = key[:-4]
            if not key:
                continue
            if "*" in key or "?" in key or key in GRAPHICS_DLLS or key == "nptunix":
                return True
    return False


def multi_ring_opt_in():
    try:
        return "brinewell.neptune.multi_ring=1" in Path("/proc/cmdline").read_text()
    except OSError:
        return False


def npt_perf(env_value):
    """Merge `multi_ring` into NPT_PERF only when the boot opted in."""
    if not multi_ring_opt_in():
        return None
    existing = (env_value or "").split(",")
    flags = [f for f in existing if f]
    if "multi_ring" not in flags:
        flags.append("multi_ring")
    return ",".join(flags)
