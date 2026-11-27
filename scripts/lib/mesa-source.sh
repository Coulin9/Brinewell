# Shared official source for the macOS driver, SteamOS drivers, and Neptune.
# Source after common.sh.
MESA_SRC="$OUT/kosmicrisp/src"
read -r MESA_TAG MESA_COMMIT MESA_URL MESA_SHA256 < <(python3 - "$ROOT/deps.lock.json" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    mesa = json.load(f)["mesa"]
print(mesa["tag"], mesa["commit"], mesa["url"], mesa["sha256"])
PY
)

# Hash the files and symlink targets as well as their relative paths. Keep
# the receipt outside the tree so the build input matches the release archive.
mesa_tree_digest() {
    mtree -c -n -k type,link,sha256digest -p "$1" | shasum -a 256 | awk '{print $1}'
}

ensure_mesa_source() (
    local parent="${MESA_SRC%/*}" receipt="${MESA_SRC%/*}/source.sha256"
    local archive="$CACHE/$MESA_TAG.tar.xz" stage digest
    mkdir -p "$parent"
    exec 9>"$CACHE/mesa-source.lock"
    lockf 9
    if [[ -f "$receipt" && -d "$MESA_SRC" ]]; then
        digest="$(mesa_tree_digest "$MESA_SRC")"
        if [[ "$(<"$receipt")" == "$MESA_SHA256 $digest" ]]; then return; fi
    fi

    fetch_verified "$MESA_URL" "$archive" "$MESA_SHA256"
    stage="$(mktemp -d "$parent/.mesa-source.XXXXXX")"
    trap 'rm -rf "$stage"' EXIT
    mkdir "$stage/src"
    tar -xf "$archive" --strip-components=1 -C "$stage/src"
    digest="$(mesa_tree_digest "$stage/src")"
    printf '%s %s\n' "$MESA_SHA256" "$digest" > "$stage/receipt"
    if [[ -e "$MESA_SRC" ]]; then mv "$MESA_SRC" "$stage/previous"; fi
    mv "$stage/src" "$MESA_SRC"
    mv "$stage/receipt" "$receipt"
)
