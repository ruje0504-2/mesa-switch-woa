# Runtime and NVK submission ordering

The Zink desktop UMD submits Vulkan work on NVK's KMT context, while the
Direct3D runtime uses a separate, runtime-owned context for implicit resource
dependencies and DXGI presentation. Completing NVK work on the CPU before
`Present` orders only the outgoing side and serializes the application.

The November 12 implementation connects both contexts with two NT-shared
monitored fences, imported into Zink as D3D12 timeline semaphores. It uses
the Win32 NVK semaphore support introduced by `7a771aa6`. The UMD and ICD
must therefore be deployed together; the old `.158` ICD is not sufficient.

## Submission contract

For every Zink batch, including internally triggered flushes:

1. On the DDI caller's thread, queue a runtime-context signal of the incoming
   fence at a new value. This follows any dependencies the runtime already
   placed on that context.
2. NVK waits for that value before the batch's unsynchronized, reordered and
   ordinary command buffers, then signals the outgoing fence.
3. After `vkQueueSubmit` returns successfully, the caller queues a runtime
   GPU wait for the outgoing value. Later runtime work, including Present
   and implicit resource signals, follows that wait.

The caller waits for submission to return, not for GPU completion. Runtime
callbacks never run on Zink's submission worker. A threaded Gallium context
cannot install this bridge; this D3D frontend creates an unthreaded pipe
context and can still use Zink's Vulkan submission worker.

The internal Zink completion signal follows all external user signals.
Batch cleanup consequently cannot drop the imported semaphore references
before their signal operations finish. Context and batch references retain
the imported objects; temporary NT export handles are closed after import.
The runtime's opaque device/context handles are never cast to KMT handles.
`D3DKMTShareObjects` receives only the documented kernel synchronization
handle returned by `pfnCreateSynchronizationObject2Cb`.

Ordinary Flush, Present and resource-identity rotation use this GPU ordering.
Resource/device destruction and display-mode setup retain CPU completion
where releasing mappings or allocations requires it. Failed submission
does not queue a runtime wait for an unsignalled value. If a frontend callback
fails after Vulkan accepted work, the error path drains that work before
marking the context lost and allowing teardown.

This does not enable Direct Flip or change the 512 MiB budget, CPU frequency,
fan, sleep settings or presentation cadence.

## Validation scope

Both the ARM64 UMD and Windows x64 control binaries compile and link. On the
VPS, Wine x64 executes the actual production functions with substituted
runtime/KMT/Vulkan calls:

| Control | Coverage |
| --- | --- |
| `test_d3d10_runtime_sync.exe` | Fence creation/import/closure, both runtime callback directions, no CPU finish on ordinary flush, destruction drain, creation/import/registration/submit/wait failures and cleanup |
| `test-zink-win32-sync.exe` | Actual `zink_end_batch` and queue submission, all three command-buffer classes, callbacks on the DDI thread with synchronous and worker submission, external signals before internal completion, failed submission/callback handling, retained semaphore lifetime and the ordinary no-bridge path |
| Existing runtime Present/primary/shared controls | Existing ownership, resource-identity rotation, allocation/paging and cleanup behavior |

The controls use explicit checks in release builds and real process exit
codes. Injected semaphore-allocation and queue-submit errors produce expected
Mesa error messages; they are not observed device failures. Original logs,
failed fixture evidence and the corrected final results are retained under
`build/runtime-submission-sync-20261112/` in the local working copy.

These are CPU/API control results, **not GM20B execution or DWM acceptance**.
At 14:42:07 UTC the same five controls also complete on the physical Windows
ARM64 system, all with exit 0 and no timeout, while GPU `.59` remains Code 0.
The new runtime bridge control takes 108 ms (SHA256
`441e363ce4599a08a682be44c6ac27ee919923ed4c19883bebba6fc64ce283c1`),
and the Zink batch control takes 83 ms (SHA256
`59fab065eaa04ccbef08df8fa2905032f9c9f71b4dd4262facdfd43740ec5024`).
They still substitute the KMT/Vulkan calls and execute no GPU commands.
CPU readback remains 1,996,800 kHz and fan driver version 12.
The new bridge still requires Windows ARM64 runtime callbacks on the device,
native shared-image readback, sustained composition and measured CPU/GPU
timings. The subsequent [.160 trial](NATIVE_DESKTOP_160.md) was interrupted
without usable native results and restored to `.59`. No performance
improvement or visual fix is claimed yet.

The subsequent [matched-component `.158` retest](NATIVE_DESKTOP_158.md#matched-component-retest-failed-onscreen-acceptance)
also fails onscreen acceptance. It retains a short Code 0/startup-complete
record but no native pixel or desktop-capture result; the owner reports no
successful native output. The retained UMD trace includes successful runtime
signal/wait callback returns. Callback acceptance does not establish GPU
completion, cross-context ordering or a rendered frame. The device is restored
to `.59` with the trial tasks removed.

The separate [native Vulkan timeline test](VULKAN_WIN32_MEMORY.md#native-gpu-timeline-result)
now passes OPAQUE_WIN32 and D3D12_FENCE handoffs between two Vulkan devices,
with no intermediate host wait. This validates the underlying KMT/NVK path;
it does not exercise runtime-owned D3D contexts or Zink's imported WDDM images.

The next native shared-image check uses `--shared-layout-load` first. The
separate `--shared-layout-checkpoints` diagnostic performs producer and consumer-import CPU
readbacks before sampling, inserting completion waits that can hide missing
GPU ordering. A checkpoint pass alone cannot validate the bridge. The
[imported-view audit](PORTING.md#imported-windows-image-views) also corrects
an attachment-usage mismatch before the next onscreen trial.
The [import-content fix](PORTING.md#imported-windows-image-contents) additionally
preserves existing pixels on the imported image's first render pass; the
load-preservation scene checks this while retaining GPU-only handoffs.

## Contracts and source

- [Create synchronization object callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_createsynchronizationobject2)
- [Share kernel objects as NT handles](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)
- [Runtime GPU signal callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_signalsynchronizationobjectfromgpucb)
- [Runtime callback threading rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)
- [UMD resource and synchronization bridge](../../src/gallium/frontends/d3d10umd/RuntimeMemory.cpp)
- [Zink batch submission](../../src/gallium/drivers/zink/zink_batch.c)
