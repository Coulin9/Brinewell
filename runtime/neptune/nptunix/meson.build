# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT

# nptunix.{so,dll} -- Wine unixlib bridge for the mesa Wine PE DLLs.
#
# nptunix.so is the unix side: a plain Linux ELF (xcb DRI3/Present, AF_UNIX
# sockets, SCM_RIGHTS, memfd) loaded by the PE d3d11.dll inside Wine via the
# nptunix.dll helper-builtin (mmdevapi pattern).  Sources are npt_unix.c +
# npt_unix_wsi.c + npt_unix_virtgpu.c + npt_display_drm.c.
# Built in BOTH builddirs because the native test apps and the Wine PE
# pipeline both need it -- in the Wine PE cross build we still want a Linux
# ELF, so we ask meson to use the build-machine compiler via `native : true`.
#
# nptunix.dll is the PE carrier: a tiny stub whose only purpose is to give
# Wine a builtin DLL whose unixlib handle (NtQueryVirtualMemory(
# MemoryWineUnixFuncs)) the PE d3d11.dll can grab via LoadLibraryA().
# Source is nptunix_pe.c.  Wine recognizes builtins by the literal
# "Wine builtin DLL" string at offset 0x40 of the DOS stub, which neither
# winegcc nor MinGW writes there -- we have to patch it post-link.  The
# patching is 32 bytes and is done by a one-shot inline python invocation.
# Only built in the Wine PE cross build because it requires a PE compiler.
#
# DRI3 1.4 / Present 1.4 wire helpers live in npt_wsi_protocol.h, so
# nptunix.so builds against libxcb 1.15+.
# A macOS cross build can produce the PE DLLs; the Linux unixlib is
# supplied by the guest payload and must be built on Linux.
if build_machine.system() == 'linux'
  npt_dep_xcb = dependency('xcb', native : true)
  npt_dep_xcb_dri3 = dependency('xcb-dri3', native : true)
  npt_dep_xcb_present = dependency('xcb-present', native : true)

  # Bake the xcb-dri3 libdir as an rpath so nptunix.so resolves the
  # pkg-config-resolved libxcb-dri3.so.0 at runtime even when loaded inside
  # Wine, where LD_LIBRARY_PATH may not be propagated.  When the system
  # libxcb is being used this is just /usr/lib/<triple> and is harmless;
  # when a custom prefix is in play this is what makes the loader find it.
  npt_xcb_dri3_libdir = npt_dep_xcb_dri3.get_variable(pkgconfig : 'libdir')

  if with_tests
    test('brinewell-frame-stats',
      executable('brinewell-frame-stats-test', 'tests/brinewell_frame_stats_test.c',
        native : true, c_args : ['-UNDEBUG']),
      suite : ['neptune'])
    test('brinewell-x11-focus',
      executable('brinewell-x11-focus-test', 'tests/brinewell_x11_focus_test.c',
        native : true, dependencies : [npt_dep_xcb], c_args : ['-UNDEBUG']),
      suite : ['neptune'])
  endif
  npt_nptunix_deps = [
    npt_dep_xcb, npt_dep_xcb_dri3, npt_dep_xcb_present,
    dependency('libdrm', native : true),
  ]

  libnptunix = shared_library(
    'nptunix',
    files('npt_unix.c', 'npt_unix_wsi.c', 'npt_unix_virtgpu.c',
          'npt_display_drm.c'),
    name_prefix : '',
    native : true,
    include_directories : inc_include,
    dependencies : npt_nptunix_deps,
    build_rpath : npt_xcb_dri3_libdir,
    gnu_symbol_visibility : 'default',
    # `install : true` is incompatible with `native : true` in a cross build
    # (meson refuses to install a build-machine binary into the cross prefix)
    # and the run scripts copy from the build dir directly anyway.
    install : false,
  )
endif

if npt_is_windows
  # ----- nptunix.dll (PE carrier, marker-patched) -----
  #
  # The shared_library is named "nptunix_unpatched" so its output filename
  # ("nptunix_unpatched.dll") doesn't collide with the patched custom_target
  # output ("nptunix.dll").  But that name is also embedded in the PE
  # export directory's Name field, which Wine reads to locate the matching
  # unixlib .so (it looks for "<embedded_name_without_.dll>.so" in
  # WINEDLLPATH/x86_64-unix/).  So the patch step must:
  #   (1) write the "Wine builtin DLL" marker into the DOS stub at 0x40
  #   (2) rewrite the embedded export name from "nptunix_unpatched.dll"
  #       to "nptunix.dll", NUL-padded to preserve the original byte length
  #       so PE offsets stay valid.  The export Name field is
  #       NUL-terminated, so the trailing NUL padding is harmless.
  nptunix_dll_unpatched = shared_library(
    'nptunix_unpatched',
    files('nptunix_pe.c'),
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : false,
  )
  nptunix_dll = custom_target(
    'nptunix.dll',
    input  : nptunix_dll_unpatched,
    output : 'nptunix.dll',
    command : [
      find_program('python3'), '-c',
      'import sys; d = open(sys.argv[1], "rb").read(); ' +
      'old = b"nptunix_unpatched.dll" + bytes(1); ' +
      'new = b"nptunix.dll" + bytes(len(old) - len(b"nptunix.dll")); ' +
      'assert d.count(old) >= 1, "embedded module name not found"; ' +
      'd = d.replace(old, new); ' +
      'open(sys.argv[2], "wb").write(' +
        'd[:0x40] + b"Wine builtin DLL" + bytes(16) + d[0x60:])',
      '@INPUT@', '@OUTPUT@',
    ],
    build_by_default : true,
    install : true,
    install_dir : get_option('libdir'),
  )
endif
