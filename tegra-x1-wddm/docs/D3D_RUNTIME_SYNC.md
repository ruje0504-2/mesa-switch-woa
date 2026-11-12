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
These controls alone do not establish native shared-image readback, sustained
composition or measured CPU/GPU timings. The subsequent [.160 trial](NATIVE_DESKTOP_160.md) was interrupted
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

The subsequent [import-content trial](NATIVE_DESKTOP_158.md#import-content-desktop-trial)
passes `--shared-layout-load` on native GM20B: all **3,686,400** BGRA/RGBA
pixels match across producer sharing, imported partial blend and consumer
sampling/placement, without intermediate CPU readbacks. Its ordinary draw and
sampling cases add 40,960 matching pixels. This supplies native D3D resource
and GPU-handoff evidence. It does not establish performance or five-minute
physical desktop acceptance; ShellExperienceHost submission failures in the
same trial still need attribution. DWM signal/wait callbacks and interval-one
Presents occur, but the ETL's reported event loss prevents complete-history
claims.

The later [onscreen-only trial](NATIVE_DESKTOP_158.md#onscreen-only-trial-physical-failure)
uses these same binaries and fails physically: the owner reports a frozen
image and automatic restart. It retains native identity and a short prefix
of successful DWM Presents, but no completed five-minute observation.
No offscreen probe is rerun in that trial. Its recovery is verified; the
earlier shared-image readback remains valid only for its own offscreen scope.

The native shared-image check uses `--shared-layout-load` first. The
separate `--shared-layout-checkpoints` diagnostic performs producer and consumer-import CPU
readbacks before sampling, inserting completion waits that can hide missing
GPU ordering. A checkpoint pass alone cannot validate the bridge. The
[imported-view audit](PORTING.md#imported-windows-image-views) also corrects
an attachment-usage mismatch in that tested UMD.
The [import-content fix](PORTING.md#imported-windows-image-contents) additionally
preserves existing pixels on the imported image's first render pass; the
load-preservation scene checks this while retaining GPU-only handoffs.

## Imported-resource teardown ordering

The source audit after the failed onscreen-only trial finds a separate,
reproducible callback-threading defect. `DestroyResource` previously waited
for GPU use, dropped the frontend's Gallium resource reference, and only then
closed `RuntimeResource`. Between the last two steps, Zink's batch worker
could drop the last imported object reference. NVK's VA destructor would
then call `free_gpu` while the runtime owner was still live, reaching
`pfnFreeGpuVirtualAddressCb` on the worker thread.

This frontend exposes the D3D10/10.1 DDI, without the D3D11 free-threaded
capability. Microsoft's [callback threading rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)
do not grant it the relaxed D3D11 callback model. The
[GPU-VA free contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_freegpuvirtualaddresscb)
also requires completion of GPU use before the synchronous unmap.

Main `1e6a3ba4d4cb603612d43e5af93f74fbea845e97` keeps the existing completion
wait, closes the runtime owner on the DDI thread, then releases the frontend's
backend reference. A delayed importer release sees the dead owner and only
removes retained bookkeeping. Ordinary Flush/Present submission and the
runtime/NVK GPU timelines are unchanged; no additional GPU wait is added.

The existing Present/identity control now calls the real `DestroyResource`
DDI and models three imported mappings after swapchain identity rotation.
Its backend deliberately releases the retained imports on a worker before
returning, making the previously possible interleaving deterministic.
Runtime callbacks assert the DDI thread; each mapping is freed once.

- Old production code fails at that thread assertion on Windows ARM64 at
  22:02:08 UTC, exit 1 without timeout. Control SHA256:
  `b8378b76dc4ac25a8517363bb2e49eaf683ac43d9070b2385f99645619b99f2f`.
- The fixed production code passes at 22:03:39. The frozen final control
  from the committed source passes again at 22:05:50, exit 0 in 100 ms:
  `f1695ede879a5062ba144997be54a131ec29fcc25d1aa34a662de23d346e88a1`.
- All runs use controlled runtime/Gallium calls, issue no GPU commands and
  leave `.59` / Code 0 in place. Nine returned records / 2,112 bytes match
  device hashes and lengths. Final result SHA256:
  `efefc77deee7c5658c9153e6c9043312b35804cace341850452edd6d0acaaa42`.

The ARM64 UMD compiles and links. The frozen candidate is
`desktop158-teardown-order-20261112-umd`, DLL SHA256
`7f8f821d7b3ca7f56b37bb47d7367e9a5db356bfcee3a838af8a75d81597e3ce`.
The subsequent [native trial](NATIVE_DESKTOP_158.md#teardown-order-umd-trial-physical-and-harness-failures)
reaches `.158` / Code 0 but the owner reports short output followed by
corruption and black output. An independent missing helper argument aborts
the timed worker before its pixel checks. This fixes the demonstrated control
interleaving; it is not a resolution of that physical failure. Native pixels
and five-minute physical acceptance for this candidate remain pending.
Control evidence is retained under
`runs/wddm-destroy-order-20261112/` in the external build workspace.

## Contracts and source

- [Create synchronization object callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_createsynchronizationobject2)
- [Share kernel objects as NT handles](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)
- [Runtime GPU signal callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_signalsynchronizationobjectfromgpucb)
- [Runtime callback threading rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)
- [UMD resource and synchronization bridge](../../src/gallium/frontends/d3d10umd/RuntimeMemory.cpp)
- [Zink batch submission](../../src/gallium/drivers/zink/zink_batch.c)
