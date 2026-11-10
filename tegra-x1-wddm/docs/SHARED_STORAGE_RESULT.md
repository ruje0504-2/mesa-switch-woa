# KMT shared-storage result

2026-11-10, Switch Lite HDH-001, Windows 11 ARM64 23H2 (22631.2861).

NT resource export/import, residency and GPU VA mapping pass on GM20B.
The complete storage/readback test fails: `D3DKMTLock2` on the imported
allocation returns `c000000d` (`STATUS_INVALID_PARAMETER`). This is a
prerequisite investigation for D3D UMD/DWM, not a desktop acceleration or
Vulkan external-memory extension pass.

## Implementation and local checks

The Windows KMT transport creates an NT-shareable resource with one Tegra
storage allocation, exports it with `D3DKMTShareObjects`, and imports it
with `D3DKMTQueryResourceInfoFromNtHandle` and `D3DKMTOpenResourceFromNtHandle`.
Imports validate ABI, size and flags, charge the device's budget, and wait
for residency. Each local resource is released independently; the caller
owns the exported NT handle. Resource destruction and budget accounting
also cover partially failed imports. NVK's internal shared-storage flag
uses this allocation path. No Win32 external-memory extension is enabled.

The full Windows ARM64 NVK ICD builds. The SDK-backed controlled fixture
passes in ARM64 WinPE and on the Switch's .59 baseline. It covers repeated
imports, resource ownership, budget rejection and failed-residency cleanup.
The fixture substitutes KMT operations and is not hardware rendering proof.

## Physical evidence

The live test uses the existing signed .130 KMD, not a newly built KMD.
Its startup permit is consumed once. The active VidPN and queried GM20B
adapter both report LUID `0000000000090adc`; the budget is 536,870,912 bytes.
The test selects this exact LUID and submits no drawing commands.

| Operation | Result |
| --- | --- |
| Two KMT devices, paging queues and virtual contexts | Success |
| 16 KiB CPU-visible shared resource creation and residency | Success |
| Producer GPU VA reservation/mapping, CPU lock, pattern write and unlock | Success |
| NT handle export and consumer import/residency | Success |
| Consumer GPU VA reservation and mapping | Success |
| Consumer CPU lock after producer release and NT-handle close | `c000000d` in the first run |
| Consumer CPU lock while producer and NT handle remain alive | `c000000d` in the second run |
| VA, allocation, context and device cleanup | All reported success; both local budget counters returned to zero |

The second run reports distinct producer/consumer allocation and resource
handles and exits with code 3. It fails before reading any imported word.
The first PowerShell wrapper recorded a null process exit code; its explicit
failure log is retained, and the second wrapper records the native exit
code through `cmd.exe`. The failure cannot be attributed to producer or
NT-handle closure because the second run fails before either operation.
The underlying CPU-mapping rejection is still unidentified.

The ETL was exported after stopping the trace. All 111,016 exported records
match the count and boundaries in its export header. The retained paging
and Lock events do not explain the failed imported lock; an absent event
is not treated as proof that a particular kernel operation did not run.

## Recovery

The 12-minute rollback remained armed during both tests and was started
early after evidence capture. At 15:40:52 UTC, SSH confirmed .59 Code 0,
the original driver and ACPI hashes, no test permit and no remaining test
tasks. The saved before/after power-setting reports are byte-identical;
AC/DC display and sleep timeouts remain zero. Windows restarted directly
into Windows. Firmware and Hekate configuration were not changed.

## Contract references and remaining work

The exported access mask is `STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE`,
as documented for readable/writable shared resources. Windows describes
CPU-visible system-memory allocations as lockable; the failed import needs
an explanation before claiming usable host-visible external memory.
[Shared-resource access](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)
[Lock2 and residency requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/allocation-usage-tracking)

The lfrb `wddm2` reference at `801c9763c6043f0de8408e905a5324eea06d81d7`
also opens NT resources, maps GPU VA, makes allocations resident and uses
`D3DKMTLock2`. Its AMD private allocation metadata is not the Tegra ABI;
its implementation is a reference, not proof that this path works here.
[RADV WDDM2 resource transport](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c)

Next, resolve imported CPU access, then validate cross-device GPU access
and completion. D3D texture layout/runtime metadata, UMD adapter/device
negotiation, rendering and presentation callbacks, and sustained DWM
composition remain separate required work. DXVK can inform compatibility
and translation work; it does not supply those native UMD contracts.

## Evidence

The run is staged under
`C:\ProgramData\SwitchWOA\GpuUpdate\shared130-20261110`.
Local builds and captures are under
`/private/tmp/t214-gpu-round/nvk-shared.qj2IaJ/`.
All 28 downloaded test records and five recovery records were verified
against device SHA256 and length. See `physical/verified.json`,
`physical/verified-recovery.json` and `physical/returned/` under that path.

| Artifact | Device SHA256 |
| --- | --- |
| Signed .130 KMD | `2a5381079bab0553f09507d6f2a8fb1b539bad398f0c3673d3b5e7d53290bb85` |
| Second live test log | `f85322c222949626a07938df6e19a1a4f4e80d206b6ee10d6a153f83851445c5` |
| ETL export | `49ee8dab546ae4fe2dc78e5dd57c44162546ce4a4183858d670798009e9920d1` |
