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
