# Tegra X1 WDDM and NVK for Windows ARM64

An experimental Windows 11 GPU driver for the Tegra X1 (GM20B).
The kernel driver and board integrations share this tree with the Mesa NVK
Windows transport. Hardware verification started with Nintendo Switch Lite. [NVK Windows backend](../src/nouveau/vulkan/nvkmd/windows/)

The long-term goal is a complete native Vulkan implementation, verified
with version-appropriate VK-GL-CTS coverage and physical GM20B tests.
The current milestone is the Direct3D UMD and hardware DWM composition.
The current desktop work replaces the Zink route with a native Vulkan hardware
backend. The replacement excludes the existing Gallium `d3d10umd` and software
rendering; DXVK is a code-reuse candidate and is also planned for DX9/10/11
applications. Native Windows UMD/DDI and DWM resource/presentation contracts
still require implementation. Zink remains a later OpenGL compatibility route.

| Area | Status |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle on the GM20B and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview displayed that readback. |
| Display adapter | **0.1.0.130** reached **Code 0**, completed GDI copies and displayed verified RGB bars through the GM20B VidPN and native scanout. |
| Shared storage | **0.1.0.144** passes Vulkan Win32 buffer/image sharing, GPU read/write after producer-device release, and re-export: 32,768 payload words plus 512 guards match. [Scope and evidence](docs/VULKAN_WIN32_MEMORY.md). |
| Windows desktop | **Incomplete.** .158 displays a complete native taskbar. Its latest 311-second observation finishes, but physical photographs show severe Quick Settings corruption, so visual stability fails. Hardware landscape and Win32 vkcube remain unverified. [Current evidence](docs/NATIVE_DESKTOP_158.md). |

The driver uses dynamically allocated shared memory with a **512 MiB**
adapter ceiling. Windows manages residency and budgets through VidMm.

- [Build and porting guide](docs/PORTING.md) for WDK, NVK, and Switch Lite integration.
- [Hardware status](docs/STATUS.md) for the current baseline and next verification.
- [Native scanout evidence](docs/NATIVE_STARTUP_RESULT.md) and [Direct3D/DWM work](docs/PORTING.md#direct3d-umd-and-dwm).
- [Driver contracts and bring-up history](docs/DRIVER.md).
- [Upstream, downstream, and provenance](UPSTREAM.md).
- [Licenses](LICENSE.md) and [copyright notices](NOTICE).
