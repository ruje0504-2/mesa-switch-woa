# SPDX-License-Identifier: MIT
# Runs the shipped implementation with explicit Vulkan/runtime substitutes.
# This executable never creates a real Vulkan or WDDM device.
tgd3d_contracts = executable(
  'tgd3d-contracts',
  ['contracts.cpp', 'runtime_contracts.cpp', tgd3d_files],
  include_directories : tgd3d_inc + [include_directories('..')],
  cpp_args : tgd3d_args,
  dependencies : tgd3d_deps,
  link_args : tgd3d_link_args,
)
test('tgd3d-contracts', tgd3d_contracts, suite : ['tgd3d'], timeout : 60)

# Explicit native tests only: registration and hardware acceptance belong to
# the machine owner. This probe checks the actual Windows runtime and LUID.
executable(
  'tgd3d-runtime-check',
  '../../../gallium/targets/d3d10umd/tests/nvk_runtime.cpp',
  cpp_args : ['-DD3D_UMD_NAME="TegraGpuD3DVk.dll"'],
  dependencies : [cpp.find_library('d3d11'), cpp.find_library('dxgi')],
  link_args : tgd3d_link_args,
)
executable(
  'tgd3d-trace-export',
  'trace_export.cpp',
  include_directories : [include_directories('..')],
  dependencies : [cpp.find_library('advapi32')],
  link_args : tgd3d_link_args + (cc.get_argument_syntax() == 'gcc' ? ['-municode'] : []),
)
