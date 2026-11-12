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
not separately record that rejection in `.153`/`.154`. Check this path if device loss has no
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
with the malformed-submit path above, that is an attribution gap in those
packages; event 2 below records these boundaries before state is cleared.

The implementation follows Microsoft's
[EtwRegister](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwregister),
[EtwWrite](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwwrite)
and [EtwUnregister](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwunregister)
contracts.

## Execution state in .155

The same provider adds event **2**, version **1**, informational level **4**,
keyword **1**. Enable this provider at level **4 or higher** to receive it;
the earlier level-2 boot-trace setup receives only event 1. It emits after
preemption admission, on submit/worker failure, and at OS reset entry/exit.
Successful preemption records are observations, not failure reports. Worker
failure is emitted before releasing the dispatch gate, so a later admission's
device-not-ready status cannot replace that record. Event 1 remains unchanged.

Each record copies scalar state under queue/engine locks and emits after
releasing both. It performs no additional GPU access, allocation or disk I/O.
The current work's polling path saves the fence and fault values it already
reads. The worker publishes its phase around mapping, activation, preparation,
submission, fence checks, retirement, software/paging work and notification.
Current-work clearing also takes the queue lock so the snapshot cannot race
that clear. The internal DMA-private size continues to be negotiated from the
updated structure size; the UMD/ICD interfaces and 512-MiB budget are unchanged.

The fixed little-endian payload is 152 bytes (`<8I3Q24I`):

| Offsets | Values in order |
| --- | --- |
| 0–28, u32 | Version, source, LUID low/high, status, request fence, request flags, stage |
| 32, 40, 48, u64 | Request context, current context, current DMA address |
| 56–68, u32 | Current submission fence, last completed fence, expected marker, last observed marker |
| 72–84, u32 | Current DMA bytes, work flags, queue flags, queue NTSTATUS |
| 88–100, u32 | Graphics generation, channel stage, last channel register/value |
| 104–132, u32 | Cached FIFO/MMU/PBDMA0/PBDMA1, MC, GR, FECS error/host interrupt |
| 136–148, u32 | Poll count, request DMA bytes, request private bytes, request UMD-private bytes |

Sources are `1=preempt`, `2=worker_failure`, `3=submit_failure`, `4=reset_begin`
and `5=reset_end`. Request fence means the preemption ID for source 1 and the
submitted fence for source 3. Reset/worker records leave request fields zero.
The decoder names stages and expands work/queue bits into `work_state` and
`queue_state`; raw masks and addresses remain available. It counts execution
events separately from first-recovery events and still accepts old exports.

`observed_valid` marks an actual read; `observed_cpu` distinguishes the retired
CPU readback from the active BAR1 GPU fence. That observed value can precede a
later fault-only poll. Poll count zero means no completed observation. `mc_valid`
and `gr_valid` distinguish reads from zero-filled unsampled fields. Channel
stage/register/value are copied only with `channel_fields_valid`: a helper
may be writing them outside the engine lock while `hardware_busy` is set.
The published worker phase still identifies that helper. No raw command buffer
or referenced pointer is dereferenced for tracing.

The preemption event captures state after admission; concurrently completed
work may already be absent. Neither that absence nor a cached zero establishes
healthy hardware. ETW flush/loss and interrupted-tail limits still apply.
Use reset-entry records before reset-exit/generation changes to find the
original work, rather than diagnosing it from later paging recovery requests.

Host wire-format/decoder checks and the full WDK build pass. Windows ARM64
context controls on `.59` verify DISPATCH-level preemption metadata, actual
uncompleted fence values, no additional GPU access, lock release before ETW,
channel-field suppression during a busy helper, rejected-submit metadata and
worker-failure emission while the dispatch gate is still held. The corrected
context fixture exits 0 at 01:32:25 UTC on November 12; reset controls exit 0
at 01:30:38 and verify paired entry/exit records for success and failure.
An initial new assertion incorrectly expected fence zero; the existing model
initializes the complement of the expected marker. That test expectation is
corrected without changing the driver's completion condition. Its failed
executable/result remain in the external evidence.

These are user-mode OS/GPU substitutes, not native ETW delivery or a physical
GPU pass. Evidence is in `execution155-20261112`; live validation is pending.
