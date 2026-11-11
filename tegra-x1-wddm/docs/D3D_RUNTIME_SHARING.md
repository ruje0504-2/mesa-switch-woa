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

## Runtime context registration

The full retained kernel stream from `sampler-binding-auto-20261111` was
reviewed after the UMD-only export. The existing native exporter produces
252,932 complete records, including 239,158 DxgKrnl events, with an exact
count/length match. Every DxgKrnl payload decodes to the installed provider's
matching ID/version template without unknown layouts or trailing bytes.
The raw export and provider metadata match device hashes. This remains a
circular trace, not a complete boot history. Analysis artifacts are under
`runs/dwm-mode-analysis-20261111/` in the external build root.

DWM PID 460/TID 1600 records 142 `AzureTriage` events saying that a signal
requires at least one broadcast context. They occur during the repeating
mode/resource cycle, 7–229 ms after the preceding retained successful UMD
SetDisplayMode. The source never registered a context with the D3D runtime:
its NVK queue owns a separate KMT device/context. This is a concrete contract
gap and a stronger lead than the 562 unsupported IOCTL results. Those IOCTL
records have a zero control code in their captured stack locations and do
not establish the cause of the DWM loop. Adapter pointers also differ across
the visibility/flush/history records; do not attribute every kernel Present
event to the Tegra rendering path.

The working tree now creates a virtual-addressing context through
`pfnCreateContextVirtualCb` on the real runtime device and destroys it through
the matching callback. Creating the UMD device fails with the original
HRESULT if registration fails. No NVK KMT handle is substituted for a runtime
handle. Until the two owners share a GPU timeline, the UMD's Flush waits for
its Zink/NVK fence before the runtime may signal its separate context; device
destruction drains the GPU before destroying that runtime context. This is
a synchronous correctness bridge with a performance cost, not a claim that
Present or shared-image integration is finished.
[Virtual context callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_createcontextvirtualcb)
[D3D10 Flush contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_flush)
[DXGI Present context ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgiddicb_present)

ARM64 compilation passes. The controlled device fixture at 11:31:00 UTC
checks callback ownership, failed-create cleanup, GPU fence waits and failed
wait propagation, as well as existing rendering/resource cleanup. The other
primary/adapter/entry controls pass; their finalized ETL has 57 valid events
without recorded loss. Native candidate `runtime-context-auto-20261111`
uses UMD artifact `runtime-primary-20261111-umd+41f76160496e`, the same ICD
and signed .144 KMD.

That first trial reaches Code 0/native LUID `9dd9e` at 11:35:11 UTC but does
not run either pixel probe. The interactive observer fails to publish its
fresh JSON because the automation's read temporarily denies writes. Its
separately completed display-path records show the correct native identity;
the gate correctly refuses the stale Code 43 snapshot. The next harness
uses a shared read and waits for the fresh snapshot before opening its path
log. This is a harness fix, not a changed GPU binary or relaxed identity gate.

The retained kernel stream has 252,348 complete records; all 245,967 DxgKrnl
payloads decode against the installed templates. DWM PID 764 records 316
`SignalSynchronizationObjectFromGpu` events with `ContextCount=1`, non-null
contexts and advancing fence values. Its UMD GPU-completion records return
S_OK. The old empty-broadcast diagnostic is absent from this retained window,
but the finalized UMD export reports two lost events, and the trace is
circular. These are positive synchronization observations, not proof of
complete startup coverage or a desktop fix. Primary/mode recreation remains.
All 120 returned files (56,354,345 bytes) match device hashes; .59 Code 0,
cleanup, portrait and unchanged power policy are verified at 11:39 UTC.

`runtime-context-shared-auto-20261111` reuses those exact binaries and their
verified controls. In addition to the reader fix, it records DXGI API return
values and DWM composition/scheduling at informational level using templates
read from the device.

On native LUID `9b7ff`, the probes run at 11:54:21.950–11:54:41.669 UTC.
All 16,384 ordinary clear/triangle pixels and 24,576 independent `t5/s2`
sampling pixels match. Both explicitly release their context/device, return
S_OK from GetDeviceRemovedReason and exit 0 without timeout. Their runtime
context creation and destruction callbacks succeed. DWM PID 488 records 401
successful GPU-completion waits and 134 successful SetDisplayMode callbacks;
261 kernel GPU-signal events have `ContextCount=1` with non-null contexts and
advancing fence values. No empty-broadcast diagnostic is retained. The ETL
is finalized with 13,878 valid UMD events and no reported event/buffer loss.

The full export has 253,480 records, including 228,484 DxgKrnl, 7,728 DXGI
and 3,370 Dwm-Core events. All three providers decode with exact payload
lengths against this OS's templates, including nested DWM structures. This
still covers the retained circular window, not the entire startup.

### Fullscreen transition boundary

DWM PID 488/TID 1900 attempts SetFullscreenState(TRUE) 133 times. Both the
DXGI operation's return event and its API return event report `887a0025`,
`DXGI_ERROR_MODE_CHANGE_IN_PROGRESS`, on every retained complete pair. This
is the error value, not the different success-severity status `087a0008`.
Each attempt's output remains windowed, with null primary/output pointers;
the corresponding swapchain is then destroyed and recreated. The UMD's
preceding SetDisplayMode returns S_OK; the later DXGI return follows it by
3.2–161.6 ms (median 14.0 ms). Its first journal error appears immediately
after that DDI return. The error identifies an incomplete transition but
does not establish which component causes it.
[Microsoft error-code definition](https://learn.microsoft.com/en-us/windows/win32/com/com-error-codes-10)
[SetFullscreenState contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate)

The swapchain requests three 720x1280 BGRA buffers, 60/1 Hz, sample count 1,
and swap effect 1. There is no retained UMD Present/Blt/OpenResource call or
runtime-error callback. Those absences do not prove what happened before
the circular window. Two retained DWM HWDEVICE creation events identify
Microsoft Basic Render Driver LUID `547d`/FL12_1; they must not be reported
as Tegra feature-level results. The native D3D probes remain FL10_0.

Capture is still black on the native `Default` desktop, with matched native
adapter, origin (0,0), 96 DPI, portrait 720x1280 and identity rotation.
Visible DWM composition and sustained Present are not established. The next
investigation is transition completion and the primary/presentation contract,
not an unsupported feature-level increase or speculative scanout-register
change.

Read-only inspection of the device's `d3d11.dll` 10.0.22621.2715 clarifies
the apparently successful DDI. `NDXGI::CDevice::SetDisplayModeCB` stores both
the actual D3DKMT NTSTATUS and a mapped HRESULT in its per-call state, then
returns S_OK for selected mapped errors, including `887a0025`. The outer
SetDisplayMode operation consumes that saved result after the UMD returns.
Consequently, a successful callback alone cannot establish that the kernel
mode change succeeded. The mapping includes invalid parameters and unsupported
VidPN modality as well as genuine present-mode/ownership changes; the generic
DXGI code cannot distinguish them. Its kernel-call wrapper records failures
in the D3D11 journal, which was not enabled in this trial.

The DLL's SHA-256 is
`ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`.
The public symbol GUID is `4DD64C1C-37D9-00CB-45AF-89C52B2A1534`; the image
requests age 1 and the stripped public PDB reports age 2. Relevant native
callback instructions are at RVA `548a0`–`54b74`; the status-save/masking
sequence is at `54b1c`–`54b40`. This static inspection motivated the following
capture; it does not by itself select one of the possible kernel statuses.

### Raw mode-setting status

`mode-journal-auto-20261111` enables the installed Direct3D11 provider's
JournalEntries keyword (`0x800`, event 1820) with unchanged UMD/ICD/KMD
binaries. A 12-second DWM observation on native LUID `953d1` runs at
12:29:40.175–12:29:52.929 UTC. It does not rerun the pixel probes or establish
a new drawing/presentation pass.

The retained D3D11 journal contains **144 SetDisplayMode failures from DWM
PID 812/TID 1864**, all `c01e0005`
(`STATUS_GRAPHICS_PRESENT_MODE_CHANGED`). Each precedes that thread's UMD
SetDisplayMode S_OK record by 9.5–20.8 microseconds (median 10.7). DXGI reports
144 corresponding fullscreen failures with `887a0025`. This confirms the
runtime's status conversion in the observed path. These specific failures
are neither OOM nor invalid-parameter results. Their cause within mode/
primary compatibility is still to be established; do not infer a particular
rotation, format or timing field from the generic mode-change code alone.

The other 15 D3D11 journal entries belong to different process IDs and cover
UMD access/interface selection. They are not DWM's mode-setting failure.
The next check is the actual primary description and active VidPN state,
including the meaning of PreserveVidPn. The existing source's allocation
description and mode/rotation fields require comparison to that state before
changing behavior.
[SetDisplayMode input contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_setdisplaymode)
[PreserveVidPn flag](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_setdisplaymode_flags)

The raw export has 250,385 complete records, including 229,940 DxgKrnl,
7,984 DXGI, 3,582 Dwm-Core and 159 Direct3D11 events. All four providers
decode exactly against the device templates; no unknown or malformed
payloads remain. The finalized UMD export has 8,712 valid events with no
reported loss. All 132 returned files (57,562,355 bytes) match device hashes.
Raw-export SHA-256:
`9f51635babac995098db73d39330ebf8d6251b92cbf43281e71479d466df3dbe`.
.59 Code 0 and complete cleanup are verified at 12:33:15 UTC after the
recovery reboot. Portrait restore/verification at 12:33:29 UTC is a no-op,
and the power reports are unchanged. Artifacts are in the external build
root's `runs/mode-journal-auto-20261111/` directory.

For the preceding runtime-context drawing trial, all 145 returned files
(58,992,567 bytes) match device lengths and SHA-256.
The raw event export hashes to
`de5210fd632314db473658c25a6c50915cdf47a7608ff80826b02089d18b868a`.
The automatic rollback is verified at 11:58:44 UTC: .59 Code 0, original
driver/ACPI hashes, cleared temporary tasks/registration/environment and
unchanged power policy. Portrait restore/verification at 11:59:25 UTC is a
no-op. Evidence and decoding artifacts are in the external build root's
`runs/runtime-context-shared-auto-20261111/` directory.

Numeric event 30, `RuntimeContext`, uses `arg0` stages 1=create, 2=destroy,
3=GPU completion. `arg1` records a non-null returned context or completed
fence as a boolean, not an opaque handle value; the HRESULT is preserved.

### Allocation rotation compatibility

Read-only inspection of this device's `dxgkrnl.sys` 10.0.22621.2792 locates
an explicit rotation check inside `DXGDEVICE::SetDisplayMode`. Before calling
DescribeAllocation, RVA `1494ac` initializes `CheckDisplayMode=1` and
`Rotation=UINT32_MAX`. A returned rotation other than that sentinel is
compared to the active mode at RVA `269ab4`; a mismatch reaches the
`c01e0005` return at `269b04`. This is a concrete path to the observed error,
not proof that every occurrence takes this branch. The kernel image hashes
to `99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`;
its public symbol GUID is `797F457A-6488-C67C-8E55-B0F3E771DC32`.

Our old whole-structure assignment returns Rotation=0, while all current
image layouts are unrotated and the active portrait mode reports identity
(1). The full 180-byte old function is byte-identical to a sequence at file
offset 73180 in the signed `.144` SYS, SHA-256
`e9cc6c5e576afd6861047983567eb2b4628209a5f227cdadc3ef48e86d59144c`. The
fix explicitly reports `D3DDDI_ROTATION_IDENTITY`; it neither rotates pixels
nor advertises a new orientation. The public DescribeAllocation page calls
Rotation reserved, so the observed OS behavior must be distinguished from
that documentation, rather than attributed to it.
[Allocation description](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_describeallocation)
[Rotation values](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ne-d3dukmdt-_d3dddi_rotation)

The real-WDK ARM64 fixture seeds the same sentinel and checks the expected
identity result. The old implementation exits 675 at the new assertion
(12:54:10 UTC); the fixed implementation exits 0 (13:01:39 UTC). Both run
on `.59` with controlled kernel substitutes and no native GPU execution.
The source-owned native startup gate's eleven cases also pass at 13:18:21
UTC. All six returned control logs/JSON files match device SHA-256.
The controls and binary analysis are retained externally under
`runs/describe-rotation-control-20261111/` and
`runs/dwm-mode-analysis-20261111/`. A newly signed `.146` package, rebuilt
from source with the documented gate, is the native validation candidate;
the controls alone do not establish a desktop or hardware rotation pass.

### Native identity-rotation trial

The first `.146` controller attempt aborts before arming the GPU because its
new directory lacks two capture utilities. No native startup is claimed for
that attempt. It returns to `.59` Code 0 with the original display/power
state, and all 99 returned files match device hashes. The corrected preflight
checks those utilities before installing the unchanged signed package.

`describe-rotation-native-v2-20261111` uses the source-built `.146` KMD,
SHA-256 `2b88feb19ec679ebc80caa16b0249222ed633ed8b29654705df7d56934ca55d6`,
with the previously validated `41f76160496e` UMD and `f828845c38b7` ICD.
This also replaces the lost temporary entry wrapper with the tracked native
gate; it is not a binary-only single-field A/B experiment. Hardware startup
completes with Code 0 and a consumed permit. On LUID `92611`, the probes run
at 13:37:06.926–13:37:27.406 UTC: all 16,384 clear/draw pixels and 24,576
sampling pixels match, both devices explicitly release and both tests exit
0 without timeout or a removed-device error.

In the retained 13:37:05.364–13:37:33.911 UTC window, DWM PID 812 creates
15 devices identified as **Tegra X1 (GM20B), LUID `92611`, FL10_0**. All 14
complete fullscreen transitions return S_OK, set `NewWindowed=0`, and report
a non-null primary. The journal contains no SetDisplayMode failure in that
window. These positive DXGI results establish passage through the mode-setting
boundary; they go beyond the UMD callback's previously masked success.

The next concrete failure is the existing hardware `_Present` stub in
`DxgiFns.cpp`: all 14 DWM calls return `887a0004` (unsupported). The runtime
records 14 driver-internal-error removals (`887a0020`), and the outer DXGI
Present returns `887a0005`. Feature-level negotiation warnings precede
successful FL10_0 device creation and must not be substituted for this
presentation failure. Wire the real runtime presentation callback after
finishing GPU work and preserving allocation/context ownership. Runtime
shared-image integration is also still incomplete.

The 13:37:27 capture is no longer black: it contains the Windows wallpaper
and desktop icons. Its immediately preceding visibility record identifies
the native ACPI adapter, `Default` input desktop, origin (0,0), 96 DPI,
720x1280 source/signal and identity rotation. Capture SHA-256 is
`d98de373862ee54a5408a3edb39cd3e4b58a56c61ea9377090c2841dbd8e2394`.
This is a desktop capture, not a physical panel photograph, a verified
hardware-composited frame, or evidence of sustained successful Present.
Hardware rotation and final 1280x720 landscape remain to be implemented.

The raw export contains 235,099 complete records: 176,364 DxgKrnl, 7,625
DXGI, 1,250 Dwm-Core, 46 Direct3D11, 49,812 UMD and two trace headers.
All four OS providers decode with exact payload lengths and no unknown
events. Raw-export SHA-256 is
`c2147e5828e78ab172c1bd40b1945d74b953bfe7a33f5283036bfff0ad650a87`.
The original exporter summary files are filled with zero bytes. A read-only re-export
from the retained ETL produces the identical 49,812-event UMD JSONL hash
`7c2fdbdcead5af166f70ec6cd9e5b7637109825b83551547ab4e790f5cb71cb8`:
ProcessTrace=0, invalid=0, events/buffers lost=0, nonzero EndTime and 2,444
buffers written. This validates finalization of the retained circular trace,
not coverage of the entire startup.

A further Windows boot at 13:38:58 is observed before a durable completion
record from collection/restoration. The record does not establish its cause.
The consumed permit keeps the restarted `.146` closed; the subsequent
recovery restores `.59` Code 0, original driver/table hashes, cleared test
registration/tasks/environment and unchanged power policy at 13:43:47 UTC.
Portrait restore/verification at 13:44:09 is a no-op. All 149 returned files
(76,015,725 bytes, including the re-export) match device SHA-256. Evidence,
strict decoding and summaries are under the external build root's
`runs/describe-rotation-native-v2-20261111/` directory.
