#!/bin/bash
# Build the x86-64 and ARM64 Wine drivers and unixlibs from the same Mesa tree.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=neptune
SRC="$ROOT/../virtio-win-mesa"
OUTDIR="$OUT/$NAME"
DOCKERFILE="$ROOT/scripts/containers/neptune.Dockerfile"
git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1 || die "missing virtio-win-mesa checkout"
SOURCE_INPUT="$(git_state_digest "$SRC")"
SOURCE_COMMIT="$(git -C "$SRC" rev-parse HEAD)"
read -r TOOLCHAIN_URL TOOLCHAIN_SHA < <(python3 - "$ROOT/deps.lock.json" <<'PY'
import json, sys
tool = json.load(open(sys.argv[1]))['neptune_toolchain']
print(tool['url'], tool['sha256'])
PY
)
NGX_CERT="$SRC/src/virtio/neptune/ngx/signing/brinewell-ngx-v1.pem"
NGX_SIGNING_DIR="${BRINEWELL_NGX_SIGNING_DIR:-$HOME/.config/brinewell-signing/ngx-v1}"
NGX_SIGN_RECIPE=1
NGX_CERT_SHA256="$(openssl x509 -in "$NGX_CERT" -outform DER | shasum -a 256 | awk '{print $1}')"
NGX_SIGNER_VERSION=none
NGX_SIGN_MODE=unsigned
if [[ -r "$NGX_SIGNING_DIR/private.pem" && -r "$NGX_SIGNING_DIR/public.pem" ]]; then
    NGX_SIGN_MODE=signed
else
    warn "no ngx signing key in $NGX_SIGNING_DIR; nvngx.dll will be unsigned"
fi
if [[ "$NGX_SIGN_MODE" == signed ]]; then
    require_cmd openssl osslsigncode
    [[ "$(openssl x509 -in "$NGX_SIGNING_DIR/public.pem" -outform DER | shasum -a 256 | awk '{print $1}')" == "$NGX_CERT_SHA256" ]] \
        || die "$NGX_SIGNING_DIR/public.pem does not match $NGX_CERT"
    key_pub="$(openssl pkey -in "$NGX_SIGNING_DIR/private.pem" -pubout 2>/dev/null)" \
        || die "cannot read ngx signing key $NGX_SIGNING_DIR/private.pem"
    [[ "$key_pub" == "$(openssl x509 -in "$NGX_CERT" -pubkey -noout)" ]] \
        || die "$NGX_SIGNING_DIR/private.pem does not match $NGX_CERT"
    key_perm="$(stat -f %Lp "$NGX_SIGNING_DIR/private.pem")"
    [[ "$key_perm" == 400 || "$key_perm" == 600 ]] \
        || die "$NGX_SIGNING_DIR/private.pem has insecure permissions $key_perm (want 600)"
    NGX_SIGNER_VERSION="$(osslsigncode --version | head -1)"
fi
INPUT="$(input_digest "$NAME" source "$SOURCE_INPUT" \
    recipe "$(sha256_file "$0")" builder "$(sha256_file "$DOCKERFILE")" \
    toolchain "$TOOLCHAIN_SHA" target x86_64-and-arm64-wine \
    ngx_sign_mode "$NGX_SIGN_MODE" ngx_cert "$NGX_CERT_SHA256" \
    ngx_sign_recipe "$NGX_SIGN_RECIPE" ngx_signer "$NGX_SIGNER_VERSION")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/root.tar"; then exit 0; fi

require_builder
ARCHIVE="$CACHE/${TOOLCHAIN_URL##*/}"
fetch_verified "$TOOLCHAIN_URL" "$ARCHIVE" "$TOOLCHAIN_SHA"
TOOLCHAIN="$BUILD_ROOT/toolchain/llvm-mingw-$TOOLCHAIN_SHA"
if [[ ! -x "$TOOLCHAIN/bin/x86_64-w64-mingw32-clang" ]]; then
    mkdir -p "$TOOLCHAIN"
    tar -xf "$ARCHIVE" -C "$TOOLCHAIN" --strip-components=1
fi
IMAGE="brinewell-neptune-builder:$(sha256_file "$DOCKERFILE" | cut -c1-12)"
if ! builder image exists "$IMAGE"; then
    builder build --tag "$IMAGE" -f "$DOCKERFILE" "$ROOT/scripts/containers"
fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/src" "$OUTDIR/root/usr/lib/brinewell/neptune"
rsync -a --exclude .git "$SRC/" "$OUTDIR/src/"
builder run --rm -i \
    -v "$OUTDIR/src:/src:ro" -v "$TOOLCHAIN:/toolchain:ro" \
    -v "$OUTDIR:/out:rw" "$IMAGE" bash -s <<'BUILD'
set -euo pipefail
export PATH="/toolchain/bin:$PATH"
# Stage the build on the guest-local disk: virtio-fs timestamp conversion can
# transiently report host times a few milliseconds ahead, which meson rejects
# as clock skew. /out is only used for the finished payload.
payload=/out/root/usr/lib/brinewell/neptune
build_wine() {
    local pe_target="$1" arch="$2" unix_cc="$3" unix_target="$4"
    local triple="$pe_target-w64-mingw32"
    local build="/tmp/build-$pe_target"
    cat > /tmp/windows.cross <<CROSS
[binaries]
c = '/toolchain/bin/$triple-clang'
cpp = '/toolchain/bin/$triple-clang++'
ar = '/toolchain/bin/llvm-ar'
strip = '/toolchain/bin/llvm-strip'
windres = '/toolchain/bin/$triple-windres'
pkg-config = '/bin/false'
[host_machine]
system = 'windows'
cpu_family = '$arch'
cpu = '$pe_target'
endian = 'little'
[properties]
needs_exe_wrapper = true
CROSS
    meson setup "$build" /src --cross-file /tmp/windows.cross \
    --buildtype=release -Db_ndebug=true \
    -Dneptune=true -Dnpt_wine=true -Dnpt_umd=off \
    -Dplatforms=windows -Dgallium-drivers=[] -Dvulkan-drivers=[] \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dglvnd=disabled \
    -Dllvm=disabled -Dbuild-tests=false -Dvideo-codecs= -Dtools= \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dshader-cache=disabled -Dxmlconfig=disabled -Dexpat=disabled \
    -Dzstd=disabled -Dzlib=disabled -Ddisplay-info=disabled
    local -a ninja_targets=(
        src/virtio/neptune/d3d11.dll src/virtio/neptune/d3d12.dll
        src/virtio/neptune/dxgi.dll src/virtio/neptune/nptunix/nptunix.dll)
    # The opt-in NGX shims are x86-64 PE only; arm64ec targets do not exist.
    if [[ $pe_target == x86_64 ]]; then
        ninja_targets+=(src/virtio/neptune/nvngx.dll
            src/virtio/neptune/nvapi64.dll)
    fi
    ninja -C "$build" -j3 "${ninja_targets[@]}"
    mkdir -p "$payload/$arch-windows" "$payload/$arch-unix"
    cp "$build/src/virtio/neptune/"{d3d11,d3d12,dxgi}.dll \
        "$build/src/virtio/neptune/nptunix/nptunix.dll" "$payload/$arch-windows/"
    if ! llvm-readobj --coff-exports "$payload/$arch-windows/d3d12.dll" | awk '
        $1 == "Ordinal:" { ordinal = $2 }
        $1 == "Name:" && $2 == "D3D12CreateDevice" { create = (ordinal == 101) }
        $1 == "Name:" && $2 == "D3D12GetDebugInterface" { debug = (ordinal == 102) }
        END { exit !(create && debug) }
    '; then
        echo "Neptune $pe_target d3d12.dll has incorrect device or debug export ordinals" >&2
        return 1
    fi
    if [[ $pe_target == x86_64 ]]; then
        cp "$build/src/virtio/neptune/"{nvngx,nvapi64}.dll \
            "$payload/$arch-windows/"
        if ! llvm-readobj --coff-exports "$payload/$arch-windows/nvngx.dll" | awk '
            $1 == "Name:" && $2 ~ /^NVSDK_NGX_D3D12_/ { count++ }
            END { exit !(count == 16) }
        '; then
            echo "Neptune $pe_target nvngx.dll does not export the 16 NVSDK_NGX_D3D12_* symbols" >&2
            return 1
        fi
        if ! llvm-readobj --coff-exports "$payload/$arch-windows/nvapi64.dll" \
                | grep -q 'Name: nvapi_QueryInterface'; then
            echo "Neptune $pe_target nvapi64.dll does not export nvapi_QueryInterface" >&2
            return 1
        fi
        # The shims go into game prefixes: only kernel32 and the Wine-provided
        # CRT forwarders may be imported, no MinGW C/C++ runtime DLLs.
        local dll
        for dll in "$payload/$arch-windows/"{nvngx,nvapi64}.dll; do
            if llvm-readobj --coff-imports "$dll" | awk '
                $1 == "Name:" && $2 ~ /\.dll$/ {
                    ok = ($2 == "KERNEL32.dll" || $2 == "ntdll.dll" ||
                          $2 == "msvcrt.dll" || $2 ~ /^api-ms-win-crt-/)
                    if (!ok) { bad = $2; exit }
                }
                END { if (bad) { print bad > "/dev/stderr"; exit 1 } }
            '; then
                :
            else
                echo "Neptune $pe_target ${dll##*/} imports a non-system runtime DLL" >&2
                return 1
            fi
        done
    fi
    if [[ $pe_target == arm64ec ]]; then
        local dll
        for dll in "$payload/$arch-windows/"*.dll; do
            llvm-readobj --coff-load-config "$dll" | grep 'CHPEMetadataPointer: 0x[1-9A-Fa-f]' > /dev/null
        done
    fi

    local unix=/src/src/virtio/neptune/nptunix
    local unix_flags
    unix_flags=$(PKG_CONFIG_LIBDIR="/usr/lib/$unix_target/pkgconfig:/usr/share/pkgconfig" \
        pkg-config --cflags --libs xcb xcb-dri3 xcb-present libdrm)
    local -a flags
    read -r -a flags <<< "$unix_flags"
    "$unix_cc" -std=gnu11 -O2 -fPIC -shared \
        -I/src/include "$unix/npt_unix.c" "$unix/npt_unix_wsi.c" \
        "$unix/npt_unix_virtgpu.c" "$unix/npt_display_drm.c" \
        -Wl,-soname,nptunix.so -Wl,--no-undefined \
        "${flags[@]}" -pthread -o "$payload/$arch-unix/nptunix.so"
    readelf --dyn-syms --wide "$payload/$arch-unix/nptunix.so" \
        | grep 'GLOBAL.*DEFAULT.*__wine_unix_call_funcs' > /dev/null
}

build_wine x86_64 x86_64 x86_64-linux-gnu-gcc x86_64-linux-gnu
# Proton installs its ARM64EC graphics DLLs in the aarch64-windows slot.
build_wine arm64ec aarch64 gcc aarch64-linux-gnu
# WINEDLLPATH bridge: one directory per arch exposing nptunix in both PE and
# unix roles, relative so the payload stays relocatable.
for arch in x86_64 aarch64; do
    mkdir -p "$payload/bridge/$arch"
    ln -sfn "../../$arch-windows/nptunix.dll" "$payload/bridge/$arch/nptunix.dll"
    ln -sfn "../../$arch-unix/nptunix.so" "$payload/bridge/$arch/nptunix.so"
    # The bridge links resolve inside the staged root; fail the build if the
    # PE or unix target is missing or the relative depth drifted.
    test -f "$payload/bridge/$arch/nptunix.dll"
    test -f "$payload/bridge/$arch/nptunix.so"
done
BUILD

NGX_DLL="$OUTDIR/root/usr/lib/brinewell/neptune/x86_64-windows/nvngx.dll"
if [[ "$NGX_SIGN_MODE" == signed ]]; then
    NGX_TMP="$(mktemp -d "$OUTDIR/.ngx-sign.XXXXXX")"
    trap 'rm -rf "$NGX_TMP"' EXIT
    log "sign nvngx.dll"
    osslsigncode sign -certs "$NGX_CERT" -key "$NGX_SIGNING_DIR/private.pem" \
        -n "Brinewell NGX" -h sha256 -in "$NGX_DLL" -out "$NGX_TMP/nvngx.dll" \
        || die "osslsigncode sign failed"
    osslsigncode verify -CAfile "$NGX_CERT" -in "$NGX_TMP/nvngx.dll" >/dev/null \
        || die "signed nvngx.dll failed osslsigncode verify"
    osslsigncode extract-signature -in "$NGX_TMP/nvngx.dll" -out "$NGX_TMP/sig.der" \
        || die "cannot extract embedded signature from signed nvngx.dll"
    openssl pkcs7 -inform DER -in "$NGX_TMP/sig.der" -print_certs -out "$NGX_TMP/signers.pem" \
        || die "cannot decode embedded signer certificates"
    leaf_sha="$(awk '/-----BEGIN CERTIFICATE-----/{n++} n==1' "$NGX_TMP/signers.pem" \
        | openssl x509 -outform DER | shasum -a 256 | awk '{print $1}')"
    [[ "$leaf_sha" == "$NGX_CERT_SHA256" ]] \
        || die "embedded signer certificate does not match $NGX_CERT"
    xcrun llvm-objdump -p "$NGX_TMP/nvngx.dll" > "$NGX_TMP/nvngx.txt" \
        || die "cannot inspect signed nvngx.dll"
    grep -q 'file format coff-x86-64' "$NGX_TMP/nvngx.txt" \
        || die "signed nvngx.dll is not a coff-x86-64 PE"
    [[ "$(grep -c 'NVSDK_NGX_D3D12_' "$NGX_TMP/nvngx.txt")" == 16 ]] \
        || die "signed nvngx.dll no longer exports the 16 NVSDK_NGX_D3D12_* symbols"
    read -r _ _ sec_rva sec_size _ < <(grep 'Security Directory' "$NGX_TMP/nvngx.txt" || true)
    [[ -n "${sec_rva:-}" && "$sec_rva" != 0000000000000000 && "${sec_size:-}" != 00000000 ]] \
        || die "signed nvngx.dll has an empty security directory"
    mv "$NGX_TMP/nvngx.dll" "$NGX_DLL"
    rm -rf "$NGX_TMP"
    trap - EXIT
fi

python3 - "$OUTDIR/root/usr/lib/brinewell/neptune" "$SOURCE_COMMIT" "$SOURCE_INPUT" "$TOOLCHAIN_SHA" "$INPUT" "$NGX_SIGN_MODE" "$NGX_CERT_SHA256" <<'PY'
import hashlib, json, struct, sys
from pathlib import Path
root = Path(sys.argv[1])
files = {}
for arch, elf_machine in [('x86_64', 62), ('aarch64', 183)]:
    names = ['d3d11', 'd3d12', 'dxgi', 'nptunix']
    if arch == 'x86_64':
        names += ['nvngx', 'nvapi64']
    for name in names:
        relative = f'{arch}-windows/{name}.dll'
        data = (root / relative).read_bytes()
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        assert data[:2] == b'MZ' and data[pe:pe+4] == b'PE\0\0'
        assert struct.unpack_from('<H', data, pe + 4)[0] == 0x8664, relative
        if name == 'nptunix':
            assert data[0x40:0x50] == b'Wine builtin DLL', relative
            assert b'nptunix.dll\0' in data and b'nptunix_unpatched.dll\0' not in data, relative
        files[relative] = hashlib.sha256(data).hexdigest()
    relative = f'{arch}-unix/nptunix.so'
    data = (root / relative).read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', data, 18)[0] == elf_machine, relative
    files[relative] = hashlib.sha256(data).hexdigest()
manifest = {'schema': 1, 'source_commit': sys.argv[2], 'source_sha256': sys.argv[3],
            'toolchain_sha256': sys.argv[4], 'build_input': sys.argv[5],
            'ngx_signing': {'mode': sys.argv[6], 'cert_sha256': sys.argv[7]}, 'files': files}
(root / 'payload.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
PY
tar -C "$OUTDIR/root" -cf "$OUTDIR/root.tar" .
stamp_write "$NAME" "$INPUT" "$OUTDIR/root.tar"
