# brinewell: generic Steam VDF helpers shared by guest pre-Steam steps.
# Every function preserves all unrelated bytes of the original document.
import os
import re
import tempfile

VDF_TOOLS = ("compatibilitytools", "compat_tools")
MAPPING = ("InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping")


def atomic_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as out:
            out.write(text)
        os.chmod(tmp, path.stat().st_mode & 0o777 if path.exists() else 0o600)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def vdf_unquote(token):
    if not token.startswith('"'):
        return token
    return re.sub(r'\\([\\"nrt])', lambda m: {"n": "\n", "r": "\r", "t": "\t"}.get(m[1], m[1]), token[1:-1])


def vdf_quote(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t") + '"'


def vdf_entries(text):
    """Scalar spans, block closing offsets and block key offsets."""
    tokens = [m for m in re.finditer(r'//[^\n]*|"(?:\\.|[^"\\])*"|[{}]|[^\s{}"]+', text)
              if not m[0].startswith("//")]
    values, blocks, starts, stack = {}, {}, {}, []
    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token[0] == "}":
            if not stack:
                raise ValueError("unbalanced VDF closing brace")
            blocks[tuple(stack)] = token.start()
            stack.pop()
            i += 1
            continue
        if token[0] == "{" or i + 1 == len(tokens):
            raise ValueError("incomplete VDF key/value")
        key = vdf_unquote(token[0]).lower()
        value = tokens[i + 1]
        if value[0] == "{":
            stack.append(key)
            starts[tuple(stack)] = token.start()
        elif value[0] == "}":
            raise ValueError("missing VDF value")
        else:
            path = (*stack, key)
            if path in values:
                raise ValueError("duplicate VDF value")
            values[path] = (vdf_unquote(value[0]), value.start(), value.end())
        i += 2
    if stack:
        raise ValueError("unbalanced VDF opening brace")
    return values, blocks, starts


def vdf_set(text, path, value):
    values, blocks, _ = vdf_entries(text)
    key = tuple(part.lower() for part in path)
    if key in values:
        _, start, end = values[key]
        return text[:start] + vdf_quote(value) + text[end:]
    depth = len(key) - 1
    while depth and key[:depth] not in blocks:
        depth -= 1
    if not depth:
        raise ValueError("no " + path[0] + " block")
    addition = "\n" + "\t" * depth + vdf_quote(path[-1]) + "\t\t" + vdf_quote(value) + "\n"
    for level in range(len(key) - 2, depth - 1, -1):
        addition = "\n" + "\t" * level + vdf_quote(path[level]) + "\n" + "\t" * level + "{" + addition + "\t" * level + "}\n"
    pos = blocks[key[:depth]]
    return text[:pos] + addition + text[pos:]


def vdf_remove(text, path):
    """Drop the block at path (its key through its closing brace and that line's newline)."""
    _, blocks, starts = vdf_entries(text)
    key = tuple(part.lower() for part in path)
    if key not in starts:
        return text
    start, end = starts[key], blocks[key] + 1
    while start and text[start - 1] in " \t":
        start -= 1
    if text[end:end + 1] == "\n":
        end += 1
    return text[:start] + text[end:]


def mappings(text):
    """appid -> {name, config, priority} of every CompatToolMapping entry."""
    values, _, _ = vdf_entries(text)
    parent = tuple(part.lower() for part in MAPPING)
    found = {}
    for path, (value, _, _) in values.items():
        if len(path) == len(parent) + 2 and path[:len(parent)] == parent:
            found.setdefault(path[-2], {})[path[-1]] = value
    return found


def set_mapping(text, appid, entry):
    """The appid's entry becomes exactly `entry` (None: removed)."""
    text = vdf_remove(text, (*MAPPING, appid))
    for name, value in (entry or {}).items():
        text = vdf_set(text, (*MAPPING, appid, name), value)
    return text
