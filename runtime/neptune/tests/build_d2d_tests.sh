#!/bin/bash
set -euo pipefail

src="$(cd "$(dirname "$0")" && pwd)"
out="${1:-/Users/mi/steamOS_VM_D3DMetal/dx12-lab/rdr1-diag-20261125/bin}"
T=/Users/mi/steamOS_VM_D3DMetal/Brinewell/.build/toolchain/llvm-mingw-c907dd2302a292b663add18752a55ce9d544ca0fd19cf33c086ca242d5994ea6
mkdir -p "$out"
common=(--target=x86_64-w64-windows-gnu --sysroot="$T" -resource-dir "$T/lib/clang/23"
  -stdlib=libc++ -rtlib=compiler-rt -unwindlib=libunwind --ld-path=/opt/homebrew/opt/lld/bin/ld.lld
  -std=c++17 -O2 -Wall -Wextra -static -I "$T/include/c++/v1")
set -x
/opt/homebrew/opt/llvm/bin/clang++ "${common[@]}" "$src/d2d_dc_render_target_test.cpp" \
  -ld2d1 -ldwrite -lwindowscodecs -lole32 -lgdi32 -luuid -o "$out/d2d_dc_render_target_test.exe"
/opt/homebrew/opt/llvm/bin/clang++ "${common[@]}" "$src/d3d11_d2d_prereqs.cpp" \
  -ld3d11 -ld3dcompiler -ldxgi -ldxguid -lgdi32 -luser32 -o "$out/d3d11_d2d_prereqs.exe"
/opt/homebrew/opt/llvm/bin/clang++ "${common[@]}" "$src/d3d10_core_test.cpp" \
  -ld3dcompiler -ldxgi -ldxguid -luser32 -o "$out/d3d10_core_test.exe"
/opt/homebrew/opt/llvm/bin/clang++ "${common[@]}" "$src/child_hwnd_present_test.cpp" \
  -ld3d11 -ldxgi -ldxguid -lgdi32 -luser32 -o "$out/child_hwnd_present_test.exe"
