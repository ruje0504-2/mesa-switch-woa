# Native D3D runtime exit-path investigation

2026-11-10, Switch Lite HDH-001, Windows 11 ARM64 23H2.

**The access violation remains unresolved.** .141 keeps .140's UMD, ICD
and KMD core and tests the original ComPtr scope-exit release order. All
three runs create a native D3D11 device at feature level 10_0 on GM20B LUID
`97642`, match 16,384 RGBA/BGRA pixels and report S_OK from
GetDeviceRemovedReason. Their process exit results differ:

| UTC | Probe | Process result |
| --- | --- | --- |
| 20:01:32–20:01:43 | Implicit release; old module-query exception reporter | `c0000005`, no timeout |
| 20:04:35–20:05:17 | Implicit release; raw-address reporter and module bases recorded before drawing | 0, no timeout |
| 20:06:36–20:06:50 | Same raw-address probe | 0, no timeout |

The normal exits do not establish a driver fix. The earlier .140 results
and rendering scope are in [D3D UMD results](D3D_UMD_RESULT.md).
Resource sharing and DWM presentation remain pending.

## What the exception record establishes

The first .141 run prints two access-violation headers, both for a read at
address `0x24`, before printing `ntdll+0x5d144` and LR `ntdll+0x5d0bc`.
The old reporter calls GetModuleHandleEx before printing an address, so
this record does not establish the original fault PC or identify a caller.
It may include a secondary fault during reporting.

The retrieved device `ntdll.dll` is version 10.0.22621.2860, SHA256
`dc52ae94b0a10af0dba3846ba536c90bfa5ff5afafa873e0de58d1f56116b5a3`.
Microsoft's symbol file has the image's GUID
`C41C3765-5AF0-0300-8A00-6793E97CB265`. Its section-relative native symbols
corroborate the PE exports RtlWaitOnAddress (`5d080`) and
RtlEnterCriticalSection (`30390`); the ARM64EC nested exports are different.
RtlpWaitOnCriticalSection starts at `5d098`, placing the recorded PC at
offset `ac`. The instruction there reads `[x10 + 0x24]`, consistent with
the reported null-base read. The LR is inside the same routine and does
not identify its caller. The downloaded PDB header age is 3 while the
image's RSDS age is 1; retain this distinction alongside the GUID and RVA
checks, rather than claiming an unqualified debugger symbol match.
[Microsoft symbol artifact](https://msdl.microsoft.com/download/symbols/ntdll.pdb/C41C37655AF003008A006793E97CB2651/ntdll.pdb)

The probe now records module bases while execution is normal and avoids
loader queries in its exception handler. It records raw PC/LR, access type,
fault address, thread ID, selected registers and a bounded ARM64 frame-pointer
chain. The chain is not a complete stack unwind. This keeps fault reporting
independent of module-list lookup during teardown; it does not change the
driver's locking or object lifetime.
[Microsoft DLL and loader-lock guidance](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices)

A controlled ARM64 child process on the .59 baseline reads a deliberately
inaccessible page at 20:14:04 UTC. It exits with the expected `c0000005`,
records exactly one exception, reports the injected address and retains
three frame-chain entries. That validates the revised diagnostic on this
device; it does not reproduce or fix the native runtime fault. The frame-chain
version has not yet captured that fault.

## Recovery and evidence

.59 Code 0, original driver/ACPI hashes, removed test tasks/permit and
temporary UMD/ICD registrations, and unchanged AC/DC display/sleep settings
were verified at 20:12:58 UTC. Firmware/Hekate and the 512 MiB GPU budget
were unchanged.

Artifacts: `/private/tmp/t214-gpu-round/runtime-ddi141.6f5gyzh6/`.
All 39 test records match device SHA256/lengths in `physical/verified.json`.
Eight recovery/control records match in `physical/verified-recovery.json`.
The ETL export has 140,854 records with matching count and boundaries.
The retained first-event journal is identical across the three runs; it
does not locate the later user-mode failure.

| Artifact | SHA256 |
| --- | --- |
| First native run, access violation | `88e8623282fc4f709933fd15419517d76b162645578fa17d619fe0d51d5fb3a8` |
| First raw-address run, exit 0 | `a7152846705ba4e0cebcfb4cf312ae0511dc5ea70b2fba08cb02e4a036fe776c` |
| Second raw-address run, exit 0 | `f3f7a12c0ad6c45ac36fc3bdead2c20e6ee9563192bd97a8a229df00ca3dbf64` |
| Controlled exception-capture executable | `afbf4d0a76308d2ad1712d6abf9f42b5ca536fe326a2c99633eee63a1485e727` |

Next, capture the original fault's frame chain and identify the caller and
synchronization object before changing the UMD/NVK teardown path.
