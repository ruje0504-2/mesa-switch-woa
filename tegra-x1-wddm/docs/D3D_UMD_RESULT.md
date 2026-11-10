# NVK-backed D3D UMD bring-up

2026-11-10, Windows 11 ARM64 23H2 on Switch Lite HDH-001.

**.137 passes native D3D UMD clear/copy/readback on GM20B.**
The explicit direct-DDI probe selects matching KMT/active VidPN LUID `a3dbb`,
opens the real NVK UMD device, and checks every 64x64 RGBA pixel after three
clear/copy/map passes. It runs 18:29:43–18:29:53 UTC and exits 0 without a
timeout. Each staging image is first filled with a different sentinel.

| Pass | Expected RGBA word | Checked pixels | Mismatches | Row pitch |
| --- | --- | --- | --- | --- |
| Red | `ff0000ff` | 4,096 | 0 | 256 |
| Green | `ff00ff00` | 4,096 | 0 | 256 |
| Blue | `ffff0000` | 4,096 | 0 | 256 |

The driver is Code 0 and reports a 512 MiB ceiling. Its first native fence
`3c`, 8,532 bytes, completes with Reference `1d` and proof mask 7. The journal
has no recorded recovery/rejection/MMU fault; the pixel checks, rather than
missing events, establish the successful result. The ICD and UMD binaries
are unchanged from .136. The KMD separates GPU-writable QMD data from the
read-only command stream, resolving the previous DFALCON read-only fault
for this tested path.

This is an offscreen direct-DDI test. It does not exercise D3D Draw or
application shader DDIs, D3D11CreateDevice, runtime resource sharing, DWM
composition or Win32 Vulkan presentation.

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
The ARM64 NVK UMD and GDI/softpipe target build. .134 and .135 also confirm
removal is reported at map; neither is a native readback pass.
[ResourceMap contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourcemap)

The earlier adapter, DLL-entry and device fixtures passed with commit
`035062b0`; their captures and tested hashes remain under
`/private/tmp/t214-gpu-round/d3d-umd.2huxro_6/returned/` and
`staged-hashes.json`. The frontend still negotiates D3D10 DDIs only, and its
native presentation callbacks remain unsupported.

Next, validate native draws/shaders and connect the Windows D3D runtime before
connecting runtime allocations, shared textures, synchronization and sustained DWM presentation. DXVK
remains a compatibility/code-reuse reference.

## Recovery and artifacts

.134 through .137 were restored to .59 Code 0, most recently at 18:38:03 UTC.
Original ACPI/driver hashes match, tasks/permit and temporary ICD registration
are gone, and before/after power reports match. Firmware/Hekate were unchanged.

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
