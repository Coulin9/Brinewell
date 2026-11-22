#!/bin/bash
# Exercise AppKit mouse capture in a temporary foreground app, without a VM.
set -euo pipefail
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
swift build --disable-sandbox --arch arm64 --build-tests

python3 - <<'PY'
import os
import pathlib
import plistlib
import shutil
import subprocess
import tempfile


def xcrun(*args):
    return subprocess.check_output(['xcrun', *args], text=True).strip()


platform = pathlib.Path(xcrun('--sdk', 'macosx', '--show-sdk-platform-path')) / 'Developer'
bin_dir = pathlib.Path(subprocess.check_output(
    ['swift', 'build', '--disable-sandbox', '--arch', 'arm64', '--show-bin-path'], text=True).strip())
bundle = bin_dir / 'BrinewellPackageTests.xctest'
if not bundle.is_dir():
    raise SystemExit('Missing test bundle: ' + str(bundle))
log = pathlib.Path('.build/mouse-capture-host-test.log').resolve()
log.write_text('')
# xctest prints its environment on launch failure; forward only the test prerequisites.
launch_env = {key: os.environ[key] for key in ['HOME', 'PATH', 'TMPDIR', 'DEVELOPER_DIR', 'LANG', 'LC_ALL'] if key in os.environ}
with tempfile.TemporaryDirectory(prefix='brinewell-mouse-capture-test-') as directory:
    app = pathlib.Path(directory) / 'MouseCaptureTests.app'
    executable = app / 'Contents/MacOS/MouseCaptureTests'
    executable.parent.mkdir(parents=True)
    shutil.copy2(platform / 'Library/Xcode/Agents/xctest', executable)
    with (app / 'Contents/Info.plist').open('wb') as output:
        plistlib.dump({
            'CFBundleExecutable': 'MouseCaptureTests',
            'CFBundleIdentifier': 'local.brinewell.mouse-capture-tests',
            'CFBundleName': 'Mouse Capture Tests',
            'CFBundlePackageType': 'APPL',
        }, output)
    subprocess.run(['codesign', '--force', '--sign', '-', str(app)], check=True)
    subprocess.run([
        '/usr/bin/open', '--arch', 'arm64', '-n', '-W', '--stdout', str(log), '--stderr', str(log),
        '--env', 'BRINEWELL_TEST_MOUSE_CAPTURE=1',
        '--env', 'DYLD_FRAMEWORK_PATH=' + str(platform / 'Library/Frameworks') + ':' + str(platform / 'Library/PrivateFrameworks'),
        '--env', 'DYLD_LIBRARY_PATH=' + str(platform / 'usr/lib'),
        str(app), '--args', '-XCTest',
        'BrinewellVMTests.MouseCaptureTests/testCaptureRelativeEventsAndReleaseLifecycle',
        str(bundle),
    ], check=True, env=launch_env)
    result = log.read_text()
    print(result, end='')
    # open -W does not propagate the test process's exit code.
    if "testCaptureRelativeEventsAndReleaseLifecycle]' passed" not in result or 'Executed 1 test, with 0 failures' not in result:
        raise SystemExit(1)
PY
