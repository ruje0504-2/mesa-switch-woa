# Build and porting guide

## Driver and tests

Run the commands from the Mesa checkout root. Use Python 3, LLVM with
`clang-cl`, `llvm-lib` and `lld-link`, plus the versioned Windows SDK and WDK
Include trees. The WDK must also have its
ARM64 Lib and KMDF 1.15 directories. Set `WDK_INCLUDE` and `SDK_INCLUDE` to
the versioned Include directories. The Windows 11 22H2/23H2 build uses
WDDM 2.4 private-memory callbacks.

```sh
python3 tegra-x1-wddm/tools/build.py \
  --wdk-include "$WDK_INCLUDE" --sdk-include "$SDK_INCLUDE" --out build/kmd
```

The base build runs host fixtures and emits `TegraGpuCore.lib`. Add
`--windows-tests` to build the native ARM64 WDK fixtures. For a hardware
package, add `--render` for the MCDM path or `--display` for the WDDM panel
path. For the KMDF diagnostic and optional PMIC bridge, add `--probe`.
`--probe --keep-fan` reuses an installed compatible bridge.

The hardware packages require `--gr-firmware "$GR_FIRMWARE"` with the
external Tegra21x `NETB_img.bin`, `fecs.bin` and `gpccs.bin`, and
`--acr-firmware "$ACR_FIRMWARE"` with the NVIDIA 32.7.6 GM20B
`nv_acr_ucode_prod.bin`, `pmu_bl.bin`, `gpmu_ucode_image.bin`,
`gpmu_ucode_desc.bin`, `pmu_sig.bin` and `fecs_sig.bin`. Supply these
locally. Generate and sign the catalog and package using the ARM64 WDK and
a signing certificate trusted by the test Windows installation. Match the
firmware, miniport, and ICD ABI when deploying. [Driver details](DRIVER.md)

## Mesa NVK ICD and triangle

Use Meson, Ninja, Mesa's build Python dependencies, LLVM-MinGW for ARM64,
Rust with `aarch64-pc-windows-gnullvm`, and bindgen. Set the Meson cross
file's host machine to Windows/aarch64, point the C, C++, and Rust linkers
at the LLVM-MinGW toolchain, and set `needs_exe_wrapper = true`.
Bindgen needs the MinGW target, its sysroot and the target clang resource
directory. Build `mesa_clc` and `vtn_bindgen2` for the build host from the
pinned Mesa tree and put them on PATH.

```sh
python3 src/nouveau/vulkan/nvkmd/windows/build_icd.py \
  --out build/icd --cross-file "$WINDOWS_CROSS_FILE" \
  --cc "$ARM64_MINGW_CC" --clang-cl "$CLANG_CL" \
  --sdk-include "$SDK_INCLUDE" --wdk-include "$WDK_INCLUDE"
```

The script builds the checked-out Mesa source and produces a matched
`libvulkan_nouveau.dll` and `nvk_windows_icd.json`. The ICD requires the
matching live adapter LUID and the KMD's `NATIVE_SUBMIT` capability.
Set `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1` to enable enumeration of the
experimental GM20B device.
Use `test-kmt.exe` for the controlled KMT cases and `nvk-windows-check.exe`
for live ICD enumeration.
`test-kmt-shared.exe` exercises the SDK-backed shared-resource lifecycle
with controlled KMT substitutes, including repeated opens, budget limits
and cleanup after residency failure.
`nvk-windows-shared-check.exe <16-digit-hex-KMT-LUID>` checks live shared
storage on the selected GM20B adapter. It maps the allocation on two KMT
devices, closes the producer and exported NT handle, then checks all 4,096
words through the importer's CPU mapping. This is a memory/lifetime test;
it does not submit GPU drawing work or exercise DWM.

`tegra-x1-wddm/tools/build_offscreen_triangle.py` builds the offscreen smoke test with
Khronos Vulkan headers and the ARM64 loader and import library. Run
`vk-triangle.exe <absolute-NVK-DLL-path> <16-digit-hex-KMT-LUID>`. The sample
loads that ICD through `VK_LUNARG_direct_driver_loading`. Optional
`--preview` opens a Win32 window after the verified readback. The command
validation in the current KMD covers the tested submission stream.
[Hardware status](STATUS.md)

## Direct3D UMD and DWM

The .130 trial established visible native scanout on GM20B. The same trial
recorded `D3D11CreateDevice` returning `DXGI_ERROR_UNSUPPORTED` for the
explicit GM20B adapter and successful WARP creation. DWM reported
`MILERR_NO_HARDWARE_DEVICE`. The installed `windows/no_d3d.c` exports
adapter entry points that reject Direct3D.
[Physical result and evidence](NATIVE_STARTUP_RESULT.md)

An experimental D3D10 UMD target now connects the existing Gallium frontend
to Zink/NVK. Its adapter entry point queries the runtime callback for the
Tegra ABI, requires the native-submission capability, and selects the exact
Vulkan device LUID. It exports no software-adapter KMT emulation. The installed
driver still uses `no_d3d.c`; the new target is not installed by its build tool.

```sh
python3 tegra-x1-wddm/tools/build_umd.py \
  --out build/umd --cross-file "$WINDOWS_CROSS_FILE" \
  --wdk-include "$WDK_INCLUDE" --sdk-include "$SDK_INCLUDE"
```

Use the LLVM-MinGW/Meson environment described above. The tool stages the
required SDK/WDK headers under the output directory and emits `TegraGpuD3D.dll`
and three controlled Windows tests. Run `test_d3d10_nvk_adapter.exe`,
`test_d3d10_nvk_device.exe` and
`test_d3d10_nvk_entry.exe <absolute-TegraGpuD3D.dll-path>` on Windows.
The tests substitute adapter callbacks or a Gallium backend and do not prove
GPU rendering or acceptance by DWM.

The explicit `nvk_d3d_check.exe <absolute-UMD.dll> <absolute-NVK-ICD.json>
<16-digit-KMT-LUID>` probe calls the real DDI frontend and Zink/NVK for a
64x64 RGBA clear/copy/readback. It is not a default Meson test. It requires a
native-submission KMD and matching active adapter identity. Elevated Windows
processes ignore `VK_DRIVER_FILES`; the physical .133 test used a temporary
`VulkanDriverName` value in that device's software key, with removal included
in rollback. Normal packaging must register its ICD through the driver package.
[Loader discovery and privilege rules](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#driver-discovery-on-windows)

The .133 test reaches native adapter/device creation and a completed native
submission, then fails clear/readback on a later 592-byte KMD rejection.
Resolve that command-validation failure and repeat clear/draw/readback,
then connect runtime allocation, shared-image metadata, synchronization and
sustained composition/presentation. [Physical DDI result](D3D_UMD_RESULT.md)
Device and resource tests must select and report the GM20B LUID.
The UMD must handle runtime-owned allocation and presentation callbacks,
resource layout metadata, ownership and completion across devices.
[Adapter DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openadapter)
[Shared-resource DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openresource)

The Windows KMT backend can create a single-allocation Tegra storage resource
with NT security sharing, export an NT handle, and open it on another device.
Import validates the allocation metadata, charges the importing device's
budget and waits for residency. Each open has a separate resource handle,
and the caller owns the NT handle. NVK shared storage allocations use this
resource path. Vulkan Win32 external-memory entry points, shared-image
layout metadata and cross-device synchronization remain to be connected.
CPU-visible shared storage uses a write-combined section and the standard
ExistingHeap allocation path. The KMD handles the standard raw-buffer
description and linear surface metadata. The .132 physical trial passes
CPU readback at three ownership stages, including after producer release
and NT-handle closure. This test uses two devices in one process on GM20B;
cross-process sharing and GPU access to the imported storage are pending.
[Physical shared-storage result](SHARED_STORAGE_RESULT.md)
[KMT resource sharing](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)

Mesa's `src/gallium/frontends/d3d10umd` supplies D3D10 DDI and shader-token
translation code. `State.h` currently disables its D3D10.1 and D3D11 branches.
The default GDI target still creates softpipe or llvmpipe screens. The NVK
target links Zink, but `Resource.cpp::OpenResource` still reports an error
and runtime-owned allocation metadata is not connected. Its Present and
SetDisplayMode callbacks return unsupported until the native bridge exists.
[Existing frontend](../../src/gallium/frontends/d3d10umd/)
[GDI target](../../src/gallium/targets/d3d10umd/)

DXVK is a compatibility and code-reuse candidate. Its D3D state translation,
shader handling and Vulkan resource management may inform this work.
It provides application-facing Direct3D and DXGI libraries. Integrating
those components into a Windows UMD would still require the DDI, runtime
callbacks and shared-resource contract above. A DXVK application rendering
successfully would establish application compatibility. Hardware DWM
acceptance requires its own device, shared-surface and continuous
composition evidence.
[DXVK upstream](https://github.com/doitsujin/dxvk)

## deko3D Maxwell reference

Consult deko3D alongside Mesa/NVK for GM20B command encoding, texture and
sampler descriptors, graphics state, synchronization and cache operations.
Relate those hardware sequences to the Windows KMD's VidMm mappings,
residency and scheduler completion contract. Reused source retains deko3D's
zlib notice and identifies local changes.
[Source](https://github.com/devkitPro/deko3d)
[License](https://github.com/devkitPro/deko3d/blob/master/LICENSE)

## RADV Windows reference

Collabora's experimental `wddm2` branch pairs a Mesa Vulkan ICD with AMD's
Windows kernel driver. Use the pinned revision to compare Windows interfaces
while keeping GM20B resource metadata and submission on the Tegra ABI.
[Pinned source](https://gitlab.freedesktop.org/lfrb/mesa/-/tree/801c9763c6043f0de8408e905a5324eea06d81d7) · [Collabora's porting notes](https://www.collabora.com/news-and-blog/news-and-events/cracking-windows-open-porting-radv-to-win32.html)

- **Mesa runtime and Win32 WSI.** NVK enables the Win32 platform,
  `VK_KHR_win32_surface` and `VK_KHR_swapchain`. Its GDI presentation path
  waits for the GPU copy fence before reading the image. Win32 WSI initializes
  DXGI and DComp for drivers with a D3D12 queue callback. Use `vkcube` to
  verify the GDI path on the Lite. The DXGI path requires a working D3D12
  device and queue on the same adapter LUID. Validate active VidPN, Tegra
  primary flips and hardware DWM composition separately.
  [NVK gate](../../src/nouveau/vulkan/nvk_physical_device.h) · [Instance extensions](../../src/nouveau/vulkan/nvk_instance.c) · [Mesa Win32 WSI](../../src/vulkan/wsi/wsi_common_win32.cpp)
  [Monitored fence](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/629e52171e03c6b7f53137236819b8f81d4eb149) · [Blit synchronization](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/98efb6c0b26d6af5230ef4bed7cbd15eda6240c8) · [Win32 WSI](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_wsi.c)
- **Adapter selection and build.** Compare DXGI and DXCore enumeration
  when matching a presentation adapter to NVK's existing KMT LUID. Keep
  the SDK-backed ARM64 bridge for Windows structure layouts. Revisit the
  branch's thunk dispatch generator if NVK needs optional KMT entry points.
  [Adapter enumeration](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/7a88cc539b31cfa94f5502b1e4592efb5f9aa08e) · [Thunk generation](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/a793e2045d9fdd43ef734f8f08f5b1315656795f)
- **KMT transport and external objects.** Compare GPU VA mapping, paging
  fences, queue waits and signals, and NT-handle resource import with the
  existing Windows NVK backend. The current local monitored fence needs
  NT-handle import and export for cross-component synchronization. Reuse
  the public KMT call sequences with WDK types. Size the runtime, resource
  and allocation private-data regions separately when opening shared
  images. Shared images need a Tegra layout and lifetime contract for the
  KMD and importers. AMD allocation blobs and command streams are specific
  to its kernel driver.
  [NT resource sizing](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_queryresourceinfofromnthandle)
  [Allocations and imports](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c) · [Queue submission](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_cs.c)
- **`VK_EXT_memory_budget`.** NVK already exposes this extension. Use
  RADV's `QueryVideoMemoryInfo` integration as a reference for live VidMm
  data. Map `Budget` and `CurrentUsage` to the GM20B heaps within the 512 MiB
  ceiling. `CurrentReservation` is a distinct WDDM value.
  [RADV query](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_winsys.c#L436) · [Windows budget fields](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info)
- **`VK_EXT_pageable_device_local_memory`.** The RADV branch provides
  examples of `MakeResident`, paging-fence waits and `Evict`. Implement and
  verify the extension on GM20B with `VK_EXT_memory_priority`, live allocation
  priority updates and VidMm paging under pressure before advertising it.
  [RADV residency](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c#L849) · [Vulkan requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_pageable_device_local_memory.html)
- **`VK_KHR_maintenance5` and later extensions.** Keep NVK's shared
  maintenance5 implementation and validate it on Windows ARM64. Revisit
  RADV's Win32 external memory and semaphore paths and sparse VA binds when
  evaluating the corresponding extensions. Gate support on the actual
  GM20B capabilities and verify with targeted Vulkan CTS cases.
  [NVK features](../../src/nouveau/vulkan/nvk_physical_device.c) · [RADV external memory](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/b286bc76ca5bddf68cd9962e98484d02b9c26b5d)

### Desktop, Direct3D and Zink

- **Native WDDM and DWM.** The RADV reference covers the user-mode Vulkan
  path over AMD's KMD. For the Tegra desktop, reuse the resource, fence and
  LUID patterns alongside the Windows DDI contracts. Mesa's `d3d10umd` is
  a software DDI example for `OpenAdapter10`, resource callbacks and
  DXGI present. GM20B needs hardware allocation, submission and present
  behind those entry points.
  [Mesa DDI example](../../src/gallium/targets/d3d10umd/README.md)
- **Direct3D 9, 10 and 11.** Check the chosen DXVK version against the
  live ICD. Its current requirements include `VK_EXT_depth_clip_enable`,
  `VK_KHR_maintenance5`, `VK_KHR_maintenance6`, `VK_KHR_load_store_op_none`,
  `VK_EXT_robustness2` and `VK_EXT_transform_feedback`. The common NVK
  source already advertises these names. Verify the features and Win32
  swapchain on GM20B for each tested process architecture.
  `VK_KHR_present_wait`, `VK_KHR_swapchain_maintenance1`
  and `VK_EXT_attachment_feedback_loop_layout` are separate compatibility
  checks. Compare the RADV WSI and synchronization paths for this work.
  [DXVK driver support](https://github.com/doitsujin/dxvk/wiki/Driver-support) · [NVK platform gate](../../src/nouveau/vulkan/nvk_physical_device.h)
- **Zink.** Run Mesa's `zink_check_requirements` against the live Windows
  NVK driver to identify the OpenGL levels it can support. Check window
  presentation separately using the same Win32 WSI work.
  [Zink profiles and checker](https://docs.mesa3d.org/drivers/zink.html)
- **Direct3D 12.** The reference branch's Dozen changes use D3D12 as a
  backend for Vulkan. For D3D12 on Vulkan, compare the target vkd3d-proton
  build against live descriptor indexing and update-after-bind limits,
  robustness2 and push descriptors. Check actual allocations within the
  512 MiB ceiling and verify resource sharing and present on Windows ARM64.
  [Dozen change](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/db23c10d7b5509a7479f168b32396bdb293c6470) · [vkd3d-proton requirements](https://github.com/HansKristian-Work/vkd3d-proton#drivers)

## Switch Lite integration

Apply the [WinX GPU patch](../integration/switch-lite/winx-gpu.patch) to
WinX at the documented base. Link this checkout's `tegra-x1-wddm/`
directory into the WinX root as `TegraGpu/` so ACPI and the UEFI
diagnostic include the same source.
Start with the HDH-001 firmware build profile and add
`-D SWITCH_LITE_GPU=1`. Enable `-D SWITCH_LITE_GPU_IRQ=1` for the GPU
nonstall interrupt route. For the WDDM panel package also enable
`-D SWITCH_LITE_GPU_POST=1`, which assigns the POST framebuffer and
display controller to GPU0. Use a single ACPI owner for POST.
[Base and lineage](../UPSTREAM.md) · [WinX Lite build](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/README.md#firmware-build)

The Lite bridge reuses SwitchFan's kernel-owned SPB connection to the
MAX77812 M1 rail. `windows/pmic.h` defines the interface. Build the bundled
bridge with `--probe` if a compatible bridge is needed. Use the
[performance checker](../integration/switch-lite/performance-check/) to query
or run the diagnostic with a matching ABI. Run its `build.sh` from an output
directory and set `MINGW_SYSROOT`, `CLANG` and `BUILD_TESTS=1` to build the
tool and existing acceptance fixtures.

For another board, start with the GM20B code under `core/` and the Windows
adapter contracts in `windows/`. Supply the board's resources, power
provider, interrupt routing, authenticated firmware and display timing
through its platform layer. The T214 assumptions are under `platform/`
and `integration/switch-lite/`. The shared adapter, allocation and submit
headers carry the KMT ABI between KMD and NVK. The Lite's 512 MiB ceiling
is enforced over VidMm allocations from system memory.

For device trials, keep a tested display package and matching ACPI available
for rollback. Validate GPU execution with fences, exact readback and guards,
and check actual panel flips separately from Device Manager status.
