#!/bin/bash
# Assemble the app bundle in place and write the build manifest.
set -euo pipefail
source "$(cd "$(dirname "$0")/.." && pwd)/scripts/lib/common.sh"

[[ $# -eq 0 || ( $# -eq 1 && "$1" == --check ) ]] || die "usage: $0 [--check]"
# Verify before compiling icons or touching the installed bundle. In particular,
# an old Swift frontend must not be paired with freshly built native workers.
swift_input="$(swift_input_digest)"
localizations=(en zh-Hans)
for language in "${localizations[@]}"; do
    [[ -f "$ROOT/Resources/$language.lproj/Localizable.strings" ]] ||
        die "missing localization: $language"
done
for f in \
    "$OUT/kernel/Image" \
    "$OUT/initramfs/initramfs.cpio.gz" \
    "$OUT/layer/brinewell-layer.img" \
    "$OUT/gvproxy/gvproxy" \
    "$OUT/macmon/macmon" \
    "$OUT/macmon/LICENSE" \
    "$OUT/virglrenderer/virgl_render_server" \
    "$OUT/d3dmetal/libd3dmetal-native.dylib" \
    "$OUT/mullion/libmullion-native.dylib" \
    "$OUT/mullion/licenses/Mullion.txt" \
    "$OUT/mullion/licenses/dxmt.txt" \
    "$OUT/mullion/licenses/dxmt-mit.txt" \
    "$OUT/mullion/licenses/LLVM.txt" \
    "$OUT/libkrun/libkrun.1.dylib" \
    "$OUT/libepoxy/libepoxy.0.dylib" \
    "$OUT/virglrenderer/libvirglrenderer.1.dylib" \
    "$OUT/kosmicrisp/libvulkan_kosmickrisp.dylib" \
    "$OUT/kosmicrisp/libSPIRV-Tools.dylib" \
    "$OUT/moltenvk/libMoltenVK.dylib" \
    "$OUT/swift/Brinewell" \
    "$OUT/swift/brinewell-vm" \
    "$OUT/swift/brinewell-diagnostics" \
    "$ROOT/Resources/Brinewell.icns" \
    "$ROOT/Resources/org.brinewell.render-context.plist"; do
    [[ -f "$f" ]] || die "missing packaging input: ${f#"$ROOT"/}"
    if [[ "$f" == "$OUT/"* ]]; then
        component="${f#"$OUT"/}"
        component="${component%%/*}"
        input=""
        [[ "$component" != swift ]] || input="$swift_input"
        verify_component_artifacts "$component" "$input" "$f"
    fi
done
log "packaging inputs verified"
[[ "${1:-}" != --check ]] || exit 0

# Icon Composer assets need full Xcode even when Command Line Tools are selected.
icon_developer_dir="${DEVELOPER_DIR:-$(xcode-select -p)}"
if [[ "$icon_developer_dir" == /Library/Developer/CommandLineTools ]]; then
    icon_developer_dir=/Applications/Xcode.app/Contents/Developer
fi
if ! DEVELOPER_DIR="$icon_developer_dir" xcrun --find actool >/dev/null 2>&1; then
    die "app icon compilation requires Xcode 26 or newer (set DEVELOPER_DIR)"
fi
mkdir -p "$OUT/app-icon"
DEVELOPER_DIR="$icon_developer_dir" xcrun actool "$ROOT/Resources/Brinewell.icon" \
    --compile "$OUT/app-icon" --app-icon Brinewell \
    --platform macosx --target-device mac --minimum-deployment-target 15.0 \
    --output-partial-info-plist "$OUT/app-icon/partial-info.plist" \
    --output-format human-readable-text

rm -rf "$APP/Contents/MacOS" "$APP/Contents/Resources/Runtime" \
       "$APP/Contents/Resources/licenses" "$APP/Contents/Frameworks" \
       "$APP/Contents/Helpers" "$APP/Contents/Library/LaunchAgents"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/Runtime" \
         "$APP/Contents/Resources/licenses" "$APP/Contents/Frameworks" \
         "$APP/Contents/Helpers" "$APP/Contents/Library/LaunchAgents"

cp "$OUT/swift/Brinewell" "$OUT/swift/brinewell-vm" "$OUT/swift/brinewell-diagnostics" "$APP/Contents/MacOS/"
cp "$ROOT/Info.plist" "$APP/Contents/Info.plist"
for language in "${localizations[@]}"; do
    destination="$APP/Contents/Resources/$language.lproj"
    mkdir -p "$destination"
    cp "$ROOT/Resources/$language.lproj/Localizable.strings" "$destination/"
done
cp "$ROOT/Resources/org.brinewell.render-context.plist" "$APP/Contents/Library/LaunchAgents/"
cp "$OUT/app-icon/Assets.car" "$APP/Contents/Resources/Assets.car"
# Keep the supplied full-size ICNS for systems that do not use the layered icon.
cp "$ROOT/Resources/Brinewell.icns" "$APP/Contents/Resources/Brinewell.icns"

cp "$OUT/kernel/Image" "$APP/Contents/Resources/Runtime/Image"
cp "$OUT/initramfs/initramfs.cpio.gz" "$APP/Contents/Resources/Runtime/initramfs.cpio.gz"
cp "$OUT/layer/brinewell-layer.img" "$APP/Contents/Resources/Runtime/brinewell-layer.img"
cp "$OUT/gvproxy/gvproxy" "$APP/Contents/Resources/Runtime/gvproxy"
cp "$OUT/macmon/macmon" "$APP/Contents/Resources/Runtime/macmon"

cp "$OUT/virglrenderer/virgl_render_server" "$APP/Contents/Helpers/virgl_render_server"
for pair in \
    "$OUT/libkrun/libkrun.1.dylib:libkrun.1.dylib" \
    "$OUT/libepoxy/libepoxy.0.dylib:libepoxy.0.dylib" \
    "$OUT/virglrenderer/libvirglrenderer.1.dylib:libvirglrenderer.1.dylib" \
    "$OUT/d3dmetal/libd3dmetal-native.dylib:libd3dmetal-native.dylib" \
    "$OUT/mullion/libmullion-native.dylib:libmullion-native.dylib" \
    "$OUT/kosmicrisp/libvulkan_kosmickrisp.dylib:libvulkan_kosmickrisp.dylib" \
    "$OUT/kosmicrisp/libSPIRV-Tools.dylib:libSPIRV-Tools.dylib" \
    "$OUT/moltenvk/libMoltenVK.dylib:libMoltenVK.dylib"; do
    src="${pair%%:*}"; name="${pair##*:}"
    cp "$src" "$APP/Contents/Frameworks/$name"
    codesign --force --sign - "$APP/Contents/Frameworks/$name"
done

if [[ -d "$ROOT/runtime/licenses" ]]; then
    cp -R "$ROOT/runtime/licenses/." "$APP/Contents/Resources/licenses/"
fi
mkdir -p "$APP/Contents/Resources/licenses/mullion"
cp "$OUT/mullion/licenses/"*.txt "$APP/Contents/Resources/licenses/mullion/"
cp "$OUT/macmon/LICENSE" "$APP/Contents/Resources/licenses/macmon-MIT.txt"
if [[ -f "$ROOT/runtime/licenses/THIRD-PARTY-NOTICES.txt" ]]; then
    cp "$ROOT/runtime/licenses/THIRD-PARTY-NOTICES.txt" "$APP/Contents/Resources/licenses/THIRD-PARTY-NOTICES.txt"
else
    printf 'Brinewell bundles third-party components; see each component source for license terms.\n' \
        > "$APP/Contents/Resources/licenses/THIRD-PARTY-NOTICES.txt"
fi

chmod +x "$APP/Contents/Resources/Runtime/gvproxy"
codesign --force --sign - "$APP/Contents/Resources/Runtime/macmon"
codesign --force --sign - "$APP/Contents/Helpers/virgl_render_server"
# Single-process mode: the launcher (Brinewell, the CFBundleExecutable) runs the
# VM in-process, so IT is the game process that calls libkrun and needs the
# hypervisor entitlement. Sign the app bundle WITH the entitlements so they are
# embedded in the CFBundleExecutable (signing the binary first and then the app
# would re-sign the executable and strip the entitlements).
# Disk preparation runs in the helper so guest poweroff cannot exit the launcher.
codesign --force --sign - --entitlements "$ROOT/BrinewellVM.entitlements" "$APP/Contents/MacOS/brinewell-vm"
codesign --force --sign - --entitlements "$ROOT/BrinewellVM.entitlements" "$APP"

# Replacing Contents in place does not change the outer bundle's mtime. macOS
# can then keep the old Game Mode metadata, even with the new Info.plist present.
# Publish the completed bundle before LaunchServices or gamepolicyd inspect it.
touch "$APP"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP"

python3 - "$STAMPS" "$MANIFEST" <<'PY'
import json, os, sys
stamps, out = sys.argv[1], sys.argv[2]
components = {}
for name in sorted(os.listdir(stamps)):
    if not name.endswith('.json'):
        continue
    with open(os.path.join(stamps, name)) as f:
        data = json.load(f)
    components[data['name']] = data
with open(out, 'w') as f:
    json.dump({'components': components}, f, indent=2, sort_keys=True)
    f.write('\n')
PY
log "app bundle: ${APP#"$ROOT"/}"
