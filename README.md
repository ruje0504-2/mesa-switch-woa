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
| Windows desktop | **Incomplete.** .153 passes 21,758,982 shared-sampling pixel checks and teardown. Queue-admission and sampler-view compiler defects are fixed with controlled regressions, but the latest .154/UMD native trial still produces black output and restarts. Complete desktop composition, NT sharing and hardware landscape remain unverified. [Current evidence](tegra-x1-wddm/docs/D3D_RUNTIME_SHARING.md#sampler-view-native-retest). |

Package versions above identify KMD test packages. The latest trial pairs
signed **.154** with UMD **77dea046dde0** (corrected sampler-view operands) and
the unchanged NVK ICD. The device is restored to **.59**; restarting Explorer
also restores its software-rendered taskbar. Next, identify the first stalled
work/preemption/reset path and complete desktop image, hardware landscape and
Win32 `vkcube` checks. Automated desktop capture is verified on the recovery
baseline; it does not by itself prove physical scanout.
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
