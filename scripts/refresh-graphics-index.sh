#!/bin/bash
# Refresh the bundled offline index before making a release.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT="$ROOT/Resources/steam-graphics-index.json"
UPDATER="$ROOT/scripts/tools/update-steam-graphics-index.py"
cmd=(python3 "$UPDATER")
proxy=""
dry_run=0
max_pages=0
checkpoint=""
help_requested=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proxy|--checkpoint|--max-pages)
            option="$1"
            [[ $# -ge 2 && "$2" != --* ]] || { echo "$option requires a value" >&2; exit 1; }
            [[ "$option" != --proxy ]] || proxy="explicit"
            [[ "$option" != --checkpoint ]] || checkpoint="$2"
            [[ "$option" != --max-pages ]] || max_pages=1
            cmd+=("$1" "$2")
            shift 2 ;;
        --proxy=*|--checkpoint=*|--max-pages=*)
            case "$1" in
                --proxy=*) proxy="explicit" ;;
                --checkpoint=*) checkpoint="${1#*=}" ;;
                --max-pages=*) max_pages=1 ;;
            esac
            cmd+=("$1")
            shift ;;
        --dry-run) dry_run=1; cmd+=("$1"); shift ;;
        --help|-h) help_requested=1; cmd+=("$1"); shift ;;
        --out*) echo "--output is managed by this script" >&2; exit 1 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done
if [[ $max_pages -eq 1 && $dry_run -ne 1 ]]; then
    echo "--max-pages requires --dry-run" >&2
    exit 1
fi
if [[ $help_requested -eq 1 ]]; then
    "${cmd[@]}"
    exit 0
fi
# The updater writes both the checkpoint and its .tmp sibling, even in dry runs.
if [[ -n "$checkpoint" ]]; then
    python3 - "$checkpoint" "$OUTPUT" <<'PY'
import os
import sys

checkpoint, output = sys.argv[1:]
output = os.path.realpath(output)
if any(os.path.realpath(path) == output for path in (checkpoint, checkpoint + '.tmp')):
    sys.exit('Checkpoint must not overwrite the bundled graphics index')
PY
fi
if [[ "$proxy" == "explicit" ]]; then
    echo "Graphics index: using the --proxy argument"
elif [[ -n "${https_proxy:-${HTTPS_PROXY:-}}" ]]; then
    echo "Graphics index: using HTTPS proxy from environment"
else
    # Prefer HTTPS, then HTTP; scutil reports the active system proxy settings.
    system_proxy="$(python3 - <<'PY'
import re
import subprocess

try:
    output = subprocess.check_output(['scutil', '--proxy'], text=True, stderr=subprocess.DEVNULL)
except (FileNotFoundError, subprocess.CalledProcessError):
    output = ''
settings = dict(re.findall(r'^\s*(\w+)\s*:\s*(.*?)\s*$', output, re.MULTILINE))
for scheme in ('HTTPS', 'HTTP'):
    if settings.get(scheme + 'Enable') == '1' and settings.get(scheme + 'Proxy') and settings.get(scheme + 'Port', '').isdigit():
        host = settings[scheme + 'Proxy']
        if ':' in host and not host.startswith('['):
            host = '[' + host + ']'
        print('http://' + host + ':' + settings[scheme + 'Port'])
        break
PY
)"
    if [[ -n "$system_proxy" ]]; then
        echo "Graphics index: using macOS system proxy $system_proxy"
        cmd+=(--proxy "$system_proxy")
    else
        echo "Graphics index: using direct connection (no HTTPS or macOS system proxy)"
    fi
fi

# Validate a separate candidate before replacing the bundled file, even if the
# updater succeeds but produces invalid or oversized output.
candidate="$(mktemp "$ROOT/Resources/.steam-graphics-index.XXXXXX")"
trap 'rm -f "$candidate" "$candidate.tmp"' EXIT
cmd+=(--output "$candidate")
"${cmd[@]}"
if [[ $dry_run -eq 1 ]]; then
    echo "Dry run: bundled index unchanged; updater reported candidate counts above."
    exit 0
fi
python3 - "$candidate" <<'PY'
import json
import os
import stat
import sys

path = sys.argv[1]
try:
    info = os.stat(path)
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= 20 * 1024 * 1024:
        raise ValueError('index must be a nonempty regular file of at most 20 MiB')
    with open(path, encoding='utf-8') as stream:
        data = json.load(stream)
    if not isinstance(data, dict) or type(data.get('schemaVersion')) is not int or data['schemaVersion'] != 1:
        raise ValueError('expected schemaVersion 1')
    for key in ('generatedAt', 'source'):
        if not isinstance(data.get(key), str) or not data[key]:
            raise ValueError(f'{key} must be a nonempty string')
    total = 0
    for key in ('neptune', 'mullion', 'venus'):
        values = data.get(key)
        if not isinstance(values, list) or any(type(value) is not int or not 1 <= value <= 0xffffffff for value in values):
            raise ValueError(f'{key} must be an array of valid AppIDs')
        total += len(values)
    if not total:
        raise ValueError('index has no entries')
except (OSError, ValueError) as error:
    sys.exit(f'Invalid graphics index: {error}')
print('Graphics index: ' + ', '.join(f'{key}={len(data[key])}' for key in ('neptune', 'mullion', 'venus')) + f', {info.st_size} bytes')
PY
chmod 644 "$candidate"
mv -f "$candidate" "$OUTPUT"
