# Build and porting guide

## Driver and tests

Use Python 3, LLVM with `clang-cl`, `llvm-lib` and `lld-link`, plus the
Windows SDK and WDK versioned Include trees. The WDK must also have its
ARM64 Lib and KMDF 1.15 directories. Set `WDK_INCLUDE` and `SDK_INCLUDE` to
the versioned Include directories. The Windows 11 22H2/23H2 build uses
WDDM 2.4 private-memory callbacks.

```sh
git clone https://github.com/danfromtico/mesa-switch.git build/mesa-switch
git -C build/mesa-switch checkout --detach 0ff52079ca9e9e837ed27eb95a8d239f439a1864
python3 tools/build.py --mesa build/mesa-switch \
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
python3 tools/build_windows_icd.py --mesa build/mesa-switch \
  --out build/icd --cross-file "$WINDOWS_CROSS_FILE" \
  --cc "$ARM64_MINGW_CC" --clang-cl "$CLANG_CL" \
  --sdk-include "$SDK_INCLUDE" --wdk-include "$WDK_INCLUDE"
```

The script leaves the Mesa checkout intact and produces a matched
`libvulkan_nouveau.dll` and `nvk_windows_icd.json`. The ICD requires the
matching live adapter LUID and the KMD's `NATIVE_SUBMIT` capability.
Use `test-kmt.exe` for the controlled KMT cases and `nvk-windows-check.exe`
for live ICD enumeration.

`tools/build_offscreen_triangle.py` builds the offscreen smoke test with
Khronos Vulkan headers and the ARM64 loader and import library. Set
`VK_DRIVER_FILES` to the explicit ICD JSON on the test machine, then run
`vk-triangle.exe <absolute-NVK-DLL-path> <adapter-LUID>`. Optional
`--preview` opens a Win32 window after the verified readback. The command
validation in the current KMD covers the tested submission stream.
[Hardware status](STATUS.md)

## Switch Lite integration

Apply the [WinX GPU patch](../integration/switch-lite/winx-gpu.patch) to
WinX at the documented base. Put this GPU checkout under the WinX root
at `TegraGpu/` so ACPI and the UEFI diagnostic include the same source.
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
