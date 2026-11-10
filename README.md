# Tegra X1 WDDM and NVK for Windows ARM64

An experimental Windows 11 GPU driver for the Tegra X1 GM20B, developed on
Nintendo Switch Lite (HDH-001, T214). The project contains the kernel-mode
driver, the Mesa NVK Windows transport and the board integration needed to
test both on a Lite.

| Area | Verified on the Switch Lite |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle on the GM20B and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview displayed that readback. |
| Display adapter | The **0.1.0.101** WDDM test build registered **Tegra X1 (GM20B)** under Display adapters with **Code 0**. |
| Windows desktop | Active WDDM panel presentation and DWM GPU acceleration remain in development. |

The driver uses dynamically allocated shared memory with a **512 MiB**
adapter ceiling. Windows manages residency and budgets through VidMm.

- [Build and porting guide](docs/PORTING.md) for WDK, NVK, and Switch Lite integration.
- [Hardware status](docs/STATUS.md) for the current baseline and next verification.
- [Driver contracts and bring-up history](docs/DRIVER.md).
- [Upstream, downstream, and provenance](UPSTREAM.md).
- [Licenses](LICENSE.md) and [copyright notices](NOTICE).
