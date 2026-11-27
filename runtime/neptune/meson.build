# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT

project(
  'neptune', ['c', 'cpp'],
  meson_version : '>= 1.5.0',
  default_options : ['c_std=c11', 'cpp_std=c++17'],
)

# Consume the utility library and headers from the official Mesa release.
# The component recipe mounts it read-only as subprojects/mesa.
mesa = subproject('mesa')
inc_include = mesa.get_variable('inc_include')
inc_src = mesa.get_variable('inc_src')
inc_virtio = mesa.get_variable('inc_virtio')
idep_mesautil = mesa.get_variable('idep_mesautil')
idep_mesautilc11 = mesa.get_variable('idep_mesautilc11')
no_override_init_args = mesa.get_variable('no_override_init_args')
add_project_arguments(mesa.get_variable('pre_args'), language : ['c', 'cpp'])
cc = meson.get_compiler('c')
dep_thread = dependency('threads')
prog_python = find_program('python3')
with_tests = get_option('build-tests')

npt_common_src = files(
  'npt_blob.c',
  'npt_com.c',
  'npt_cs.c',
  'npt_device.c',
  'npt_dispatch.c',
  'npt_env.c',
  'npt_event.c',
  'npt_host_address.c',
  'npt_ngx_bridge.c',
  'npt_profile.c',
  'npt_ring.c',
  'npt_tls.c',
  'npt_shmem_pool.c',
  'npt_resource.c',
  'npt_d3d12_heap.c',
  'npt_shared_texture.c',
  'npt_swapchain.c',
  'npt_swapchain12.c',
  'npt_swapchain_common.c',
  'npt_overrides_d3d11_buffer.c',
  'npt_overrides_d3d11_context.c',
  'npt_overrides_d3d11_device.c',
  'npt_overrides_d3d11_fence.c',
  'npt_overrides_d3d11_query.c',
  'npt_overrides_d3d11_texture.c',
  'npt_overrides_d3d11_view.c',
  'npt_overrides_d3d12_device.c',
  'npt_overrides_d3d12_fence.c',
  'npt_overrides_d3d12_list.c',
  'npt_overrides_d3d12_queue.c',
  'npt_overrides_d3d12_resource.c',
  'npt_overrides_dxgi_adapter.c',
  'npt_overrides_dxgi_factory.c',
  'npt_overrides_dxgi_factorymedia.c',
  'npt_overrides_dxgi_output.c',
  'npt_overrides_dxgi_surface.c',
  'npt_overrides_event_handles.c',
  'ngx/npt_ngx_visibility.c',
)

# Generated client-side COM sources from neptune-protocol.  One out-of-line
# .c per interface family + a global IID -> default-ctor table.  These are
# committed in-tree under neptune-protocol/ alongside the encode/decode
# headers, regenerated from the upstream `neptune-protocol/builddir`
# whenever the protocol changes.
#
# Each .c file holds the default thunk bodies, the fill_default_vtbl
# populator, and the default constructor for one interface family.  Hand-
# written wrapper files (npt_overrides_<family>.c) override individual
# slots via npt_overrides_<family>_init() functions called from
# npt_com_init().
#
# The family list is read from neptune-protocol/npt_client_families.txt
# which the generator emits alongside the .c/.h files, so a protocol
# update only requires re-running the generator and copying in the new
# files -- no hand edit to this meson.build.
npt_client_families = import('fs').read(
  'neptune-protocol/npt_client_families.txt',
).strip().split('\n')

foreach family : npt_client_families
  npt_common_src += files(
    'neptune-protocol/npt_protocol_client_' + family + '.c'
  )
endforeach
npt_common_src += files(
  'neptune-protocol/npt_protocol_client_table.c'
)

npt_incs = [
  inc_include,
  inc_src,
  inc_virtio,
]

npt_deps = [
  dep_thread,
  idep_mesautil,
]

npt_c_args = [
  no_override_init_args,
]

# The vendored protocol output intentionally carries GCC diagnostic pragmas
# around generated inline helpers.  They are inert under MSVC, which otherwise
# emits C4068 for every push/ignored/pop directive and floods native builds.
if cc.get_argument_syntax() == 'msvc'
  npt_c_args += ['/wd4068']
endif

npt_is_windows = host_machine.system() == 'windows'

# vtest + virtgpu backends.  The Windows build defaults to the Wine PE
# pair (which routes through the nptunix unixlib bridge); flip
# -Dnpt_wine=false to target native Windows.
if npt_is_windows
  npt_common_src += files(
    'npt_d3d10_device.c',
    'npt_d3d10_device_ctx.c',
    'npt_d3d10_device_create.c',
    'npt_d3d10_resource.c',
    'npt_d3d10_state.c',
    'npt_d3d11_surface_local.c',
  )
  npt_deps += [cc.find_library('gdi32')]
  if get_option('npt_wine')
    npt_common_src += files(
      'npt_renderer_vtest_wine.c',
      'npt_renderer_vtest_protocol.c',
      'npt_renderer_virtgpu_wine.c',
      'npt_renderer_wine_common.c',
      'npt_common_wine.c',
    )
    npt_c_args += ['-D__WINE__']
  else
    npt_common_src += files(
      'npt_renderer_vtest_protocol.c',
      'npt_renderer_virtgpu_win32.c',
    )
  endif
endif

if npt_is_windows and get_option('npt_wine') and cc.get_argument_syntax() == 'gcc'
  npt_swapchain_state_test = executable(
    'npt-swapchain-state-test',
    files(
      'tests/swapchain_test_support.c',
      'tests/swapchain_test_boundaries.c',
      'tests/swapchain_common_test.c',
      'tests/swapchain_d3d11_test.c',
      'tests/swapchain_d3d12_test.c',
      'tests/swapchain_state_test.c',
    ),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
    ],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [
      cc.find_library('user32'),
      cc.find_library('gdi32'),
      cc.find_library('synchronization'),
    ],
    install : false,
    build_by_default : false,
  )
  npt_descriptor_copy_test = executable(
    'npt-descriptor-copy-test',
    files('tests/descriptor_copy_test.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
    ],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [cc.find_library('synchronization')],
    install : false,
    build_by_default : false,
  )
  npt_desc_batch_test = executable(
    'npt-desc-batch-test',
    files('tests/desc_batch_test.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
    ],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [cc.find_library('synchronization')],
    install : false,
    build_by_default : false,
  )
  npt_command_list_type_test = executable(
    'npt-command-list-type-test',
    files('tests/command_list_type_test.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
    ],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [cc.find_library('synchronization')],
    install : false,
    build_by_default : false,
  )
  npt_budget_event_test = executable(
    'npt-budget-event-test',
    files('tests/budget_event_test.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
    ],
    link_args : ['-Wl,--gc-sections'],
    install : false,
    build_by_default : false,
  )
  npt_map_ring_test = executable(
    'npt-map-ring-test',
    files('tests/map_ring_test.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
    ],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [cc.find_library('synchronization')],
    install : false,
    build_by_default : false,
  )
  npt_view_desc_test = executable(
    'npt-view-desc-test',
    files('tests/view_desc_test.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
    ],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [cc.find_library('synchronization')],
    install : false,
    build_by_default : false,
  )
  npt_backbuffer_format_test = executable(
    'npt-backbuffer-format-test',
    files('tests/backbuffer_format_test.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
    ],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [cc.find_library('synchronization')],
    install : false,
    build_by_default : false,
  )
  npt_ngx_bridge_test = executable(
    'npt-ngx-bridge-test',
    files('tests/ngx_bridge_test.c', 'ngx/npt_ngx_visibility.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + [
      '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
      '-DNPT_NGX_TEST_HOOKS', '-DNPT_NGX_TEST_X64',
    ],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [cc.find_library('synchronization')],
    install : false,
    build_by_default : false,
  )
  if with_tests
    test('npt-backbuffer-format', npt_backbuffer_format_test, suite : ['neptune'])
    test('npt-ngx-bridge', npt_ngx_bridge_test, suite : ['neptune'])
    test('npt-swapchain-state', npt_swapchain_state_test, suite : ['neptune'])
    test('npt-descriptor-copy', npt_descriptor_copy_test, suite : ['neptune'])
    test('npt-desc-batch', npt_desc_batch_test, suite : ['neptune'])
    test('npt-command-list-type', npt_command_list_type_test, suite : ['neptune'])
    test('npt-budget-event', npt_budget_event_test, suite : ['neptune'])
    test('npt-map-ring', npt_map_ring_test, suite : ['neptune'])
    test('npt-view-desc', npt_view_desc_test, suite : ['neptune'])
    if build_machine.system() != 'windows'
      test('npt-pipeline-library-ring', prog_python,
        args : files('tests/test_pipeline_library_ring.py'), suite : ['neptune'])
      test('npt-present-resource-creation', prog_python,
        args : files('tests/test_present_resource_creation.py'), suite : ['neptune'])
    endif
  endif
endif

# Host-buildable variant of the NGX bridge test: it compiles the same
# translation unit for the build machine (no wine needed) so the bridge
# logic can be exercised outside the PE toolchain.
if not npt_is_windows and with_tests
  npt_ngx_bridge_test_host = executable(
    'npt-ngx-bridge-test',
    files('tests/ngx_bridge_test.c', 'ngx/npt_ngx_visibility.c'),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + ['-O1', '-UNDEBUG', '-DNPT_NGX_TEST_HOOKS',
                           '-DNPT_NGX_TEST_X64'],
    dependencies : [dep_thread],
    install : false,
    build_by_default : false,
  )
  test('npt-ngx-bridge', npt_ngx_bridge_test_host, suite : ['neptune'])

  npt_ngx_guest_api_test = executable(
    'npt-ngx-guest-api-test',
    files('tests/ngx_guest_api_test.cpp'),
    include_directories : [include_directories('.')],
    cpp_args : ['-O1', '-UNDEBUG'],
    dependencies : [dep_thread],
    install : false,
    build_by_default : false,
  )
  test('npt-ngx-guest-api', npt_ngx_guest_api_test, suite : ['neptune'])

  npt_ngx_identity_test = executable(
    'npt-ngx-identity-test',
    files(
      'tests/ngx_identity_test.c',
      'npt_overrides_dxgi_adapter.c',
      'ngx/nvapi64.c',
      'ngx/npt_ngx_visibility.c',
    ),
    include_directories : npt_incs + [include_directories('.')],
    c_args : npt_c_args + ['-O1', '-UNDEBUG', '-DNPT_NGX_TEST_X64',
                           '-DNPT_NGX_TEST_HOOKS'],
    dependencies : [dep_thread, idep_mesautilc11],
    install : false,
    build_by_default : false,
  )
  test('npt-ngx-identity', npt_ngx_identity_test, suite : ['neptune'])
endif

if not npt_is_windows or get_option('npt_wine')
  subdir('nptunix')
endif

if npt_is_windows and get_option('npt_umd') != 'off'
  subdir('triton')
endif

if npt_is_windows
  # Windows: produce d3d11.dll and dxgi.dll as native DLLs for Wine
  # (MinGW cross-compile, installed to Wine prefix system32 like dxvk).
  # The nptunix.dll helper builtin provides unixlib access.

  libd3d11_npt = shared_library(
    'd3d11',
    [npt_common_src, files('npt_entry_d3d11.c')],
    include_directories : npt_incs,
    dependencies : npt_deps,
    c_args : npt_c_args,
    vs_module_defs : 'd3d11.def',
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : true,
  )

  libd3d10core_npt = shared_library(
    'd3d10core',
    files('npt_entry_d3d10core.c'),
    link_with : [libd3d11_npt],
    c_args : npt_c_args,
    vs_module_defs : 'd3d10core.def',
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : true,
  )

  libdxgi_npt = shared_library(
    'dxgi',
    [npt_common_src, files('npt_entry_dxgi.c')],
    include_directories : npt_incs,
    dependencies : npt_deps,
    c_args : npt_c_args,
    vs_module_defs : 'dxgi.def',
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : true,
  )

  libd3d12_npt = shared_library(
    'd3d12',
    [npt_common_src, files('npt_entry_d3d12.c')],
    include_directories : npt_incs,
    dependencies : npt_deps,
    c_args : npt_c_args,
    vs_module_defs : 'd3d12.def',
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : true,
  )

  # Standalone NGX shims for the Brinewell SR payload: no npt_common_src,
  # they reach the wrappers only through the private bridge interface.
  # x86_64 PE only; -static keeps MinGW C/C++ runtime deps out of the
  # game prefixes.
  if host_machine.cpu_family() == 'x86_64'
    libnvngx_npt = shared_library(
      'nvngx',
      files('ngx/nvngx.cpp'),
      vs_module_defs : 'ngx/nvngx.def',
      name_prefix : '',
      cpp_args : ['-fno-exceptions', '-fno-rtti'],
      link_args : ['-static'],
      install : true,
    )

    libnvapi64_npt = shared_library(
      'nvapi64',
      files('ngx/nvapi64.c', 'ngx/npt_ngx_visibility.c'),
      vs_module_defs : 'ngx/nvapi64.def',
      name_prefix : '',
      link_args : ['-static'],
      install : true,
    )
  endif

  if get_option('npt_wine') and cc.get_argument_syntax() == 'gcc'
    npt_fence_sync_test = executable(
      'npt-fence-sync-test',
      files('tests/fence_sync_test.cpp'),
      cpp_args : ['-fms-extensions'],
      link_args : ['-static'],
      link_with : [libd3d11_npt, libd3d12_npt],
      dependencies : [cc.find_library('uuid')],
      install : false,
      build_by_default : false,
    )
    npt_stage_order_test = executable(
      'npt-stage-order-test',
      files('tests/stage_order_test.cpp'),
      cpp_args : ['-fms-extensions'],
      link_args : ['-static'],
      link_with : [libd3d11_npt],
      dependencies : [cc.find_library('uuid')],
      install : false,
      build_by_default : false,
    )
    if with_tests
      test('npt-fence-sync', npt_fence_sync_test, suite : ['neptune'])
      test('npt-stage-order', npt_stage_order_test, suite : ['neptune'])
    endif
  endif
endif
# Linux build only produces libnptunix above -- d3d11.dll/dxgi.dll are
# Wine PE artifacts.
