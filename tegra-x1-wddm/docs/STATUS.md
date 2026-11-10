# GPU status

**Latest physical result** on Nintendo Switch Lite HDH-001, Windows 11 ARM64
23H2 (22631.2861).

| Driver | Result | Remaining work |
| --- | --- | --- |
| 0.1.0.87 | Khronos ARM64 loader and NVK submitted a Vulkan triangle through the GM20B LUID. GPU fence 27 completed. Readback matched 300 red pixels, 724 blue pixels and 128 guards. The separate Win32 GDI preview displayed the verified pixels. | Native Win32 swapchain presentation. |
| 0.1.0.101 | `ACPI\SWOA0F01\0` appeared as **Tegra X1 (GM20B)** in the **Display** class with **OK / Code 0**. The GPU initialized, the miniport acquired the POST scanout, and the display core accepted the callbacks. | The internal adapter startup later reported `STATUS_NOT_SUPPORTED` after monitor-mode queries. The capture had no active VidPN mode commit or WDDM flip. |
| 0.1.0.59 | The Lite was restored to the accepted diagnostic driver after the one-boot .101 test. | The experimental display miniport remains an opt-in test. |

Next, establish the active internal VidPN with landscape orientation and
confirm primary allocation and physical flip on the GM20B LUID. Then bring
up the Direct3D user-mode path and verify DWM work retires on GM20B. For
OpenGL compatibility, the intended userspace path is Mesa Zink over NVK.

Use the [build and porting guide](PORTING.md) and
[driver contracts](DRIVER.md) for details.
