#!/bin/bash
# Exercise in-place bundle publication with lightweight packaging fixtures.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$ROOT" <<'PY'
import os
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='brinewell-package-test-') as temp:
    root = Path(temp) / 'Brinewell'
    root.mkdir()
    # swift_input_digest also hashes $ROOT/../libkrun/include/*.h.
    libkrun_include = Path(temp) / 'libkrun/include'
    libkrun_include.mkdir(parents=True)
    libkrun_header = libkrun_include / 'libkrun.h'
    libkrun_header.write_text('// fixture libkrun header\n')
    (root / 'scripts/lib').mkdir(parents=True)
    (root / 'scripts/components').mkdir()
    shutil.copy(source / 'scripts/package.sh', root / 'scripts/package.sh')
    shutil.copy(source / 'scripts/lib/common.sh', root / 'scripts/lib/common.sh')
    shutil.copy(source / 'scripts/components/swift.sh', root / 'scripts/components/swift.sh')
    shutil.copy(source / 'Info.plist', root / 'Info.plist')
    (root / 'Sources').mkdir()
    swift_source = root / 'Sources/Fixture.swift'
    swift_source.write_text('// fixture source\n')
    (root / 'Tests').mkdir()
    (root / 'Tests/FixtureTests.swift').write_text('// fixture tests\n')
    (root / 'Package.swift').write_text('// fixture package\n')
    (root / 'Resources/Brinewell.icon').mkdir(parents=True)
    (root / 'Resources/Brinewell.icns').touch()
    shutil.copy(source / 'Resources/org.brinewell.render-context.plist',
                root / 'Resources/org.brinewell.render-context.plist')
    localizations = ('en', 'zh-Hans')
    for language in localizations:
        folder = f'Resources/{language}.lproj'
        shutil.copytree(source / folder, root / folder)
    (root / 'BrinewellVM.entitlements').touch()
    for name in [
        'kernel/Image', 'initramfs/initramfs.cpio.gz', 'layer/brinewell-layer.img',
        'gvproxy/gvproxy', 'virglrenderer/virgl_render_server',
        'macmon/macmon', 'macmon/LICENSE',
        'd3dmetal/libd3dmetal-native.dylib', 'libkrun/libkrun.1.dylib',
        'mullion/libmullion-native.dylib',
        'libepoxy/libepoxy.0.dylib', 'virglrenderer/libvirglrenderer.1.dylib',
        'kosmicrisp/libvulkan_kosmickrisp.dylib', 'kosmicrisp/libSPIRV-Tools.dylib',
        'moltenvk/libMoltenVK.dylib', 'swift/Brinewell', 'swift/brinewell-vm',
        'swift/brinewell-diagnostics',
    ]:
        path = root / '.build/out' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'fixture')
    licenses = root / '.build/out/mullion/licenses'
    licenses.mkdir()
    for name in ('Mullion.txt', 'dxmt.txt', 'dxmt-mit.txt', 'LLVM.txt'):
        (licenses / name).write_text(f'fixture license: {name}\n')
    app = root / 'build/Brinewell.app'
    (app / 'Contents/MacOS').mkdir(parents=True)
    sentinel = app / 'Contents/MacOS/old-app'
    sentinel.write_bytes(b'installed app must survive failed preflight')
    manifest = root / 'build/manifest.json'
    manifest.write_text('{"old": "manifest"}\n')
    old_mtime = 946684800
    os.utime(app, (old_mtime, old_mtime))

    # Stub only external tools, not the packaging shell logic. Avoid registering
    # a fixture application in the user's real LaunchServices database.
    tools = root / 'tools'
    tools.mkdir()
    (tools / 'codesign').write_text('''#!/bin/bash
set -euo pipefail
printf '%s\\n' "${!#}" >> "${FIXTURE_APP%/*}/signed-paths"
if [[ "${!#}" == "$FIXTURE_APP" ]]; then
    [[ "$(stat -f %m "$FIXTURE_APP")" == 946684800 ]]
    touch "$FIXTURE_APP/Contents/signed-marker"
fi
''')
    (tools / 'xcrun').write_text('''#!/bin/bash
set -euo pipefail
touch "${FIXTURE_APP%/*}/xcrun-called"
if [[ "$1" == --find ]]; then printf '/fixture/actool\\n'; exit 0; fi
while [[ $# -gt 0 ]]; do
    if [[ "$1" == --compile ]]; then
        touch "$2/Assets.car"
        break
    fi
    shift
done
''')
    (tools / 'lsregister').write_text('''#!/bin/bash
set -euo pipefail
[[ "$1" == -f && "$2" == "$FIXTURE_APP" ]]
[[ -f "$FIXTURE_APP/Contents/signed-marker" ]]
[[ "$(stat -f %m "$FIXTURE_APP")" -gt 946684800 ]]
touch "$FIXTURE_APP/Contents/registered-marker"
''')
    for tool in tools.iterdir():
        tool.chmod(0o755)
    # common.sh prepends Homebrew to PATH. For the fixture, put our tools first.
    common = root / 'scripts/lib/common.sh'
    common.write_text(common.read_text().replace(
        'export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:$PATH"',
        f'export PATH="{tools}:$PATH"'))
    package = root / 'scripts/package.sh'
    registration = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
    package.write_text(package.read_text().replace(registration, str(tools / 'lsregister')))
    env = dict(os.environ, DEVELOPER_DIR='/fixture/Xcode', FIXTURE_APP=str(app))

    # Use the build's stamp writer and Swift input calculation. Each negative
    # case changes one real input independently after the valid stamp is made.
    out = root / '.build/out'
    for component in sorted(out.iterdir()):
        subprocess.run(['bash', '-c', '''
source "$1"
name="$2"; shift 2
input=0000000000000000000000000000000000000000000000000000000000000000
if [[ "$name" == swift ]]; then input="$(swift_input_digest)"; fi
stamp_write "$name" "$input" "$@"
''', 'fixture', str(common), component.name,
            *(str(path) for path in sorted(component.rglob('*')) if path.is_file())],
            env=env, check=True, capture_output=True, text=True)

    def unchanged(label):
        assert sentinel.read_bytes() == b'installed app must survive failed preflight', label
        assert app.stat().st_mtime == old_mtime, f'{label}: changed bundle mtime'
        assert manifest.read_text() == '{"old": "manifest"}\n', f'{label}: changed manifest'
        assert not (app.parent / 'xcrun-called').exists(), f'{label}: ran icon compiler'
        assert not (app.parent / 'signed-paths').exists(), f'{label}: signed bundle'
        assert not (app / 'Contents/registered-marker').exists(), f'{label}: registered bundle'

    def rejected(label, expected, command=None):
        result = subprocess.run(command or ['bash', str(package)], env=env,
                                capture_output=True, text=True)
        assert result.returncode != 0, f'{label}: accepted invalid artifacts'
        assert expected in result.stderr, f'{label}: unexpected failure: {result.stderr}'
        unchanged(label)
        print(f'PASS: {label} rejected before changing installed bundle')

    subprocess.run(['bash', str(package), '--check'], env=env, check=True)
    unchanged('valid --check')
    print('PASS: valid --check leaves installed bundle and manifest unchanged')

    stamp = root / '.build/stamps/swift.json'
    saved_stamp = stamp.read_bytes()
    stamp.unlink()
    rejected('missing Swift stamp', 'swift')
    stamp.write_bytes(saved_stamp)
    stale = json.loads(saved_stamp)
    stale['input'] = '0' * 64
    stamp.write_text(json.dumps(stale))
    rejected('stale Swift stamp', 'swift')
    stamp.write_bytes(saved_stamp)

    swift_source.write_text('// changed frontend source\n')
    rejected('changed Swift source with old frontend', 'swift')
    swift_source.write_text('// fixture source\n')

    libkrun_header.write_text('// changed libkrun header\n')
    rejected('changed libkrun header with old frontend', 'swift')
    libkrun_header.write_text('// fixture libkrun header\n')

    for name in ('swift/Brinewell', 'swift/brinewell-vm', 'swift/brinewell-diagnostics',
                 'virglrenderer/virgl_render_server', 'virglrenderer/libvirglrenderer.1.dylib'):
        artifact = out / name
        original = artifact.read_bytes()
        artifact.write_bytes(b'replaced output')
        rejected(f'replaced {name}', name)
        if name == 'swift/brinewell-diagnostics':
            env['BRINEWELL_REUSE'] = 'all'
            rejected('Swift reuse with replaced diagnostics', name,
                     ['bash', str(root / 'scripts/components/swift.sh')])
            del env['BRINEWELL_REUSE']
        artifact.write_bytes(original)

    missing = out / 'swift/brinewell-diagnostics'
    missing.unlink()
    rejected('missing Swift output', 'swift/brinewell-diagnostics')
    missing.write_bytes(b'fixture')

    localization = root / 'Resources/zh-Hans.lproj/Localizable.strings'
    strings = localization.read_bytes()
    localization.unlink()
    rejected('missing Chinese localization', 'missing localization: zh-Hans')
    localization.write_bytes(strings)

    subprocess.run(['bash', str(package)], env=env, check=True)
    assert (app / 'Contents/registered-marker').is_file(), 'bundle was not published'
    assert app.stat().st_mtime > old_mtime, 'outer bundle mtime stayed stale'
    assert not sentinel.exists(), 'old executable remained in published bundle'
    for path in app.rglob('*'):
        name = path.name.lower()
        assert name != 'd3dmetal.framework', f'Apple framework bundled: {path}'
        assert not (path.is_file() and name.endswith('.dmg')), f'Disk image bundled: {path}'
    for name in ('MacOS/Brinewell', 'MacOS/brinewell-vm', 'MacOS/brinewell-diagnostics',
                 'Helpers/virgl_render_server', 'Frameworks/libvirglrenderer.1.dylib'):
        assert (app / 'Contents' / name).read_bytes() == b'fixture', f'incorrect packaged {name}'
    assert json.loads(manifest.read_text())['components']['swift'] == json.loads(saved_stamp)
    mullion = app / 'Contents/Frameworks/libmullion-native.dylib'
    assert mullion.read_bytes() == b'fixture', 'Mullion backend was not bundled'
    signed_paths = (app.parent / 'signed-paths').read_text().splitlines()
    assert str(mullion) in signed_paths, 'Mullion backend was not signed'
    macmon = app / 'Contents/Resources/Runtime/macmon'
    assert macmon.read_bytes() == b'fixture' and str(macmon) in signed_paths
    assert (app / 'Contents/Resources/licenses/macmon-MIT.txt').read_bytes() == b'fixture'
    bundled_licenses = app / 'Contents/Resources/licenses/mullion'
    for license_file in licenses.iterdir():
        assert (bundled_licenses / license_file.name).read_bytes() == license_file.read_bytes()
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    assert info['CFBundleDevelopmentRegion'] == 'en'
    for language in localizations:
        table = f'Resources/{language}.lproj/Localizable.strings'
        assert (app / 'Contents' / table).read_bytes() == (source / table).read_bytes()
    assert info['LSApplicationCategoryType'] == 'public.app-category.games'
    assert info['GCSupportsGameMode'] is True
    assert info['LSSupportsGameMode'] is True
    agent = plistlib.loads((app / 'Contents/Library/LaunchAgents/org.brinewell.render-context.plist').read_bytes())
    assert agent['Label'] == 'org.brinewell.render-context'
    assert agent['BundleProgram'] == 'Contents/Helpers/virgl_render_server'
    assert agent['ProgramArguments'][1:] == ['--work-context-service']
    assert agent['MachServices'] == {'org.brinewell.render-context': True}
    assert agent['ProcessType'] == 'Adaptive'
    assert not agent.get('RunAtLoad') and not agent.get('KeepAlive')
    print('PASS: signed in-place bundle gets a fresh mtime before registration; macOS Game Mode support is declared')
    print('PASS: Mullion backend is bundled and signed with its license notices')
PY
