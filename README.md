# Mesa Switch for Windows ARM64

Mesa NVK and a Windows 11 WDDM driver for the Tegra X1 (GM20B), based on
the Mesa Switch port. The current hardware validation was performed on
Nintendo Switch Lite (HDH-001).

The long-term goal is a complete native Vulkan implementation, verified
with version-appropriate VK-GL-CTS coverage and physical GM20B tests.
Windows desktop GPU acceleration is the current development milestone.

| Area | Status |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview showed that readback. |
| Display adapter | **0.1.0.130** reached **Code 0**, completed GDI copies and displayed verified RGB bars on the internal panel through the GM20B VidPN and native scanout. |
| Windows desktop | The next milestone is a Direct3D UMD and DWM composition. The current UMD rejects Direct3D device creation, and continuous desktop refresh remains unresolved. |

The Windows NVK backend uses KMT for allocations, GPU virtual addresses,
submissions and monitored fences. The KMD uses dynamically allocated system
memory under VidMm with a **512 MiB** adapter ceiling.

- [Windows NVK backend](src/nouveau/vulkan/nvkmd/windows/) and its ARM64 build.
- [Tegra X1 WDDM source and board integration](tegra-x1-wddm/).
- [Build and porting guide](tegra-x1-wddm/docs/PORTING.md) and [hardware status](tegra-x1-wddm/docs/STATUS.md).
- [Native scanout evidence](tegra-x1-wddm/docs/NATIVE_STARTUP_RESULT.md) and [Direct3D/DWM work](tegra-x1-wddm/docs/PORTING.md#direct3d-umd-and-dwm).
- [RADV `wddm2` reference and compatibility notes](tegra-x1-wddm/docs/PORTING.md#radv-windows-reference) for WDDM, Win32 presentation, Direct3D and Zink.
- [Upstream and downstream](tegra-x1-wddm/UPSTREAM.md) and [license map](tegra-x1-wddm/LICENSE.md).
- [Horizon OS documentation from mesa-switch](README.rst).

The Windows NVK code retains Mesa's MIT license. Each source file retains
its existing license and notices. [Mesa license policy](https://docs.mesa3d.org/license.html)
