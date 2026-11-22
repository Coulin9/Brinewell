#!/bin/bash
# Check the default latency policy through the packaged worker entry point.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "${1:-$ROOT/build/Brinewell.app}" <<'PY'
import os
from pathlib import Path
import re
import socket
import subprocess
import sys

app = Path(sys.argv[1]).resolve() / 'Contents'
worker = app / 'Helpers/virgl_render_server'
adapter = app / 'Frameworks/libd3dmetal-native.dylib'
assert worker.is_file(), f'Missing packaged worker: {worker}'
assert adapter.is_file(), f'Missing Neptune adapter: {adapter}'
env = dict(os.environ, NPT_BACKEND='d3dmetal', DMN_LOG='warn',
           NPT_D3D11_LIBRARY_PATH=str(adapter), NPT_DXGI_LIBRARY_PATH=str(adapter),
           NPT_D3D12_LIBRARY_PATH=str(adapter))

# virglrenderer.h flags used by Brinewell's render-server processes.
common_flags = 2 | (1 << 7) | (1 << 8) | (1 << 9)
neptune_flag = 1 << 15
venus_flag = 1 << 6
cases = [('x86_64', neptune_flag), ('x86_64', venus_flag), ('arm64', venus_flag)]
for arch, backend in cases:
    parent, child = socket.socketpair()
    with parent, child:
        command = ['/usr/bin/arch', '-' + arch, str(worker),
                   '--worker-context-init-flags', str(common_flags | backend),
                   '--worker-context-id', '9203',
                   '--worker-context-name', 'DefaultPolicyProbe',
                   '--worker-context-fd', str(child.fileno())]
        process = subprocess.Popen(command, env=env, pass_fds=(child.fileno(),),
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True)
        # Closing the client endpoint lets the worker exit after initialization.
        child.close()
        parent.close()
        try:
            output, _ = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
    assert process.returncode == 0, output
    if backend == neptune_flag:
        policy = re.search(r'base=0x([0-9a-f]+) throughput=0x([0-9a-f]+)', output)
        assert policy is not None, output
        assert int(policy[1], 16) == 0xff0001, output  # LATENCY_QOS_TIER_0
        assert int(policy[2], 16) == 0, output  # THROUGHPUT_QOS_TIER_UNSPECIFIED
        assert 'Neptune renderer initialized' in output, output
        name = 'Neptune'
    else:
        assert 'latency tier 0' not in output, output
        name = 'Venus'
    print(f'PASS: {arch} {name} worker startup and default latency policy')
PY
