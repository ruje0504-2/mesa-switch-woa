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
Use `test-kmt.exe` for the controlled KMT cases and `nvk-windows-check.exe`
for live ICD enumeration.

`tegra-x1-wddm/tools/build_offscreen_triangle.py` builds the offscreen smoke test with
Khronos Vulkan headers and the ARM64 loader and import library. Run
`vk-triangle.exe <absolute-NVK-DLL-path> <16-digit-hex-KMT-LUID>`. The sample
loads that ICD through `VK_LUNARG_direct_driver_loading`. Optional
`--preview` opens a Win32 window after the verified readback. The command
validation in the current KMD covers the tested submission stream.
[Hardware status](STATUS.md)

## RADV Windows reference

Collabora's experimental `wddm2` branch pairs a Mesa Vulkan ICD with AMD's
Windows kernel driver. Use the pinned revision to compare Windows interfaces
while keeping GM20B resource metadata and submission on the Tegra ABI.
[Pinned source](https://gitlab.freedesktop.org/lfrb/mesa/-/tree/801c9763c6043f0de8408e905a5324eea06d81d7) · [Collabora's porting notes](https://www.collabora.com/news-and-blog/news-and-events/cracking-windows-open-porting-radv-to-win32.html)

- **Mesa runtime and Win32 WSI.** Review the monitored-fence handle sharing,
  the platform blit semaphore and the software present fallback for reuse in
  the current Mesa tree. The branch's DXGI hardware path asks for a D3D12
  queue on the same adapter LUID. The Windows desktop milestone still uses
  the Tegra WDDM VidPN, primary flips and DWM execution as separate checks.
  [Monitored fence](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/629e52171e03c6b7f53137236819b8f81d4eb149) · [Blit synchronization](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/98efb6c0b26d6af5230ef4bed7cbd15eda6240c8) · [Win32 WSI](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_wsi.c)
- **Adapter selection and build.** Compare DXGI and DXCore enumeration
  when matching a presentation adapter to NVK's existing KMT LUID. Keep
  the SDK-backed ARM64 bridge for Windows structure layouts. Revisit the
  branch's thunk dispatch generator if NVK needs optional KMT entry points.
  [Adapter enumeration](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/7a88cc539b31cfa94f5502b1e4592efb5f9aa08e) · [Thunk generation](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/a793e2045d9fdd43ef734f8f08f5b1315656795f)
- **KMT transport and external objects.** Compare GPU VA mapping, paging
  fences, queue waits and signals, and NT-handle resource import with the
  existing Windows NVK backend. Reuse the public KMT call sequences with
  WDK types. Shared images need a Tegra layout and lifetime contract for
  the KMD and importers. AMD allocation blobs and command streams are
  specific to its kernel driver.
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
