"""Shared qualification checks for Brinewell per-launch graphics routing.

Used by brinewell-graphics-wrap (bubblewrap delegation) and
game_patches.prepare (LaunchOptions stage) so both agree on whether a
command tail is a real Proton game launch and which architecture/backend
it selects.
"""

import os
from pathlib import Path
import struct

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
