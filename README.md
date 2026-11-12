# Mesa Switch for Windows ARM64

Mesa NVK and a Windows 11 WDDM driver for the Tegra X1 (GM20B), based on
the Mesa Switch port. The current hardware validation was performed on
Nintendo Switch Lite (HDH-001).

The long-term goal is a complete native Vulkan implementation, verified
with version-appropriate VK-GL-CTS coverage and physical GM20B tests.
Windows desktop GPU acceleration is the current development milestone.
First light up and quickly verify desktop GPU acceleration through the
existing D3D UMD → Zink → NVK path. The DX9/10/11 game path will use DXVK
over the same NVK/KMD foundation. Desktop DXVK is deferred and requires
explicit user confirmation before development. Zink also remains the
OpenGL compatibility route.

| Area | Status |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview showed that readback. |
| Display adapter | **0.1.0.130** reached **Code 0**, completed GDI copies and displayed verified RGB bars on the internal panel through the GM20B VidPN and native scanout. |
| Shared storage | **0.1.0.144** passes Vulkan Win32 buffer/image sharing, GPU read/write after producer-device release, and re-export: 32,768 payload words plus 512 guards match. [Scope and evidence](tegra-x1-wddm/docs/VULKAN_WIN32_MEMORY.md). |
| Windows desktop | **Short native observation passed with .158.** Four inspected screenshots across 90 seconds show the complete taskbar and right-side tray; the user also reports normal physical output. DWM uses Tegra X1 at FL10_0, with 79 successful flips and matching VSync addresses. Extended stability, menu interaction and hardware landscape remain pending. [Exact components and evidence](tegra-x1-wddm/docs/NATIVE_DESKTOP_158.md). |

Package versions above identify KMD test packages. The latest trial pairs
signed **.158** with UMD **77dea046dde0** and NVK ICD **6e2fad99d742**.
Its selected `.153`-based source includes compiler, queued-state and GR
helper-load corrections. The device is restored to **.59** with a complete
software-rendered taskbar in the inspected screenshot. Later `.158` interaction
and cube attempts did not complete. Hardware rotation has source and model
coverage, but **1280x720 landscape has no physical validation**. Complete the
remaining desktop presentation paths and hardware landscape before another
Win32 `vkcube` acceptance run. The successful short observation is not a
stable-driver release or complete Vulkan implementation.
Each component has its own build manifest and source/artifact identity.
[Current component identifiers](tegra-x1-wddm/docs/STATUS.md)

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
