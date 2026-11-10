# NVK-backed D3D UMD bring-up

2026-11-10, Windows 11 ARM64 23H2 on Switch Lite HDH-001.

**.139 passes native D3D UMD application-shader drawing on GM20B.**
The explicit direct-DDI probe selects matching KMT/active VidPN LUID `a2dc3`,
opens the NVK UMD device, and checks every 64x64 RGBA pixel after three clear
passes and two triangle passes. It runs 19:15:28–19:15:41 UTC and exits 0
without a timeout: 20,480 checked pixels, zero mismatches.

| Pass | Expected image | Checked pixels | Mismatches |
| --- | --- | --- | --- |
| Clear red | 4,096 words `ff0000ff` | 4,096 | 0 |
| Clear green | 4,096 words `ff00ff00` | 4,096 | 0 |
| Clear blue | 4,096 words `ffff0000` | 4,096 | 0 |
| SM4 Draw | 1,152 red and 2,944 blue pixels | 4,096 | 0 |
| SM4 DrawIndexed + scissor | 736 red and 3,360 blue pixels | 4,096 | 0 |

Both triangle checks compare pixel positions, not just color counts. The
probe reuses the existing Mesa/apitrace SM4 passthrough shaders and supplies
real vertex/index buffers. Pixel centers do not lie on triangle edges;
the indexed pass applies the rectangle `[16,48)` on both axes. All mappings
have row pitch 256. The ordinary and indexed shader paths are physical
results; instanced draw routing has controlled-fixture coverage only.

The driver is Code 0 and reports a 512 MiB ceiling. Its first native fence
`3f`, 8,532 bytes, completes with Reference `21` and proof mask 7. The journal
has no recorded recovery/rejection/MMU fault; the pixel comparisons establish
the drawing result. The KMD core and ICD are unchanged from .138. The UMD
now uses CSO/u_vbuf to prevent CPU dummy vertex bindings reaching Zink.

This is an offscreen direct-DDI test. D3D11CreateDevice, runtime resource
sharing, DWM composition and Win32 Vulkan presentation remain unverified.
.137 remains the earlier native three-color clear/copy/readback baseline,
including the QMD writable-mirror fix described below.

## Barrier rejection and execution fault

.134 uses matching KMT/VidPN LUID `9d74b`. Native fence `32` completes with
Reference `14` and proof mask 7; fence `47`, 592 bytes, is rejected with
`c000000d`. The dump has 148 DWORDs. DWORD `0x92` is `0x80000014`, immediate
SET_REFERENCE(0), from MME-data invalidation. KMD reserves that register
for retirement.

Replaying this packet reproduces the rejection. Changing only DWORD `0x92`
to `0x8001001e` (WFI/SCOPE_ALL) passes unchanged KMD preparation, including
the inline QMD relocation. The command-stream regression preserves the
captured words and checks both outcomes and the relocated QMD. Windows NVK
now shares that wait emitter across barriers, query reset and indirect
commands. Non-Windows SET_REFERENCE and Hopper/Horizon special cases remain.
[NVIDIA B06F definitions](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/common/sdk/nvidia/inc/class/clb06f.h)

.135 uses matching LUID `98054`, the same KMD core and a 512 MiB ceiling.
Its 592-byte dump differs from .134 only in the barrier DWORD. Initial
native fence `2e` completes with Reference `10` and proof mask 7. First
worker recovery is fence `43`, `c000009c` (STATUS_DEVICE_DATA_ERROR), FIFO
interrupt `10000000`, MMU mask `1`, PBDMA interrupt zero. Engine reset
returns `c0000001`; adapter reset returns success. A later boot at
18:01:40 UTC gates the driver and uses the fallback display path. There is
no native-rejection record; absence alone does not prove every submission
was accepted or completed. Push dumps also include later buffers in a batch
whose execution may have stopped early.

The test runs 18:00:49–18:00:58 UTC, exits 1 without timing out, and reports
VK_ERROR_DEVICE_LOST from command-buffer finalization. Mapping returns
`88760870`, as in .134, instead of exposing stale data. Neither completes
a pixel-check pass. The first ETL export has 97,813 records with matching
count/boundaries, but its retained events end before the native test starts.
The worker journal supplies the fault evidence; the ETL tail is not used
as the failure location. The fallback export has 88,774 records, also with
matching count/boundaries.

The optional `NVK_DEBUG=log` enables Mesa Vulkan diagnostics in release
builds. .136 physically verifies it: after device removal, Lock2 and
allocation/residency return `c00002b6`; the log identifies the source lines
that propagate it into command-buffer finalization. Those calls are later
symptoms, not the original MMU fault.

.136 runs 18:14:32–18:14:41 UTC on matching LUID `97b6f`, exits 1 without
timing out, and preserves the first worker fault at fence `7d`: address
`04004000`, MMU info `1dc6`, instance `fc241` (physical `fc241000`), FIFO
`10000000`, MMU mask `1`, PBDMA zero. The early journal returned in the same
SSH response has SHA256
`f77eeddd39c61b92a9a8e1fbb8c42c292b2e15959666aef761e3c6e9ee91172a`,
matching the later device-hashed download. Windows boots again at
18:15:21 UTC and gates the test driver.
[nouveau MMU field decoding](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/fifo/gf100.c)
[Access, client and reason names](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/fifo/gk104.c)

The .137 KMD fix keeps the aligned, checked QMD in the immutable CPU snapshot,
but PCAS points into a separate GPU-writable mirror. The serialized worker
refreshes that mirror before execution; command pages, ring and USERD keep
their existing protections. Host tests and the ARM64 KMD build pass. The
controlled context fixture passes on the Switch, including the distinct
command/QMD PTE permissions. The .137 native test passes as described above.

## Application draw path: .138

.138 repeats all three clear/readback passes on matching LUID `a0a94`,
then exits with `c0000005` in its first Draw. The diagnostic repeat records
UMD fault RVA `1256e4` (`zink_set_vertex_buffers_optimal+268`), with caller
RVA `2518` in `ResolveState`. The instruction writes the resource's vertex
binding mask. D3D's unused vertex slots hold CPU-backed zero arrays, but
the frontend disabled CSO's vertex-buffer fallback and forwarded those
pointers directly to Zink as if they were GPU resources. Clear background
and shader/resource/state creation had returned success before the fault.

The frontend now enables Mesa's capability-driven `u_vbuf` handling, binds
vertex elements/buffers together, and routes all five Draw entry points
through CSO. The extended ARM64 controlled fixture rejects CPU pointers at
its hardware-backend boundary and passes ordinary, instanced, indexed and
indexed-instanced draws plus cleanup. .139 physically verifies ordinary
and indexed/scissored draws as described above.
Both the native and GDI/softpipe UMD targets build.

.138 artifacts are under `/private/tmp/t214-gpu-round/native-ddi138.wts1cb4o/`:
31 test records and seven recovery/model records match device SHA256/lengths.
`diag-result-summary.json`, `diag-push.txt`, `umd-symbols.txt` and
`vbuf-model-result.json` identify the failure and subsequent controlled test.
The device was restored to .59 Code 0 at 19:04:23 UTC with original driver/
ACPI hashes, removed test tasks/permit/ICD registration and unchanged power
settings. .139 artifacts: `/private/tmp/t214-gpu-round/native-ddi139.mp3g_eov/`.

## Earlier physical evidence: .133

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

The original .133 `nvk_d3d_check.exe` creates a 64x64 RGBA render target and a
staging image, initializes readback with a different sentinel, and checks
every word after clear/copy/map. It stops on the first failure; .133 covers
only that failed first pass. .139 extends this probe with the two passing
shader draws reported at the top of this document.

This run also exposed a UMD error-propagation problem: a mapping returned a
host pointer after the asynchronous backend had lost the device. The
frontend now checks Gallium reset status before and after mapping, reports
`D3DDDIERR_DEVICEREMOVED`, and unmaps a newly returned transfer instead of
exposing stale data. The extended controlled device fixture passes on the
Switch's .59 baseline, including removal during a successful backend map.
The ARM64 NVK UMD and GDI/softpipe target build. .134 and .135 also confirm
removal is reported at map; neither is a native readback pass.
[ResourceMap contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourcemap)

The earlier adapter, DLL-entry and device fixtures passed with commit
`035062b0`; their captures and tested hashes remain under
`/private/tmp/t214-gpu-round/d3d-umd.2huxro_6/returned/` and
`staged-hashes.json`. The frontend still negotiates D3D10 DDIs only, and its
native presentation callbacks remain unsupported.

Next, connect the Windows D3D runtime, runtime allocations, shared textures,
synchronization and sustained DWM presentation. DXVK
remains a compatibility/code-reuse reference.

## Recovery and artifacts

.134 through .139 were restored to .59 Code 0, most recently at 19:22:02 UTC.
Original ACPI/driver hashes match, tasks/permit and temporary ICD registration
are gone, and before/after power reports match. Firmware/Hekate were unchanged.

The .139 artifacts are under `/private/tmp/t214-gpu-round/native-ddi139.mp3g_eov/`.
All 25 test records and five recovery records match device SHA256/lengths.
The ETL export has 107,298 records with matching count and record boundaries.
`app-manifest.json` identifies the tested UMD, ICD and probe; the controlled
vertex-input regression is retained in .138's `vbuf-model-result.json`.

| .139 artifact | SHA256 |
| --- | --- |
| Signed KMD | `24808e81100e943fa7a655e9d626f3f88fc753ab18ff5ae07bc35756c94b93d3` |
| Passing native test result | `ff6b3a77f54e414f15b3b1250aa09cc363dc95f7e7e0181e976529cefc61a642` |
| Early/live startup journal | `bf6c2f1ac77ca351a70842a7850e3ecfab1702e130adc490c0e5864068bfdde7` |
| ETL export | `cce54b27c6b7b85d96d5519b9367eba5e555d3429e15d8200825bea3879e8b6d` |

Artifacts: `/private/tmp/t214-gpu-round/native-ddi134._opeardc/` (24 test and
five recovery records) and `/private/tmp/t214-gpu-round/native-ddi135.3m6lfgsj/`
(25 test and five recovery records). Each downloaded record matches the
device SHA256/length in `physical/verified.json` and
`physical/verified-recovery.json`. `app-manifest.json` identifies tested
binaries; .135's `icd-with-log/` is a later build, distinct from its tested
`app/`. .136 artifacts: `/private/tmp/t214-gpu-round/native-ddi136.govmyen6/` (26
test records and seven recovery/model records match device SHA256/lengths).
QMD implementation and its controlled model are in `qmd-wip/` and
`qmd-model-result.json`.

The .137 artifacts are under `/private/tmp/t214-gpu-round/native-ddi137.5ktxn6se/`.
All 25 test records and five recovery records match device SHA256/lengths.
The ETL export has 103,455 records with matching count and record boundaries.

| .137 artifact | SHA256 |
| --- | --- |
| Signed KMD | `af5ef53a7ea4803d1c8127265704cfbe6098422cfd8b36f47817db1ceb145b39` |
| Passing native test result | `d404120b7a31f6564b3af562ded2ef82d641a6754171a75c65ffdd4555b5496a` |
| Early/live startup journal | `6b15b8b95bef0c05fd1f6653d29576a4fe2dc3ca321aeb31cab318c88915b7c5` |
| ETL export | `5eed92183066fafe6c2fa97564f1f260e2fd760a97597a9c900c118f2dd8e97e` |

Earlier .133 artifacts are under `/private/tmp/t214-gpu-round/native-ddi133.q2b6fbs1/`.
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
