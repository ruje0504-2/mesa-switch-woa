# NVK-backed D3D UMD interface bring-up

2026-11-10, Windows 11 ARM64 on Switch Lite HDH-001.

The experimental `TegraGpuD3D.dll` connects Mesa's D3D10 Gallium frontend to
Zink/NVK. Three controlled tests pass on the Switch at 16:53:36 UTC. The
installed driver remains .59 Code 0; no driver installation, reboot, firmware
change or power-setting change was performed for these tests.

## Implementation

`gallium-d3d10-backend=nvk` queries the runtime adapter callback for the Tegra
ABI and requires a valid LUID and the KMD's native-submission capability.
It opens Zink on that exact LUID. Zink now also checks `deviceLUIDValid`
before accepting a match. No software-renderer fallback or software-adapter
KMT thunks are linked into this target. The default GDI backend remains
available separately.

The frontend handles context/empty-shader creation failures and releases
partially created objects. Resource creation now tests the backend's
allocation result, reports initial-data mapping failures, and cleans up
those allocations. The ARM64 build uses Windows `DebugBreak`, and a D3D-only
build includes Gallium's shader/state helpers without enabling OpenGL.

The hardware target returns `S_OK` after successful DDI device creation,
rather than selecting GDI's no-redirection mode. Present and SetDisplayMode
remain explicitly unsupported until the runtime resource/presentation bridge
exists. The build still negotiates only D3D10 DDIs; D3D10.1 and D3D11 DDIs
are not enabled.
[Adapter callback contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openadapter)
[Device creation status](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createdevice)

## Validation scope

| Check | Result |
| --- | --- |
| Windows ARM64 NVK UMD and test executables | Build successfully with `tools/build_umd.py` |
| Existing GDI/softpipe target | ARM64 build still succeeds |
| Adapter fixture | Runtime callback ABI, exact 64-bit LUID, capability rejection and backend failure propagation pass; Zink is substituted |
| DLL entry fixture | Actual DLL loads; software KMT exports are absent; both adapter entry points propagate controlled callback/gate failures correctly |
| Device fixture | Real frontend DDIs negotiate a version, create/destroy a device, and clean up failed context/shader/resource/initial-map operations; Gallium is substituted |

All three executables exit 0 on the Switch. These are controlled interface
tests, not calls to `D3D11CreateDevice` through the Windows runtime and not
GPU rendering proof. The build retains existing `ShaderTGSI.c` warnings
about operand paths using an uninitialized `reg`; shader execution has not
been validated by this round.

## Next physical work

Validate the UMD's adapter/device path with native NVK submissions on GM20B,
then clear/draw/read back known pixels through the DDI. Connect D3D runtime
allocation callbacks, texture layout and OpenResource to the storage/sharing
path proved by .132. Native presentation, shared-surface synchronization and
sustained DWM composition remain required. DXVK remains a compatibility and
code-reuse reference, not a replacement for these UMD contracts.

## Evidence

Builds and captures are under
`/private/tmp/t214-gpu-round/d3d-umd.2huxro_6/`.
`verified-build/manifest.json` records artifact hashes; `staged-hashes.json`
confirms the four files copied to the Switch. Results are retained under
`C:\ProgramData\SwitchWOA\GpuUpdate\d3d-umd-20261110a`.
All eight downloaded result files match their device hashes and lengths in
`verified-results.json`; `returned/results-summary.json` contains the compact
exit-code and installed-driver record.

| Artifact | SHA256 |
| --- | --- |
| `TegraGpuD3D.dll` | `0d52c9eec154d2134eb774e7705ee60cbd014a979e451a424d9f2a430c947c9b` |
| Adapter fixture | `8c48019e99ca689835b1616a8f9afc17a6b51635666af4c99e459a30db995fd8` |
| DLL entry fixture | `838704d70a2a5f8d79b969be648bf640e176f60addc6917604cafdaac4e98d92` |
| Device fixture | `d1297359af1c784394ee9db9c29c142c41b26bdd286cc3b61a7596d7b71bdeed` |
