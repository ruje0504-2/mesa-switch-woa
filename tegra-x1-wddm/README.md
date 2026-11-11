# Tegra X1 WDDM and NVK for Windows ARM64

An experimental Windows 11 GPU driver for the Tegra X1 (GM20B).
The kernel driver and board integrations share this tree with the Mesa NVK
Windows transport. Hardware verification started with Nintendo Switch Lite. [NVK Windows backend](../src/nouveau/vulkan/nvkmd/windows/)

The long-term goal is a complete native Vulkan implementation, verified
with version-appropriate VK-GL-CTS coverage and physical GM20B tests.
The current milestone is the Direct3D UMD and hardware DWM composition.
Use the current Zink/NVK UMD to light up and quickly verify desktop GPU
acceleration. The DX9/10/11 game path will use DXVK over NVK/KMD. Desktop
DXVK is deferred and requires explicit user confirmation before development.
Zink also remains the OpenGL compatibility route.

| Area | Status |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle on the GM20B and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview displayed that readback. |
| Display adapter | **0.1.0.130** reached **Code 0**, completed GDI copies and displayed verified RGB bars through the GM20B VidPN and native scanout. |
| Shared storage | **0.1.0.144** passes Vulkan Win32 buffer/image sharing, GPU read/write after producer-device release, and re-export: 32,768 payload words plus 512 guards match. [Scope and evidence](docs/VULKAN_WIN32_MEMORY.md). |
| Windows desktop | **.146** matches 40,960 native D3D11 FL10_0 draw/sample pixels. DWM selects Tegra and completes its fullscreen transitions; its next failure is the unimplemented UMD `Present`. Sustained hardware composition, shared-image opens and hardware landscape rotation remain pending. [Evidence](docs/D3D_RUNTIME_SHARING.md#native-identity-rotation-trial). |

The driver uses dynamically allocated shared memory with a **512 MiB**
adapter ceiling. Windows manages residency and budgets through VidMm.

- [Build and porting guide](docs/PORTING.md) for WDK, NVK, and Switch Lite integration.
- [Hardware status](docs/STATUS.md) for the current baseline and next verification.
- [Native scanout evidence](docs/NATIVE_STARTUP_RESULT.md) and [Direct3D/DWM work](docs/PORTING.md#direct3d-umd-and-dwm).
- [Driver contracts and bring-up history](docs/DRIVER.md).
- [Upstream, downstream, and provenance](UPSTREAM.md).
- [Licenses](LICENSE.md) and [copyright notices](NOTICE).
