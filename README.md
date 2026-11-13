# Mesa Switch for Windows ARM64

Mesa NVK and a Windows 11 WDDM driver for the Tegra X1 (GM20B), based on
the Mesa Switch port. The current hardware validation was performed on
Nintendo Switch Lite (HDH-001).

The long-term goal is a complete native Vulkan implementation, verified
with version-appropriate VK-GL-CTS coverage and physical GM20B tests.
Windows desktop GPU acceleration is the current development milestone.
The existing D3D UMD → Zink → NVK desktop path is being repaired in parallel
with **tgd3d**, an experimental native D3D DDI → Vulkan backend preserved on
the [claude-vulkan branch](https://github.com/ruje0504-2/mesa-switch-woa/tree/claude-vulkan)
for Claude to continue. The DX9/10/11 application path uses DXVK over the same
NVK/KMD foundation; it has its own compatibility work and validation.
Zink also remains the OpenGL compatibility route.

| Area | Status |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview showed that readback. |
| Display adapter | **0.1.0.130** reached **Code 0**, completed GDI copies and displayed verified RGB bars on the internal panel through the GM20B VidPN and native scanout. |
| Shared storage | **0.1.0.144** passes Vulkan Win32 buffer/image sharing, GPU read/write after producer-device release, and re-export: 32,768 payload words plus 512 guards match. [Scope and evidence](tegra-x1-wddm/docs/VULKAN_WIN32_MEMORY.md). |
| Windows desktop (Zink/NVK) | **Experimental .158 baseline.** A 90-second observation showed a complete taskbar and native FL10_0 composition, but later menu interaction showed severe visual corruption. Visual correctness, extended stability and hardware landscape remain unresolved. [Exact components and evidence](tegra-x1-wddm/docs/NATIVE_DESKTOP_158.md) · [Current status](tegra-x1-wddm/docs/STATUS.md). |
| Latest native trial | **.158 with updated UMD/ICD failed onscreen acceptance.** The owner reports no successful native output; no native pixel/desktop result was retained. The device is restored to **.59 / Code 0**, with trial tasks and registration removed and unchanged power settings. [Retest and recovery record](tegra-x1-wddm/docs/NATIVE_DESKTOP_158.md#matched-component-retest-failed-onscreen-acceptance). |
| Native Vulkan desktop backend (`tgd3d`) | **In development; build failing; no hardware validation.** A separate D3D10 WDDM UMD maps DDI calls directly to Vulkan/NVK, bypassing Gallium/Zink. Its source covers resources/views, DXBC → SPIR-V shaders, drawing, synchronization and DXGI callbacks. The recovered implementation and unfinished tests are preserved for Claude to complete; this is not the application-side DXVK path. [Branch and handoff](https://github.com/ruje0504-2/mesa-switch-woa/blob/claude-vulkan/src/nouveau/tgd3d/README.md). |

Package versions above identify KMD test packages. The frozen desktop baseline
pairs signed **.158** with UMD **77dea046dde0** and NVK ICD **6e2fad99d742**.
Its selected `.153`-based source includes compiler, queued-state and GR
helper-load corrections. The device is restored to **.59** with a complete
software-rendered taskbar in the inspected screenshot. Later `.158` interaction
showed visual corruption; cube acceptance remains incomplete. Hardware rotation has source and model
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
