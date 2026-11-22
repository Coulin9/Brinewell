#!/bin/bash
# Emit the GPL/LGPL source-links table for a Brinewell release.
#
#   ./scripts/gpl-source-links.sh VERSION [--tag TAG] [--no-check-links]
#
# Writes build/Brinewell-<VERSION>-gpl-sources.md. Nothing outside build/ is
# modified; package inspection runs in a throwaway (--rm) builder container.
set -euo pipefail
source "$(cd "$(dirname "$0")/.." && pwd)/scripts/lib/common.sh"

VERSION=""
TAG=""
CHECK_LINKS=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag)
            [[ $# -ge 2 ]] || die "--tag requires a tag"
            TAG="$2"; shift 2 ;;
        --tag=*) TAG="${1#*=}"; shift ;;
        --no-check-links) CHECK_LINKS=0; shift ;;
        -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) [[ -z "$VERSION" ]] || die "unexpected argument: $1"; VERSION="$1"; shift ;;
    esac
done
[[ -n "$VERSION" ]] || die "usage: $0 VERSION [--tag TAG] [--no-check-links]"
[[ -n "$TAG" ]] || TAG="v$VERSION"

OUTPUT="$ROOT/build/Brinewell-$VERSION-gpl-sources.md"
UU_IMAGE="brinewell-uu-builder:$(sha256_file "$ROOT/runtime/uu/Dockerfile" | cut -c1-12)"

# --- repo / tag ---------------------------------------------------------------
REPO_URL="$(git -C "$ROOT" remote get-url origin)"
REPO_URL="${REPO_URL%.git}"
[[ "$REPO_URL" == https://* ]] || die "origin is not an https remote: $REPO_URL"
[[ -n "$(git -C "$ROOT" ls-remote --tags origin "refs/tags/$TAG")" ]] \
    || die "tag $TAG does not exist on origin"
log "tag $TAG on $REPO_URL"

# --- kernel / initramfs upstream sources --------------------------------------
KVER="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["kernel"]["version"])' "$ROOT/deps.lock.json")"
KURL="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["kernel"]["url"])' "$ROOT/deps.lock.json")"
python3 - "$ROOT/deps.lock.json" > "$BUILD_ROOT/gpl-links-initramfs.list" <<'PY'
import json, sys
i = json.load(open(sys.argv[1]))["initramfs"]
for key in ("dosfstools", "e2fsprogs", "btrfs_progs"):
    print(i[key + "_url"])
PY

# --- busybox (control fields of the shipped .deb, read in the uu builder) -----
BUSYBOX_URL="$(sed -n 's/^BUSYBOX_URL="\(.*\)"$/\1/p' "$ROOT/scripts/components/initramfs.sh")"
BUSYBOX_DEB="$(sed -n 's/^BUSYBOX_DEB="\$CACHE\/\(.*\)"$/\1/p' "$ROOT/scripts/components/initramfs.sh")"
[[ -n "$BUSYBOX_URL" && -n "$BUSYBOX_DEB" ]] || die "could not read BUSYBOX_URL/BUSYBOX_DEB from initramfs.sh"
BUSYBOX_DEB="$CACHE/$BUSYBOX_DEB"
[[ -f "$BUSYBOX_DEB" ]] || die "missing cached deb: $BUSYBOX_DEB"
require_builder
builder image exists "$UU_IMAGE" >/dev/null 2>&1 || die "missing builder image $UU_IMAGE"

read -r BB_BIN BB_VER BB_SRC BB_SRCVER < <(builder run --rm -v "$BUSYBOX_DEB:/in.deb:ro" "$UU_IMAGE" \
    bash -c 'dpkg-deb -f /in.deb Package Version Source' \
    | python3 -c '
import sys
fields = {}
for line in sys.stdin:
    if ":" in line:
        key, val = line.split(":", 1)
        fields[key.strip()] = val.strip()
pkg = fields["Package"]
ver = fields["Version"]
src, sver = pkg, ver
s = fields.get("Source", "")
if s:
    if "(" in s:
        src, sver = s.split("(", 1)[0].strip(), s.split("(", 1)[1].rstrip(")").strip()
    else:
        src = s
print(pkg, ver.split(":", 1)[-1], src, sver)
')
log "busybox source: $BB_SRC $BB_SRCVER (deb $BB_BIN $BB_VER)"

# --- mullion ------------------------------------------------------------------
MULLION="$ROOT/../Mullion"
[[ -d "$MULLION/.git" ]] || die "missing Mullion checkout: ../Mullion"
[[ -z "$(git -C "$MULLION" status --porcelain)" ]] \
    || die "../Mullion has uncommitted or untracked changes"
MULLION_SHA="$(git -C "$MULLION" rev-parse HEAD)"
MULLION_REMOTE="$(git -C "$MULLION" remote get-url origin)"
MULLION_REMOTE="${MULLION_REMOTE%.git}"
[[ "$MULLION_REMOTE" =~ https://github.com/([^/]+)/([^/]+) ]] \
    || die "cannot parse Mullion origin: $MULLION_REMOTE"
MULLION_REPO="${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"
log "mullion $MULLION_SHA ($MULLION_REPO)"

# --- uu-runtime debian source map ---------------------------------------------
[[ -f "$OUT/uu-runtime/root.tar" ]] || die "missing $OUT/uu-runtime/root.tar; build uu-runtime first"
mkdir -p "$BUILD_ROOT/gpl-links-work"
tar -C "$BUILD_ROOT/gpl-links-work" -xf "$OUT/uu-runtime/root.tar" ./usr/share/licenses/brinewell-uu-runtime/build-packages.txt ./usr/lib/brinewell/uu-runtime
UUROOT="$BUILD_ROOT/gpl-links-work/usr/lib/brinewell/uu-runtime"
SHIPPED="$BUILD_ROOT/gpl-links-shipped.list"
{
    for t in curl ip nft xtables-nft-multi iptables ip6tables iptables-save iptables-restore ip6tables-save ip6tables-restore; do
        echo "bin/$t"
    done
    echo "xtables.tar"
    echo "ca-certificates.crt"
    find "$UUROOT/lib" -type f | sed 's|.*/|lib/|'
} > "$SHIPPED"
[[ -s "$SHIPPED" ]] || die "no shipped uu-runtime files found under $UUROOT"
cp "$BUILD_ROOT/gpl-links-work/usr/share/licenses/brinewell-uu-runtime/build-packages.txt" "$BUILD_ROOT/gpl-links-buildpkgs.txt"

builder run --rm -v "$BUILD_ROOT:/work:ro" "$UU_IMAGE" bash -euc '
set -euo pipefail
PKGLIST=/work/gpl-links-buildpkgs.txt
own() { dpkg -S "$1" 2>/dev/null | head -n1 | sed "s/: .*//"; }
meta() { # PKG -> "binver srcname srcver"; fails on version drift vs shipped list
    local rec v s sv
    rec="$(dpkg-query -W -f="\${Version}\t\${source:Package}\t\${source:Version}" "$1")"
    IFS=$'"'"'\t'"'"' read -r v s sv <<< "$rec"
    [[ -n "$s" ]] || s="$1"
    [[ -n "$sv" ]] || sv="$v"
    grep -Fq "$1	$v" "$PKGLIST" || { echo "version drift: $1 is $v" >&2; exit 1; }
    printf "%s\t%s\t%s\n" "$v" "$s" "$sv"
}
emit() { # SHIPPED_RELPATH PKG
    local v s sv
    IFS=$'"'"'\t'"'"' read -r v s sv <<< "$(meta "$2")"
    printf "%s\t%s\t%s\t%s\t%s\n" "$1" "$2" "$v" "$s" "$sv"
}
while read -r rel; do
    case "$rel" in
        bin/*)
            tool="${rel#bin/}"
            case "$tool" in iptables*|ip6tables*) tool=xtables-nft-multi;; esac
            emit "$rel" "$(own "$(readlink -f "$(command -v "$tool")")")" ;;
        xtables.tar)
            emit "$rel" "$(own "$(readlink -f "$(command -v xtables-nft-multi)")")" ;;
        ca-certificates.crt)
            # The bundle file is generated; no package owns the path, so credit its generator.
            emit "$rel" ca-certificates ;;
        lib/*)
            base="${rel#lib/}"
            real="$(find /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu -name "$base" -print -quit)"
            [[ -n "$real" ]] || { echo "cannot resolve shipped file $rel" >&2; exit 1; }
            pkg="$(own "$(readlink -f "$real")")"
            [[ -n "$pkg" ]] || { echo "no package owns $real ($rel)" >&2; exit 1; }
            emit "$rel" "$pkg" ;;
    esac
done < /work/gpl-links-shipped.list
. /etc/os-release
echo "OSRELEASE	${VERSION_ID}	${VERSION_CODENAME:-}"
' > "$BUILD_ROOT/gpl-links-uu.tsv"
SOURCES="$(awk -F'\t' '$1 != "OSRELEASE" {print $4"="$5}' "$BUILD_ROOT/gpl-links-uu.tsv" | sort -u)"
DEBIAN_REL="$(awk -F'\t' '$1 == "OSRELEASE" {print $2" ("$3")"}' "$BUILD_ROOT/gpl-links-uu.tsv")"

urlquote() { python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"; }
snap() { printf 'https://snapshot.debian.org/package/%s/%s/' "$1" "$(urlquote "$2")"; }
BB_UPVER="${BB_SRCVER#*:}"; BB_UPVER="${BB_UPVER%-*}"

{
    printf '| Component | License | Source |\n'
    printf '|---|---|---|\n'
    printf '| Linux kernel %s (`%s-brinewell`) | GPL-2.0 | [%s](%s) + Brinewell'"'"'s [patches](%s/tree/%s/runtime/kernel/patches) and [config](%s/blob/%s/runtime/kernel/config) |\n' \
        "$KVER" "$KVER" "$(basename "$KURL")" "$KURL" "$REPO_URL" "$TAG" "$REPO_URL" "$TAG"
    # busybox shows the upstream version (no epoch, no debian revision):
    printf '| busybox %s (Debian `%s` %s) | GPL-2.0 | [Debian source %s %s](%s) |\n' \
        "${BB_UPVER}" "$BB_BIN" "$BB_VER" "$BB_SRC" "$BB_SRCVER" "$(snap "$BB_SRC" "$BB_SRCVER")"
} > "$OUTPUT"

while read -r url; do
    name="$(basename "$url")"
    case "$name" in
        dosfstools-*)  ver="$(sed -E 's/dosfstools-([0-9.]+)\.tar\.gz/\1/' <<< "$name")"
            printf '| dosfstools %s | GPL-3.0-or-later | [%s](%s) |\n' "$ver" "$name" "$url" ;;
        e2fsprogs-*)   ver="$(sed -E 's/e2fsprogs-([0-9.]+)\.tar\.xz/\1/' <<< "$name")"
            printf '| e2fsprogs %s (initramfs `mke2fs`) | GPL-2.0 | [%s](%s) |\n' "$ver" "$name" "$url" ;;
        btrfs-progs-*) ver="$(sed -E 's/btrfs-progs-v([0-9.]+)\.tar\.xz/\1/' <<< "$name")"
            printf '| btrfs-progs %s | GPL-2.0 | [%s](%s) |\n' "$ver" "$name" "$url" ;;
        *) die "unrecognized initramfs source archive: $name" ;;
    esac
done < "$BUILD_ROOT/gpl-links-initramfs.list" >> "$OUTPUT"

printf '| Mullion (`libmullion-native.dylib`) | LGPL-2.1-or-later | [%s @ %s](%s/tree/%s) |\n' \
    "$MULLION_REPO" "${MULLION_SHA:0:7}" "$MULLION_REMOTE" "$MULLION_SHA" >> "$OUTPUT"
printf '\nUU runtime: tools and libraries taken from Debian %s binary packages, unmodified. Debian source packages:\n\n' \
    "$DEBIAN_REL" >> "$OUTPUT"
printf '| Source package | Version |\n|---|---|\n' >> "$OUTPUT"
while IFS='=' read -r src ver; do
    printf '| [%s](%s) | %s |\n' "$src" "$(snap "$src" "$ver")" "$ver" >> "$OUTPUT"
done <<< "$SOURCES"
log "wrote ${OUTPUT#"$ROOT"/}"

# --- link check -----------------------------------------------------------------
if [[ "$CHECK_LINKS" -eq 1 ]]; then
    URLS="$(grep -oE 'https://[^) |]+' "$OUTPUT" | sort -u)"
    total="$(printf '%s\n' "$URLS" | grep -c .)"
    failures=0
    while read -r url; do
        [[ -n "$url" ]] || continue
        ok=0
        for try in 1 2 3; do
            code="$(curl -sIL --max-time 40 -o /dev/null -w '%{http_code}' "$url" || true)"
            if [[ "$code" == 200 || "$code" == 206 ]]; then ok=1; break; fi
            code="$(curl -sfL --max-time 40 -r 0-0 -o /dev/null -w '%{http_code}' "$url" || true)"
            if [[ "$code" == 200 || "$code" == 206 ]]; then ok=1; break; fi
        done
        [[ "$ok" -eq 1 ]] || { warn "link check failed ($code): $url"; failures=$((failures+1)); }
    done <<< "$URLS"
    [[ "$failures" -eq 0 ]] || die "$failures link(s) failed verification"
    log "all $total links verified"
fi
printf '%s\n' "$OUTPUT"
