# GPU status

**Latest physical result** on Nintendo Switch Lite HDH-001, Windows 11 ARM64
23H2 (22631.2861).

**Current work: D3D UMD/DWM.** .139 passes native direct-DDI SM4 drawing
on GM20B LUID `a2dc3`: ordinary and indexed/scissored triangles plus three
clear/copy/readback passes check 20,480 pixels with zero mismatches, exit 0
and no timeout. The frontend now routes vertex input and Draw through
Mesa CSO/u_vbuf, fixing .138's CPU dummy-buffer pointer crash in Zink.
See the [UMD evidence](D3D_UMD_RESULT.md).

Next: Windows D3D runtime integration, runtime allocations/shared images,
synchronization and sustained DWM presentation. The direct-DDI result does
not test those stages or Win32 Vulkan presentation. Latest artifacts:
`/private/tmp/t214-gpu-round/native-ddi139.mp3g_eov/`.

The .132 trial passes 16 KiB NT-handle
storage sharing between two KMT devices on GM20B, including CPU readback
after releasing the producer and closing the exported handle. Both device
budgets return to zero. The matched adapter/active VidPN LUID is `949ff`.
D3D runtime integration, shared-image rendering/synchronization and DWM
composition remain pending. DXVK remains a compatibility and code-reuse
candidate. See the [shared-storage result](SHARED_STORAGE_RESULT.md).
After .139 testing, .59 Code 0, the original ACPI/driver hashes and unchanged
AC/DC display/sleep settings were verified. The start permit, test tasks and
temporary ICD registration have been removed.

| Driver | Result | Remaining work |
| --- | --- | --- |
| 0.1.0.139 | Code 0, matching KMT/active VidPN LUID `a2dc3`, 512 MiB ceiling. Three color passes and two SM4 triangle passes check 20,480 pixels with zero mismatches; exit 0 without timeout. | Windows D3D runtime/shared resources and DWM; Win32 `vkcube` remains pending. |
| 0.1.0.138 | Three-color readback passes, then first Draw exits `c0000005`. UMD RVA `1256e4` identifies Zink writing resource metadata through a CPU dummy vertex-buffer pointer. | CSO/u_vbuf routing fix passes natively in .139. |
| 0.1.0.137 | Code 0, matching KMT/active VidPN LUID `a3dbb`, 512 MiB ceiling. Native D3D UMD clear/copy/readback passes three colors with 12,288 matching pixels; exit 0 without timeout. | Native draw/shaders, D3D runtime/shared resources and DWM; Win32 `vkcube` remains pending. |
| 0.1.0.136 | Code 0, matching LUID `97b6f`; worker recovery at fence `7d` captures MMU address `04004000`, info `1dc6`, instance `fc241`. Release logging confirms later Lock2/allocation calls return `c00002b6`. Windows subsequently restarts. | QMD mirror passes in .137; native draw and runtime/DWM integration remain. |
| 0.1.0.135 | Code 0, matching LUID `98054`; the barrier uses WFI. Native fence `2e` completes; worker recovery at fence `43` records `c000009c`, FIFO `10000000`, MMU mask `1`. Windows later restarts. | Capture the fault address/type and fix the failing mapping; clear/readback remains unverified. |
| 0.1.0.134 | Code 0, LUID `9d74b`; the 592-byte rejection is reproduced offline at SET_REFERENCE(0). Changing only that DWORD to WFI passes unchanged KMD validation. Native mapping reports `88760870` after device loss. | Barrier fix physically tested in .135; further GPU execution work remains. |
| 0.1.0.133 | Code 0, matched LUID `9493e`, native-submit flag, real NVK UMD adapter/device and texture creation. Native fence `14f` completes with Reference `131` and proof mask 7. Fence `163` is rejected with `c000000d`; all 4,096 first-pass pixels mismatch. | Rejection identified in .134. Native clear/draw/readback, DWM and Vulkan presentation remain pending. |
| 0.1.0.132 | Code 0, LUID `949ff`, successful section-backed shared storage and 4,096 matching data words before/after producer release and NT-handle closure. GDI fence 23 completed with proof mask 7. | Cross-device GPU rendering/completion, native D3D UMD/DWM, and Win32 `vkcube`. The storage probe submits no drawing work. |
| 0.1.0.130 | Code 0, GM20B LUID `8e7d4`, matching active VidPN, completed GDI copies, successful MPO3 and native scanout at `fb9c7000`. Pixel readback and the user confirm visible RGB bars on the internal panel. | DWM exits with `MILERR_NO_HARDWARE_DEVICE`; D3D11 device creation on GM20B returns `DXGI_ERROR_UNSUPPORTED`, while WARP succeeds. Continuous desktop refresh remains unresolved. See [native startup result](NATIVE_STARTUP_RESULT.md). |
| 0.1.0.121 | The timing callback returned link success with a nonzero change ID. GM20B LUID `962ec` reached 720x1280, 60 Hz mode setup and successful shared-primary allocation creation. | Windows restarted later. The normal journal filled before the first failure; the next boot was gated to Code 43. Stable native presentation remains open. See [display timing result](DISPLAY_TIMING_RESULT.md). |
| 0.1.0.120 | GDI DMA now uses aperture segment 2. The live adapter stayed Code 0 through the test, progressed to GPU-virtual paging submissions, and exposed GM20B LUID `8ec5d`. | The interactive probe found zero active display paths and the panel was black while SSH responded. Native desktop presentation remains unverified. See [GDI DMA pool investigation](GDI_DMA_POOL.md). |
| 0.1.0.119 | The live driver-caps query returned `NoCacheCoherentApertureMemory` (`0x20000000`). GDI creation succeeded with 232 DMA private bytes and 256 allocation entries. | Windows restarted to BasicDisplay. Both ETLs associate the retained mode switches with BasicDisplay. |
| 0.1.0.118 | The write-through scheduler journal retained the writer's ready marker and the successful GDI return with 256 allocation entries. | Windows restarted to BasicDisplay. The journal has no submit, preempt, recovery or reset record, and the first internal failure remains unidentified. |
| 0.1.0.117 | Tegra StartDevice returned success with one source and one child. The GDI context returned 232 bytes of DMA private data and 256 allocation entries. | Windows restarted to BasicDisplay. The callback record contains no Tegra VidPN commit. |
| 0.1.0.116 | The live `GPUVERSION` query returned 128 bytes with `Maxwell GM20B`. Tegra StartDevice succeeded and GDI again reported 256 allocation entries. | Windows restarted and BasicDisplay resumed. The GDI callback record is the last retained Tegra startup milestone. |
| 0.1.0.115 | The desktop demand start returned success with one source and one child. DxgKrnl recorded a LUID for `SWOA0F01`. The driver again reached GDI with 256 allocation entries. | Windows restarted and BasicDisplay resumed. The callback record contains no Tegra VidPN commit. |
| 0.1.0.114 | Holding the grant until consumption let the same driver start from the BasicDisplay desktop. GPU and panel initialization and the system paging root succeeded. The live GDI context returned `AllocationListSize=256`. | Windows restarted. The durable record contains no GDI root setup, Present or VidPN commit. The next start was gated and BasicDisplay resumed. |
| 0.1.0.113 | The permitted start installed the 16,384-entry system paging root in segment 1. The GDI device and virtual-addressed GDI context were created after mode enumeration. | The callback record contains no GDI root setup, Present or VidPN commit. The second start was denied by the test budget. |
| 0.1.0.112 | One permitted start initialized the panel and completed the first CPU page-table update. After five successful mode enumerations, Windows created a GDI device and virtual-addressed GDI context. | No VidPN commit or timing submission appears in the callback record. The next start was denied by the test budget and the desktop used BasicDisplay. |
| 0.1.0.110 / .111 | Both permitted starts in each trial initialized the panel and completed five mode enumerations. The .111 ETLs retained successful StartDevice results. | The callback records show no entry into the hardware capability query, POST release or VidPN submission. The next start was denied by the test budget and the desktop used BasicDisplay. |
| 0.1.0.108 / .109 | In both trials, each permitted start initialized the panel and completed five mode enumerations. The persisted counter denied the third start, and Windows reached the BasicDisplay desktop. | No completion was recorded for VidPN commit or timing submission. |
| 0.1.0.107 | The startup trace recorded four complete rounds of successful DSI link queries and VidPN enumeration. | Windows repeatedly restarted before a recorded VidPN commit. The same boot sequence value kept the test permit active across the observed restarts. |
| 0.1.0.105 / .106 | The DSI link callback succeeded on hardware. The .106 trace recorded the first start and five successful VidPN enumeration calls. | A later driver load was denied by the test gate. The interactive probe saw Code 43 and the fallback display path. |
| 0.1.0.87 | Khronos ARM64 loader and NVK submitted a Vulkan triangle through the GM20B LUID. GPU fence 27 completed. Readback matched 300 red pixels, 724 blue pixels and 128 guards. The separate Win32 GDI preview displayed the verified pixels. | Native Win32 swapchain presentation. |
| 0.1.0.101 | `ACPI\SWOA0F01\0` appeared as **Tegra X1 (GM20B)** in the **Display** class with **OK / Code 0**. The GPU initialized, the miniport acquired the POST scanout, and the display core accepted the callbacks. | The internal adapter startup later reported `STATUS_NOT_SUPPORTED` after monitor-mode queries. The capture had no active VidPN mode commit or WDDM flip. |
| 0.1.0.59 | After the .121 rollback, the Lite returned to the diagnostic driver with Code 0 and the baseline ACPI table. | Continue from .121's accepted timing and shared-primary creation, preserving first-failure evidence. |

The test decrements and flushes the start budget before GPU initialization.
The .109, .110 and .111 callback records were identical.
AutoLogger preallocation made the first two files readable in .110. The
.111 configuration used 8 KiB buffers with circular preallocation. Each
of its first two ETLs retained StartDevice and three diagnostics with
`STATUS_NOT_SUPPORTED`. These ETLs lack a final log header and the
diagnostics do not identify the callback.
The .112 and .113 admitted-boot ETLs retained GPU VA events and still had
unfinalized headers. The .112 mode-switch events were in the fallback boot.
The .114 desktop-start ETL retained the Tegra StartDevice entry in an
unfinalized trace. Its mode-switch events and those in the finalized
fallback trace belong to BasicDisplay. The restart interrupted demand-script
logging.
The .115 write-through request and desktop records survived the restart.
The .115 and .116 admitted ETLs retained only registration events from the
scheduler provider. The .116 flusher logged 14 successful flushes in the
780 ms following the PnP request. Its first sample already counted 3,091
lost events. The sampled window added zero. The durable callback records
supply the GDI results.
[AutoLogger file rotation](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-an-autologger-session)

In .117, the callback record confirms the GDI marker was enabled and
`EtwWrite` returned success. The admitted ETL header is unfinalized and its
scheduler events are limited to registration. The flusher recorded 14
successful flushes in the 779 ms following the PnP request. Both loss
counters were zero throughout the sampled window. The first internal
failure remains unidentified.
[ETW session counters](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_properties)

The .118 journal's GDI marker was about 37 ms after the final ETW flush
sample by the boot-relative counters. Mode-switch events in both traces
match BasicDisplay. The fallback trace records Tegra's denied start.

The .113 package predates the GDI allocation-list change. The first .114
demand request left the permit unused.
[WDK context requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_contextinfo)

The .115 callback record shows `STATUS_NOT_SUPPORTED` for static adapter
performance caps, GPU version and DisplayID before GDI. The .116 test
verified the GPU version response on the Lite.
[WDK GPU version](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_gpuversion)

GDI now reserves one kernel queue entry of DMA private data. The Windows
ARM64 fixture passed with controlled substitutes under .59. The .117
package is the first physical test of this size change.

The ARM64 NVK build now includes Win32 surface and swapchain support.
Mesa's GDI presentation waits for the GPU copy fence before reading the
image. On-screen `vkcube` validation is pending.

## Long-term objective

Deliver a complete native Vulkan driver for Windows 11 ARM64 on GM20B.
The .87 offscreen draw/readback and .130 native scanout passes are the
hardware baselines. Complete the Vulkan core functionality, supported
extensions, memory and synchronization contracts, and Win32 presentation,
with explicit API-version and feature reporting. Validate each claimed
version and extension with the corresponding VK-GL-CTS coverage and
physical rendering, readback and sustained-presentation tests. Track
outstanding conformance failures and distinguish local fixtures from
hardware results. The D3D UMD and DWM work below is an intermediate milestone.

Autonomously analyze the WDDM contracts and the Mesa/NVK, Microsoft driver
samples, lfrb/mesa `wddm2` and deko3D sources, and implement the required changes
in the relevant upstream and downstream code. The GPU development and
publication repository is `ruje0504-2/mesa-switch-woa`, branch `main`.
Preserve upstream attribution and licensing, and update the published
status as implementation and evidence advance.
[Canonical GPU repository](https://github.com/ruje0504-2/mesa-switch-woa/tree/main)

WinX and Windows-for-Switch-Drivers remain companion firmware and board
integration repositories. Their GPU integration references this project.
[Repository relationships](../UPSTREAM.md)

Keep the 512 MiB dynamically allocated GPU budget and the existing
AC/DC display and sleep settings. Black-screen investigation belongs to
the separate task.

Use deko3D when comparing Maxwell command encoding, descriptors, graphics
state, synchronization and cache operations. Preserve its zlib notice and
identify adaptations when reusing source.
[deko3D upstream](https://github.com/devkitPro/deko3d)
[License](https://github.com/devkitPro/deko3d/blob/master/LICENSE)

## Development order

1. Windows desktop hardware acceleration. Build the Direct3D UMD device,
   rendering, shared-resource and synchronization paths needed by DWM on
   the GM20B LUID. Verify continuous window composition and landscape
   presentation. ARM64 Khronos `vkcube` through NVK's Win32 surface and
   swapchain remains a separate pending Vulkan test.
2. Direct3D 9, 10 and 11 alongside `VK_EXT_memory_budget`,
   `VK_EXT_pageable_device_local_memory` and `VK_KHR_maintenance5`.
3. OpenGL compatibility through Mesa Zink over NVK.
4. Other Vulkan extensions.
5. Direct3D 12.

Use the [build and porting guide](PORTING.md), [driver contracts](DRIVER.md)
and [RADV Windows reuse notes](PORTING.md#radv-windows-reference) for details.
DXVK is a compatibility and code-reuse candidate for the Direct3D work.
The native UMD and DWM requirements are tracked in the
[Direct3D plan](PORTING.md#direct3d-umd-and-dwm).
