# Cached KMD debug report

`DxgkDdiCollectDbgInfo` previously copied only the last completed fence (four
bytes). After the failed desktop trials, that left no driver-specific queue
or recovery context in a system TDR report, even if SD dump capture worked.
The callback now emits a versioned **TGDB** record from resident cached data.
This improves evidence collection; it does not establish the GPU fault or
change rendering, completion, reset or presentation behavior.

Microsoft permits the callback at
[undefined/high IRQL and synchronization level zero](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_collectdbginfo).
It can interrupt a thread holding the driver locks. The implementation performs
no lock acquisition, retry loop, allocation, ETW write, queue/context pointer
dereference or GPU register access. The existing execution/recovery collectors
publish while holding `Engine.Lock`. Independently sequenced atomic words
prevent a partial payload being accepted: an odd or changed sequence returns
an unavailable flag immediately. Recovery and execution snapshots are from
separate earlier boundaries, not an atomic pair or a live register snapshot.
The header's last completed fence is an independent current observation.

## Wire format

All fields are little endian. The full report is 256 bytes; the callback
copies at most the OS-provided `BufferSize` and never claims a complete record
that does not fit. Capacities below the 32-byte header fail without writing.

| Offset / bytes | Contents |
| --- | --- |
| 0 / 32 | Eight u32 fields: `TGDB` signature (`0x42444754`), version 1, full report bytes, written bytes, OS reason, flags, last completed fence, reserved zero. |
| 32 / 72 | `TG_GPU_RECOVERY_EVENT`: most recently published first internal recovery request, with source, status, submission/completion fence, generation and cached error fields. |
| 104 / 152 | `TG_GPU_EXECUTION_EVENT`: latest preemption/failure/reset boundary, including stage, expected/observed fence, poll count, queue/work state and cached observations. |

Flags: 1 recovery valid, 2 execution valid, 4 output truncated, 8 recovery
publication busy/changed, 16 execution publication busy/changed. An absent
valid flag is not a successful GPU result. A never-published record is zero;
a publication interrupted during collection is also zero with its busy flag.
Cached opaque context/VA identities are not dereferenced.

Extract the driver's TGDB payload from the system report, then run:

```sh
python3 tegra-x1-wddm/tools/decode_debug_report.py extracted-tgdb.bin
```

The decoder consumes that payload, not a complete Windows dump. It validates
the header, sizes and flags, and uses the existing ETW payload layouts and
source/stage names. It does not invent timestamps or imply ETW delivery.

## Validation and remaining evidence

The actual cache/report helpers pass host ASan/UBSan controls for an interrupted
writer, an update during a read, sequence wrap, every buffer capacity around
the header/full-record boundaries, unaligned output and guard bytes. The
decoder accepts a block written by the actual C helper and rejects missing or
contradictory records. The build entry point runs these checks.

The actual WDK callback control compiles for ARM64. It calls the registered
DDI with simulated `HIGH_LEVEL`, held engine/queue locks and unusable private
owner pointers; it checks no hardware/model allocation side effects, exact
buffer bounds, cached trace publication and unavailable records. This control
has **not yet executed on Windows**: the separate user-authorized SD test
currently owns the hardware window.

The full host backend suite and ARM64 kernel build pass. The local unsigned
`.161` package is built from current `main`, including its earlier GR
helper-load, queue-validation and trace changes; it is not the frozen `.158`
KMD plus diagnostics alone. SYS SHA256 before signing:
`9e7642914ccdace2b606504d46675763d2e37621a588fd6ee82e69f625d6da9f`.
The ARM64 CPU control SHA256 is
`41dd36f8530d0364565f49cf59224989616d15a71d94488646884b555d006027`.
Neither native KMD loading nor an actual crash-report callback/capture has
been validated for this candidate. The `.59` recovery baseline remains
selected; no native GPU deployment was performed during this change.

External build/evidence directory: `runs/debug-report161-20261113/`.
