# NVK-backed D3D UMD bring-up

2026-11-10, Windows 11 ARM64 23H2 on Switch Lite HDH-001.

**.133 opens the real NVK UMD adapter/device, but clear/readback fails.**
The direct-DDI probe reaches GM20B LUID `000000000009493e`, creates its
textures and completes a native submission. A later 592-byte submission is
rejected with `STATUS_INVALID_PARAMETER`; Zink reports `VK_ERROR_DEVICE_LOST`
and all 4,096 pixels in the first readback mismatch. The rejecting rule has
not yet been identified.

This calls the DDI frontend directly. It is not a `D3D11CreateDevice` runtime
test, DWM composition test, or a successful rendering/presentation result.

## Physical evidence

| Check | .133 observation |
| --- | --- |
| Identity | Code 0, matching KMT and active VidPN LUID `9493e`; native-submit capability set; 512 MiB ceiling |
| Driver selection | Loader selects the staged `libvulkan_nouveau.dll` and names the NVK GM20B device |
| UMD setup | OpenAdapter10_2, CreateDevice, render-target/staging allocation and render-target-view creation succeed |
| First captured native submission | Fence `0x14f`, 8,532 command bytes |
| First captured native completion | Fence `0x14f`, Reference `0x131`, completion/CPU-fence-readback/retirement mask 7 |
| First captured native rejection | Fence `0x163`, 592 command bytes, status `c000000d` |
| Clear/readback | Queue submit reports device loss; 4,096/4,096 first-pass pixels mismatch; probe exits 1 without timing out |
| Separate GDI execution | Fence 14, Reference 5, proof mask 7 |

The dedicated native records survive the normal startup journal filling.
In this diagnostic package, their `Flags` field stores the submission fence;
their last data field stores byte count or completion proof. The completion
preceding the rejection does not prove the failing clear/copy succeeded.

The exported ETL contains 137,700 records, with matching header count and
record boundaries. Its Tegra adapter report has LUID `9493e`; initial
Microsoft adapters use `4d7e` and `4db7`. The persistent native rejection is
the specific failure evidence. Missing or last-visible ETL events are not
used to identify an unrecorded internal failure.

## Loader discovery

Initial runs could not find an ICD. The loader later explicitly reports that
these Windows processes have elevated privileges, including the task started
with a Limited principal, so it ignores `VK_DRIVER_FILES`. Registering the
ICD manifest as `VulkanDriverName` in the Tegra device's software key allows
discovery and native device creation. The test records the previous absence
of that value and adds its removal to rollback. Production packaging needs
the corresponding driver-package registration.
[Khronos driver discovery and privilege rules](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#exception-for-elevated-privileges)

## Source changes and validation

The KMD private query now derives `NATIVE_SUBMIT` from its actual paging-only
mode and requires initialized adapter identity. The main miniport remains
paging-only; .133 explicitly enables application submissions in its test
entry point. The WDK capability fixture passes on the .59 baseline.

The new explicit `nvk_d3d_check.exe` creates a 64x64 RGBA render target and a
staging image, initializes readback with a different sentinel, and checks
every word after clear/copy/map. The intended three-pass test stops on the
first failure. Its result above covers only the failed first pass.

This run also exposed a UMD error-propagation problem: a mapping returned a
host pointer after the asynchronous backend had lost the device. The
frontend now checks Gallium reset status before and after mapping, reports
`D3DDDIERR_DEVICEREMOVED`, and unmaps a newly returned transfer instead of
exposing stale data. The extended controlled device fixture passes on the
Switch's .59 baseline, including removal during a successful backend map.
The ARM64 NVK UMD and GDI/softpipe target build. The native test must be
repeated with the fix; these model results are not a native readback pass.
[ResourceMap contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourcemap)

The earlier adapter, DLL-entry and device fixtures passed with commit
`035062b0`; their captures and tested hashes remain under
`/private/tmp/t214-gpu-round/d3d-umd.2huxro_6/returned/` and
`staged-hashes.json`. The frontend still negotiates D3D10 DDIs only, and its
native presentation callbacks remain unsupported.

Next, use NVK's existing `NVK_DEBUG=push` dump to capture the rejected command
stream, reproduce the precise admission
failure and correct the relevant transport/validation behavior. Then repeat
native clear/draw/readback before connecting runtime allocations, shared
textures, synchronization and sustained DWM presentation. DXVK remains a
compatibility/code-reuse reference.

## Recovery and artifacts

At 17:23:43 UTC the device is restored to .59 Code 0 with original driver and
ACPI hashes. The start permit, test tasks and temporary ICD registration are
gone. Before/after power-setting reports are byte-identical. Windows restarted
directly into Windows; firmware and Hekate were unchanged.

Artifacts are under `/private/tmp/t214-gpu-round/native-ddi133.q2b6fbs1/`.
All 39 downloaded test records and five recovery records match device SHA256
and lengths in `physical/verified.json` and `physical/verified-recovery.json`.
`app-manifest.json` identifies the exact physical-test binaries;
`umd-after-loss/` contains the subsequent mapping-error fix and fixture.

| Artifact | SHA256 |
| --- | --- |
| Signed .133 KMD | `90d402f7540d4219ddccbd6fbf975ba3d2306e1abf07c1ead448d93fd1503d43` |
| Registered-ICD test result | `37d1135f0f98b8b9e1b9a8401f24b1462b9be67a33d1e5e2a9ed9ac14a89538e` |
| Live startup journal | `a4b4b99683e6f46c671e8f88873ba7af2bd304bfe9e9ac6d9ae9fe47edd032a1` |
| ETL export | `ebec876c50251219cfd67e5536f39fb24becc0d9fecbfd820bf7c78bf1a20b5d` |
