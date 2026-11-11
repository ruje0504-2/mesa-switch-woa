# First KMD recovery event

The `.153` diagnostic candidate adds provider
`{c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0}`, event **1**, version **1**,
level **2**, keyword **1**. It records the first transition to
`RecoveryPending`, after releasing engine/queue spinlocks and before
`DxgkCbSynchronizeExecution` requests GPU engine recovery. A notification
retry keeps the first status and does not emit a second record. A later reset
can start a new recovery episode.

The implementation accepts callers through DISPATCH_LEVEL and copies only
scalar fields. It performs no GPU access, allocation, disk write or command
pointer dereference. DriverEntry registers the provider and records its
`TraceStatus`; unload and failed initialization unregister it. The diagnostic
provider does not change submission admission, allocation limits or recovery
behavior.

The payload is a fixed 72-byte little-endian record:

| Byte offset | Type | Field |
| --- | --- | --- |
| 0, 4 | u32 each | Payload version, source |
| 8, 12 | u32 each | Adapter LUID low/high parts |
| 16 | u32 bits | Original NTSTATUS |
| 20 | u32 | Virtual-submit flags |
| 24 | u64 | Opaque context value |
| 32, 36, 40 | u32 each | DMA bytes, private bytes, UMD-private bytes |
| 44, 48, 52 | u32 each | Submission fence, last completed fence, graphics generation |
| 56, 60, 64, 68 | u32 each | Cached FIFO interrupt, MMU fault, PBDMA interrupt 0/1 |

Sources are `0=unspecified`, `1=submit_gate`, `2=submit`, `3=reject_drain`,
`4=preempt`, `5=worker`, `6=legacy_submit`. Submit metadata is zero for paths
without a virtual-submit argument. Cached register fields describe the last
cached channel state; they are not newly sampled hardware fault registers.
The original status and source identify the recovery trigger, not necessarily
its underlying cause.

Enable this GUID in the existing diagnostic session with keyword 1 and level
2 or higher. Retain the raw ETL as well as its `SWETW001` ProcessTrace export.
The decoder accepts that binary export (not ETL) and validates its event count,
lengths and schema before printing recovery records:

```sh
python3 tegra-x1-wddm/tools/decode_recovery.py dxg-start.etl.001.recovered.bin
```

ETW may lose buffers or leave the tail unflushed if execution stops. A circular
trace may also overwrite earlier records. No matching event does **not** prove
that no failure occurred. Check `TraceStatus`, provider enablement, export
identity, ETL finalization and loss counts with the existing evidence.

This event covers engine-recovery requests. A malformed virtual submission
returned as `STATUS_INVALID_PARAMETER` can instead put its calling device in
an error state without requesting engine recovery. The current provider does
not separately record that rejection. Check this path if device loss has no
corresponding recovery event; see Microsoft's
[SubmitCommandVirtual return contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual).

Windows ARM64 context/worker models verify first-status retention, duplicate
suppression, emission before OS notification, spinlock release, PASSIVE_LEVEL
submission failure and DISPATCH_LEVEL preemption failure. Decoder tests cover
fixed wire bytes and truncated/incompatible exports. These controls do not
prove live kernel ETW delivery. The later `sample-i153-native-20261111` trial
does retain five valid events on native LUID `b02db`, verifying live delivery.
They describe paging submissions returning `STATUS_DEVICE_NOT_READY` in
graphics generations 3 through 7. They follow the first observed device-error
mark and DWM's generic resource OOM, so none establishes the initial failure.
Zero cached register values do not establish healthy hardware. The native ETL
is unfinalized; its raw export and device hashes are retained.

OS-requested reset can also set `RecoveryPending` directly in
`isolate_backend`, without going through this notification event. Together
with the malformed-submit path above, that remains an attribution gap to
resolve before diagnosing the first device error from a later recovery.

The implementation follows Microsoft's
[EtwRegister](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwregister),
[EtwWrite](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwwrite)
and [EtwUnregister](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwunregister)
contracts.
