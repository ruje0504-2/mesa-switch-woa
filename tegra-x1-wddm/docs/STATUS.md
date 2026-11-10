# GPU status

**Latest physical result** on Nintendo Switch Lite HDH-001, Windows 11 ARM64
23H2 (22631.2861).

| Driver | Result | Remaining work |
| --- | --- | --- |
| 0.1.0.114 | Windows reached the BasicDisplay desktop. Both `StartDevice` calls in that boot stopped at the test gate and the disk permit remained at 1. | Aligning the boot timestamps suggests the second call followed PnP's return and grant cleanup. The GDI allocation-list fix awaits a live WDDM start. |
| 0.1.0.113 | The permitted start installed the 16,384-entry system paging root in segment 1. The GDI device and virtual-addressed GDI context were created after mode enumeration. | The callback record contains no GDI root setup, Present or VidPN commit. The second start was denied by the test budget. |
| 0.1.0.112 | One permitted start initialized the panel and completed the first CPU page-table update. After five successful mode enumerations, Windows created a GDI device and virtual-addressed GDI context. | No VidPN commit or timing submission appears in the callback record. The next start was denied by the test budget and the desktop used BasicDisplay. |
| 0.1.0.110 / .111 | Both permitted starts in each trial initialized the panel and completed five mode enumerations. The .111 ETLs retained successful StartDevice results. | The callback records show no entry into the hardware capability query, POST release or VidPN submission. The next start was denied by the test budget and the desktop used BasicDisplay. |
| 0.1.0.108 / .109 | In both trials, each permitted start initialized the panel and completed five mode enumerations. The persisted counter denied the third start, and Windows reached the BasicDisplay desktop. | No completion was recorded for VidPN commit or timing submission. |
| 0.1.0.107 | The startup trace recorded four complete rounds of successful DSI link queries and VidPN enumeration. | Windows repeatedly restarted before a recorded VidPN commit. The same boot sequence value kept the test permit active across the observed restarts. |
| 0.1.0.105 / .106 | The DSI link callback succeeded on hardware. The .106 trace recorded the first start and five successful VidPN enumeration calls. | A later driver load was denied by the test gate. The interactive probe saw Code 43 and the fallback display path. |
| 0.1.0.87 | Khronos ARM64 loader and NVK submitted a Vulkan triangle through the GM20B LUID. GPU fence 27 completed. Readback matched 300 red pixels, 724 blue pixels and 128 guards. The separate Win32 GDI preview displayed the verified pixels. | Native Win32 swapchain presentation. |
| 0.1.0.101 | `ACPI\SWOA0F01\0` appeared as **Tegra X1 (GM20B)** in the **Display** class with **OK / Code 0**. The GPU initialized, the miniport acquired the POST scanout, and the display core accepted the callbacks. | The internal adapter startup later reported `STATUS_NOT_SUPPORTED` after monitor-mode queries. The capture had no active VidPN mode commit or WDDM flip. |
| 0.1.0.59 | After the .114 test, the Lite returned to the diagnostic driver with Code 0 and the baseline ACPI table. | The experimental display miniport remains an opt-in test. |

The test decrements and flushes the start budget before GPU initialization.
The .109, .110 and .111 callback records were identical.
AutoLogger preallocation made the first two files readable in .110. The
.111 configuration used 8 KiB buffers with circular preallocation. Each
of its first two ETLs retained StartDevice and three diagnostics with
`STATUS_NOT_SUPPORTED`. These ETLs lack a final log header and the
diagnostics do not identify the callback.
The .112 and .113 admitted-boot ETLs retained GPU VA events and still had
unfinalized headers. The .112 mode-switch events were in the fallback boot.
[AutoLogger file rotation](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-an-autologger-session)

The GDI context allocation list now has 256 entries. The ARM64 fixture
verified this output on the Lite under the .59 baseline. The .113 driver
package predates the allocation-list change.
[WDK context requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_contextinfo)

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
