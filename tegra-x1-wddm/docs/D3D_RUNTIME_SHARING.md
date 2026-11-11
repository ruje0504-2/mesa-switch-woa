# D3D runtime resource association

2026-11-10, Switch Lite, Windows 11 ARM64 23H2 (22631.2861).

The current desktop validation route is D3D UMD → Zink → NVK; DXVK is the
game-facing route. Desktop DXVK is deferred pending explicit user approval.
This investigation covers the Windows resource
association required by the desktop UMD. It does not establish shared-image
rendering, synchronization or DWM composition.

## Probe and controlled baseline

`nvk_d3d_runtime_check.exe --luid <LUID> --shared-contract` creates two
native D3D11 devices at feature level 10_0 and tests 64x64 BGRA textures.
It obtains/opens a legacy shared handle, releases the original texture and
checks the consumer descriptor. It then repeats with an NT/keyed-mutex
resource. Legacy handles are not closed as NT handles; the NT handle is
closed after opening. No drawing or copy command uses these textures.

The exact executable passes both cases with `--warp --shared-contract` on
the .59 baseline at 22:14:55 UTC, exit 0. This validates the application
probe against the Windows software implementation, not the Tegra driver.

The .145 diagnostic UMD retains the .144 Zink/NVK device path but intercepts
the shared-resource callbacks in the probe process. It allocates one raw
16 KiB KMT buffer through `pfnAllocateCb`, passing the real `hRTResource`;
opens the runtime-provided allocation and releases it through
`pfnDeallocateCb`. It also records `D3DKMTShareObjects` results. These
diagnostic resources have no Gallium/NVK image and must not be rendered to.
The diagnostic source lives in the artifact directory, not the production
`Resource.cpp`. The .144 ICD and KMD core are unchanged.
[Runtime creation/destruction contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/handling-resource-creation-and-destruction)

## .145 native observation

Code 0, matching KMT/active VidPN LUID `9edf6`, 512 MiB ceiling.
The probe runs 22:24:11–22:24:16 UTC, exits 1 and does not time out.

| Operation | Observation |
| --- | --- |
| Legacy allocation callback | `S_OK`; allocation handle `400019c0`; `hKMResource` remains zero in the output structure |
| Runtime legacy export | `GetSharedHandle` succeeds; handle `400002c2` |
| Consumer open callback | Resource `40001a00`, allocation `40001a40`, one allocation with 16 private bytes, zero resource-private bytes |
| Legacy resource release | Both deallocation callbacks return `S_OK`; the opened descriptor still matches after producer release |
| NT export attempt on the legacy resource | `D3DKMTShareObjects` returns `c000000d`; no NT handle is obtained |
| NT texture creation | `CreateTexture2D` with `SHARED_NTHANDLE | SHARED_KEYEDMUTEX` returns `80070057` (`E_INVALIDARG`) |

The zero creation-time `hKMResource`, later opened resource handle and
legacy global handle are distinct observations. Do not cast runtime opaque
handles or allocation handles into another handle class. The existing NVK
NT import cannot be connected merely by substituting one of these values.
[NT sharing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)
[NT texture creation requirements](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsharedhandle)

Two diagnostic limits matter. The open callback printed a `va` field using
an `OPENALLOCATIONINFO2` view, but that extended layout was not established
for the negotiated D3D10 DDI. That field is discarded; it proves no GPU VA
mapping. The allocation handle/private-data fields have the SDK's common
prefix. Future instrumentation must use the negotiated structure layout.
Also, the create hook only intercepts the DDI shared flag. The lack of an
NT allocation-hook record does not prove that the runtime never entered
any UMD callback, or identify the callback responsible for `E_INVALIDARG`.
[Open-resource structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_openresource)

Next, resolve the negotiated DDI/resource contract and the typed storage
bridge before binding a Zink image to runtime-owned memory. Validate GPU
contents and synchronization separately. .144 remains the native Vulkan
sharing and D3D rendering baseline.

## Recovery and artifacts

The 22:30:30 UTC check confirms .59 Code 0, original driver/ACPI hashes,
removed test tasks/permit and temporary ICD/UMD registration. Display and
sleep settings remain Never. Firmware/Hekate were not changed.

The 27 downloaded test records total 9,973,055 bytes and match every
device-side length/SHA-256. Five recovery records also match device hashes;
the before/after power reports are byte-identical. The ETL export contains
122,665 records in 9,574,862 bytes with matching count and record boundaries.
Artifacts and diagnostic source:
`/private/tmp/t214-gpu-round/runtime-sharing145._hkhc88e/`.

| Artifact | SHA-256 |
| --- | --- |
| Diagnostic .145 SYS | `5aa6c8788f265531140d7daff4274f611fd3af281f1a1fe425385adb68dcfb85` |
| Diagnostic UMD | `6f528daad49df78acf7c96a83f04eed07ac51890461ae4f11a0c3c8b4842cc86` |
| Runtime application probe | `fd7523c45d6bedda71acb10c36571b69789e9486734324b16f3226351b77f185` |
| Diagnostic Resource.cpp | `fa849818caac21fd3c02ba8c65a30f1c398354e4f5da82d066636ea67c063b91` |

## Resource metadata work

On 2026-11-11 the KMD create/open paths gain a resource-level storage
envelope. Both previously rejected all nonempty resource-private data.
`tg_gpu_resource_request` is a 32-byte, pointer-free header with version,
header length, allocation count, renderer-payload length and the sum of
page-rounded allocation sizes. Allocation creation validates it before
charging memory or allocating objects; the KMD retains the storage header.
The trailing renderer payload is opaque to the KMD and is not yet a defined
or implemented D3D image-layout contract.

Windows forwards the original resource-specific data to OpenAllocation.
The KMD now accepts matching metadata, requires the opened allocations to
belong to the same fixed allocation set, and preserves each device's
read/write access. Later allocation appends are rejected for these fixed
sets; resources without private data retain the original behavior.
[CreateAllocation resource data](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_createallocation)
[OpenAllocation resource data](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_openallocation)

The D3D runtime allocation handle cannot simply be used with NVK's separate
paging queue: MakeResident requires allocations to belong to the device
that owns that queue. Sharing/binding must preserve device ownership and
residency as well as the process GPU address space. The .145 legacy/NT
handle and negotiated DDI questions remain open.
[Residency device ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_makeresident)

The standard KMD build passes the host models and builds the ARM64 library
and WDK fixtures. At 00:34:50 UTC, `test-context.exe` passes on the .59 Code 0
baseline with controlled OS/GPU substitutes. Coverage includes malformed
headers, incorrect rounded totals, create/open allocation failures, balanced
references, cross-device read-only access after another device closes, and
append rejection without changing existing allocations or budget charges.
Existing buffer, shared-open and context/queue tests also pass. This is not
a live installation or GPU execution result; the desktop image bridge and
DWM remain pending.

Executable SHA-256:
`2cc70b21bf3c9458d0f5511cbb7f5a3ec75d91eb716db4a3f090d9380e8cb730`.
The downloaded final report matches device-side length and SHA-256
`19b660447c0a226c0231b380379bd0ee0891db400c95c38244068bf0ff4b09ae`.
Artifacts, exact build command and source diff are retained under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/resource-metadata-20261111/`.

The former `/private/tmp/t214-gpu-round` host directory is no longer present.
The C/C++ toolchain and SDK/WDK were restored into
`/Users/rujian/.local/share/mesa-switch-woa-build/`: LLVM-MinGW 20261104 and
SDK/WDK NuGet 10.0.26100.6584 (Include 10.0.26100.0). Download origins/hashes
are recorded in `downloads-manifest.json`. Rust 1.90.0, Python dependencies
and the host `mesa_clc`/`vtn_bindgen2` have since been restored under that
same persistent root. Complete ARM64 UMD and NVK ICD builds pass. Five
rebuilt controlled Windows fixtures also pass on .59: UMD adapter, DLL
entry, device, KMT transport and shared-resource lifetime. These fixtures
use controlled substitutes and do not establish native presentation.
The two .144 native result JSON files and the .145 native result JSON were
recovered from the device into `recovered-evidence/`; all three match the
original verified hashes. This does not restore the former host build trees
or the .145 diagnostic source.

## Runtime primary bridge work

The 2026-11-11 working tree connects the D3D primary's allocation owner to
Zink/NVK. This addresses the explicitly unsupported SetDisplayMode boundary
observed after the SAMPLE fix; it is not yet a native presentation result.

`RuntimeMemory.cpp` allocates through the runtime's callback with the real
`hRTResource`, `Primary` flag and VidPN source. It retains the returned
allocation handle separately from the optional kernel resource handle.
Residency, paging-fence waits and GPU/CPU mappings use that same device's
callbacks and paging queue. No runtime opaque handle is cast to a KMT device
or NT shared-resource handle. The implementation currently targets the KMD's
existing 720x1280 BGRA primary, 2880-byte pitch and 3,686,400-byte allocation.
The 16 MiB scanout segment and overall 512 MiB ceiling are unchanged.
[Resource association](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_allocate)
[Primary description](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc)

Zink passes a typed, Mesa-internal owner description through the existing
WSI allocation chain. NVK checks its adapter LUID, size, access and callbacks,
retains the owner, and maps this allocation into its GPUVA reservation.
The owner completes its paging fence before returning success. Process-wide
GPUVA makes the allocation addressable by the NVK context without sending
the runtime's allocation to an unrelated device's paging queue. An explicit
import acknowledgement prevents an older/different ICD from silently
allocating unrelated memory. This is not a new public Vulkan external-handle
type; it cannot be exported as OPAQUE_WIN32 memory.
[GPUVA process scope](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)
[Mapping and paging completion](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_mapgpuvirtualaddress)

For the explicit native pitch, NVK uses its existing tiled-shadow rendering
and GPU resolve implementation for linear images. The internal image query
does not change the public linear-image format capabilities or advertise a
DRM extension on Windows. There is no CPU pixel-copy presentation path in
this change. The UMD waits for rendering before calling `pfnSetDisplayModeCb`
with the actual primary allocation and returns its HRESULT unchanged.
Other modes, format conversion and rotation remain work beyond this fixed
primary layout; Present and shared-resource opens are also still unfinished.

Resource destruction waits for GPU use, releases the complete GPUVA
reservations, and then deallocates through the runtime resource handle.
The owner context survives delayed Zink/NVK object cleanup; subsequent frees
of already-released ranges are no-ops and further mapping requests fail.
This ordering matters because runtime deallocation automatically releases
associated GPUVAs and those addresses can be reused immediately.
[Free/deallocate ordering](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_freegpuvirtualaddresscb)

Both ARM64 builds pass. On .59 Code 0 at 10:18:44 UTC, the new controlled
primary fixture passes ownership, allocation flags, exact byte count,
residency/map waits, failure cleanup, mode HRESULT propagation and delayed
cleanup without a second VA free. Existing UMD adapter, device and DLL-entry
controls also pass; their finalized ETL exports 46 valid events with no
recorded loss. These callbacks are substitutes, not real GPU allocations.
The candidate `runtime-primary-auto-20261111` reused the signed .144 miniport.
UMD SHA-256:
`78d45f3d4352c37da1ca6780a5482ffe3fe9fd9cd844506afed4c88c70a01cb9`;
ICD: `f828845c38b7bfb6e34ef83beb1c873009c2f89afcd014090a82ec4f926a2f30`.

On native LUID `9750b`, DWM creates fourteen real runtime primary allocations
of 3,686,400 bytes, all successfully. Each `MakeResident` then returns
`8000000a` (`E_PENDING`), with paging fence `1b59`. The first implementation
incorrectly treated the HRESULT failure bit as a terminal error and
deallocated each primary without waiting. It therefore did not reach the
display-mode callback. The finalized ETL has 57,369 valid events, with no
recorded loss. All 131 returned files (44,406,284 bytes) match device hashes.
.59 Code 0 and unchanged power settings were verified at 10:29:21 UTC;
portrait restoration was a no-op at 10:29:36 UTC.

`MakeResidentCb` and `MapGpuVirtualAddressCb` explicitly permit `E_PENDING`:
the operation has been queued, and the returned paging fence must complete
before GPU access. The implementation now handles this status only for
these paging callbacks, preserving their raw trace values and propagating
a failed wait. This does not turn arbitrary failed HRESULTs into success.
The extended ARM64 fixture passes queued residency/mapping and failed-wait
cases on .59 at 10:41:09 UTC; existing UMD controls export 46 valid events.
[Residency return values](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_makeresidentcb)
[Mapping return values](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_mapgpuvirtualaddresscb)

The same native trial matches 16,384 ordinary clear/triangle pixels, but its
explicit device release faults with `c0000005`. The matching UMD's PC RVA
`112f18` reads a freed `curr_program->last_variant_hash` during shader unbind;
LR `111530` is in `zink_bind_fs_state`. Program pruning now clears the
context's non-owning selected-program pointer and its hash contribution
before releasing references. Native sampling separately reports 2,940
mismatches in 4,096 pixels, a pattern consistent with clamp addressing
instead of the requested wrap. This does not yet distinguish an incorrect
sampler binding from a missing/default state. The next probe runs all three
sampler/palette cases and retains its SPIR-V.

`runtime-paging-auto-20261111` tests the paging and lifetime fixes with UMD
artifact `runtime-primary-20261111-umd+d17a557b3a76` and the same ICD above.
Its updated probe passes the WARP reference (24,576 pixels); native results
are pending. Its first automatic attempt observes Code 0 and a consumed
start permit while `HardwareStatus` is still `ffffffff`, then prematurely
starts rollback before the drawing probes. `runtime-paging-wait-auto-20261111`
adds a bounded wait for a terminal hardware status while retaining that
first snapshot; it uses the same UMD/ICD binaries. The harness records each
probe's exit status separately and recognizes negative Windows exception
codes as failures.

The recovered first attempt nevertheless establishes a real DWM primary
bridge pass on LUID `98df6`: 116 successful primary allocations, 116 queued
residency operations, 116 queued GPUVA maps and 232 successful paging waits.
The actual runtime `SetDisplayMode` callback returns S_OK 115 times, with
the real primary allocation handle. DWM has 479 successful resource creations
and no recorded runtime-error callback in this finalized 12,816-event trace
(no recorded event/buffer loss). Repeated primary creation/mode setting is
not evidence of sustained Present or a visible desktop. The drawing probes
were not launched because of the early harness check. All 120 returned files
(36,355,064 bytes) match device hashes. .59 Code 0 and environment/power
restoration pass at 10:48:35 UTC; portrait verification is a no-op at
10:48:49 UTC.

The bounded-wait run reaches native LUID `954c5` at 10:54:18–10:54:40 UTC.
Ordinary RGBA/BGRA drawing matches all 16,384 pixels and completes context,
device and adapter release with exit 0. The sampler probe exits 1 without
timeout: its wrap/clamp/rebound-texture cases have 2,940/0/2,940 mismatches.
This is inconsistent with simply selecting the deliberately swapped `s5`.
The retained finalized circular ETL contains 14,432 valid events, including
140 successful SetDisplayMode callbacks and no recorded DWM runtime error.
Circular retention does not establish a complete startup history.
All 139 returned files (36,812,705 bytes) match device hashes. .59 and cleanup
pass at 10:58:29 UTC; portrait verification is a no-op at 10:58:44 UTC.

Six returned SPIR-V files identify the sampler bug: the fragment shader
correctly loads the independent texture and sampler, but the sampler's
Binding decoration is 2, the original Gallium slot. `emit_sampler` used
`driver_location` instead of the Vulkan `binding` assigned by Zink, unlike
image emission. The next candidate uses the assigned binding and adds a
complete NIR-to-SPIR-V control covering remapped binding 450, binding zero,
and the unchanged-slot case. The old fragment SPIR-V `sample-dump05.spv`
SHA-256 is `fb513b6a16cee202daa5602d0a36f4e3836e5a31b16bdb124b1c7f255441a462`.
`sampler-binding-auto-20261111` tests UMD artifact
`runtime-primary-20261111-umd+826085f423ab` with the same ICD and signed KMD.
On .59 at 11:00:40 UTC, the new regression fixture reproduces the original
generator's binding 2 instead of 450 and exits 1; the patched generator passes
all three cases and exits 0. WARP reference sampling again matches 24,576
pixels. These are compiler/CPU controls.

The patched native trial on LUID `92166` runs at 11:05:01–11:05:09 UTC.
Ordinary clear/triangle drawing matches 16,384 RGBA/BGRA pixels; independent
`t5/s2` sampling matches all 24,576 pixels across wrap, clamp and texture
rebinding in both render-target formats. Both processes report S_OK device
status, complete explicit teardown and exit 0 without timeout. This validates
the sampler binding fix on GM20B. The returned fragment SPIR-V now uses
Binding 451 for the separate precompile and Binding 450 for the linked
program, matching Zink's remapping. The linked fragment's SHA-256 is
`96ffd0ccb1d67d2df0bd5adc1a328ba430348e5943a0ee92911ac574604549e4`.

The retained finalized circular ETL contains 13,760 valid UMD events and no
recorded event/buffer loss, including 141 S_OK SetDisplayMode callbacks. DWM
continues recreating primary surfaces; the retained window has no Present,
Blt or runtime-error records. This does not establish their absence outside
that window or identify the first failure after mode setting. Desktop capture
is still black despite the confirmed Default input desktop, 720x1280 mode,
identity rotation, (0,0) origin and 96 DPI. Visible desktop composition and
sustained presentation are not validated. The next boundary is the repeated
mode/resource cycle and actual Present/shared-image contracts.

All 144 returned files (36,655,102 bytes) match device hashes in
`runs/sampler-binding-auto-20261111/evidence/verified.json` under the external
build root. .59 Code 0, original driver/ACPI hashes, temporary registration,
loader/environment/task/permit cleanup and unchanged power policy pass at
11:09:12 UTC. Portrait verification is a no-op at 11:09:27 UTC.

ETW numeric event 29 is `RuntimeMemory`. The high 32 bits of `arg0` identify
the operation; the low 32 bits contain the allocation handle when available.
`arg1` is the byte count, paging-fence value or paging-queue handle, as relevant.

| Stage | Operation |
| --- | --- |
| 1 | Allocate runtime primary |
| 2 | Make resident |
| 3 | Wait for paging completion |
| 4 | Map GPUVA |
| 5 | Free GPUVA reservation |
| 6 | Deallocate runtime resource |
| 7 / 8 | CPU lock / unlock |
| 9 / 10 | Create / destroy paging queue |
