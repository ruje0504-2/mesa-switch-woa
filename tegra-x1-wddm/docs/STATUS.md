# GPU status

**Latest physical result** on Nintendo Switch Lite HDH-001, Windows 11 ARM64
23H2 (22631.2861).

| Driver | Result | Remaining work |
| --- | --- | --- |
| 0.1.0.105 / .106 | The DSI link callback succeeded on hardware. The .106 trace recorded the first start and five successful VidPN enumeration calls. | A later driver load in the same boot lost its test permit. The interactive probe saw Code 43 and the fallback display path. |
| 0.1.0.87 | Khronos ARM64 loader and NVK submitted a Vulkan triangle through the GM20B LUID. GPU fence 27 completed. Readback matched 300 red pixels, 724 blue pixels and 128 guards. The separate Win32 GDI preview displayed the verified pixels. | Native Win32 swapchain presentation. |
| 0.1.0.101 | `ACPI\SWOA0F01\0` appeared as **Tegra X1 (GM20B)** in the **Display** class with **OK / Code 0**. The GPU initialized, the miniport acquired the POST scanout, and the display core accepted the callbacks. | The internal adapter startup later reported `STATUS_NOT_SUPPORTED` after monitor-mode queries. The capture had no active VidPN mode commit or WDDM flip. |
| 0.1.0.59 | The Lite was restored to the accepted diagnostic driver with Code 0 after the .106 test. | The experimental display miniport remains an opt-in test. |

## Development order

1. Windows desktop hardware acceleration. Once the Tegra X1 display adapter
   starts with Code 0, run ARM64 Khronos `vkcube` on GM20B through the Win32
   surface and swapchain. Verify an active landscape VidPN, primary allocation
   and panel flips on the GM20B LUID, and DWM composition on GM20B.
2. Direct3D 9, 10 and 11 alongside `VK_EXT_memory_budget`,
   `VK_EXT_pageable_device_local_memory` and `VK_KHR_maintenance5`.
3. OpenGL compatibility through Mesa Zink over NVK.
4. Other Vulkan extensions.
5. Direct3D 12.

Use the [build and porting guide](PORTING.md), [driver contracts](DRIVER.md)
and [RADV Windows reuse notes](PORTING.md#radv-windows-reference) for details.
