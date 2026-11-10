# GPU status

**Latest physical result** on Nintendo Switch Lite HDH-001, Windows 11 ARM64
23H2 (22631.2861).

| Driver | Result | Remaining work |
| --- | --- | --- |
| 0.1.0.105 | The internal DSI monitor uses the native mode recommendation without EDID. The link capability callback succeeded on hardware, and DxgKrnl proceeded to VidPN support checks and mode enumeration. | The capture ended before a VidPN commit. A later start request in the same boot hit the one-shot test gate and left the device at Code 43. |
| 0.1.0.87 | Khronos ARM64 loader and NVK submitted a Vulkan triangle through the GM20B LUID. GPU fence 27 completed. Readback matched 300 red pixels, 724 blue pixels and 128 guards. The separate Win32 GDI preview displayed the verified pixels. | Native Win32 swapchain presentation. |
| 0.1.0.101 | `ACPI\SWOA0F01\0` appeared as **Tegra X1 (GM20B)** in the **Display** class with **OK / Code 0**. The GPU initialized, the miniport acquired the POST scanout, and the display core accepted the callbacks. | The internal adapter startup later reported `STATUS_NOT_SUPPORTED` after monitor-mode queries. The capture had no active VidPN mode commit or WDDM flip. |
| 0.1.0.59 | The Lite was restored to the accepted diagnostic driver after the one-boot .101 test. | The experimental display miniport remains an opt-in test. |

## Development order

1. Windows desktop hardware acceleration. Establish an active landscape
   VidPN, verify primary allocation and panel flips on the GM20B LUID, then
   verify DWM composition retires on GM20B.
2. Direct3D 9, 10 and 11 alongside `VK_EXT_memory_budget`,
   `VK_EXT_pageable_device_local_memory` and `VK_KHR_maintenance5`.
3. OpenGL compatibility through Mesa Zink over NVK.
4. Other Vulkan extensions.
5. Direct3D 12.

Use the [build and porting guide](PORTING.md) and
[driver contracts](DRIVER.md) for details.
