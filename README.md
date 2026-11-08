# Tegra GPU backend

Native Windows ARM64 backend components for the GM20B in Switch Lite (HDH-001,
T214). Development and hardware evidence live in the single
[Vulkan status page](https://github.com/ruje0504-2/winx/blob/vulkan-test/docs/gpu-vulkan/STATUS.md).

The current build produces a kernel static library. It is **not** an installable
display driver or a Vulkan ICD. Do not replace the working display driver with it.

Build with LLVM and the official Windows WDK/SDK headers. `WDK_INCLUDE` and
`SDK_INCLUDE` denote their versioned Include directories; the script targets
ARM64 and the Windows 11 22H2 baseline, using WDDM 2.4 private-memory
callbacks. From the repository root:

```sh
git clone --depth 1 https://github.com/danfromtico/mesa-switch.git build/mesa-switch
git -C build/mesa-switch checkout 0ff52079ca9e9e837ed27eb95a8d239f439a1864
python3 drivers/TegraGpu/tools/build.py \
  --mesa build/mesa-switch \
  --wdk-include "$WDK_INCLUDE" --sdk-include "$SDK_INCLUDE" \
  --out build/tegra-gpu
```

`TegraGpuCore.lib` contains WDDM resource/private-memory helpers, GM20B 4 KiB
page-entry and GPFIFO encoders, the unchanged mesa-switch report encoder, and
T214 power/clock control. The fixed core target is **729.6 MHz** using C1 NA PLL
coefficients; the helper checks the rail and measures GPCCLK with the hardware
counter. OUT5 at 204 MHz is a separate power/reference clock. Private memory
handles track OS allocation lifetime; platform DMA address mapping is still
required. Host tests cover formats, ordering, counters and failure cleanup;
they do not execute GPU commands. There is no driver entry point, GPU MMU setup
or channel submission yet.

The firmware consumes `platform/t214_resources.h` through its WindowsDrivers
submodule. Build it with `-D SWITCH_LITE_GPU=1` to expose the separate ACPI
render node. Keep the existing POST display path during bring-up.
