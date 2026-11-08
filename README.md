# Tegra GPU backend

Native Windows ARM64 backend components for the GM20B in Switch Lite (HDH-001,
T214). Development and hardware evidence live in the single
[Vulkan status page](https://github.com/ruje0504-2/winx/blob/vulkan-test/docs/gpu-vulkan/STATUS.md).

The default build produces a kernel static library. `--probe` also builds a
System-class KMDF test driver and its SwitchFan PMIC bridge. These are **not**
a display miniport or Vulkan ICD. They keep the working POST display driver.

Build with LLVM and the official Windows WDK/SDK headers. `WDK_INCLUDE` and
`SDK_INCLUDE` denote their versioned Include directories; the script targets
ARM64 and the Windows 11 22H2 baseline, using WDDM 2.4 private-memory
callbacks. From the repository root:

```sh
git clone --depth 1 https://github.com/danfromtico/mesa-switch.git build/mesa-switch
git -C build/mesa-switch checkout 0ff52079ca9e9e837ed27eb95a8d239f439a1864
python3 drivers/TegraGpu/tools/build.py \
  --mesa build/mesa-switch \
  --wdk-include "$WDK_INCLUDE" --sdk-include "$SDK_INCLUDE" \
  --out build/tegra-gpu --probe
```

`TegraGpuCore.lib` contains WDDM resource/private-memory helpers, GM20B 4 KiB
page-entry and GPFIFO encoders, the unchanged mesa-switch report encoder, and
T214 power/clock control. The default target is **729.6 MHz**, with an opt-in
**1228.8 MHz** profile. C1 NA PLL calibration and the MAX77812 M1 voltage follow
the conservative Switchroot B01 CVB table and are checked by a GPCCLK counter.
OUT5 at 204 MHz is a separate power/reference clock. The KMDF probe uses a
private uncached arena, four mapped GPU pages and one channel to release two
FIFO semaphores with different values. It powers down afterward; it does not
load graphics firmware or execute shaders. The future WDDM adapter uses the
separate Dxgkrnl private-memory helpers; VidMm/scheduler integration is open.
The diagnostic requires exclusive cold GPU ownership, resets/enables PFIFO
before reading or initializing its channel slot, and preserves init-register
snapshots in the existing status response. Do not interpret a reset CCSR slot
as a live channel. The probe separately enables PBDMA0 through `MC_ENABLE_PB`,
checks its runlist-zero mapping, and disables it before the final FIFO reset.
Runlist zero is blocked during binding, explicitly allowed after submission,
and blocked again before teardown, following Switchroot's scheduler control.
The diagnostic and WDDM encoder use Switchroot's complete single-channel TSG
layout: a TSG-zero header (one member, timeslice 128 at scale 3), followed by
CHID zero and a reserved zero word. The RAMFC address is supplied through
CCSR. Submission counts both entries; WDDM retirement preempts TSG zero.
Copying `gk20a_get_ch_runlist_entry` alone had omitted the group header emitted
by `gk20a_runlist_construct_locked`. Switchroot's channel setup requires TSG
membership. This closes that source mismatch; physical scheduling is still
unverified. Local PBDMA/FIFO fault
enables follow Switchroot while the MC CPU interrupt outputs stay disabled.
Bind, scheduler and channel-switch interrupts stop submission as faults;
ordinary runlist events do not. Scheduler error is read from `0x254c`, not
the distinct runlist-fault register at `0x262c`. The status includes binding,
engine, committed-runlist and valid PBDMA state. An enabled or pending channel
does not prove execution. `--probe --keep-fan`
rebuilds only the GPU probe when the matching SwitchFan bridge is verified.

The diagnostic starts with empty saved RAMFC and USERD `GP_PUT`, then publishes
the first entry through BAR1 after binding and scheduling. It publishes the
second only after the first GPU-written fence completes. It does not assume
that a preloaded RAMFC value substitutes for a submission notification.
A two-way BAR1 visibility check restores its scratch word before enabling
snoop. A posted BAR1 write can be visible through the GPU alias while CPU
RAM still contains the old value. The probe orders FB writes, flushes dirty
L2 lines to DRAM, and invalidates clean lines before requiring CPU visibility.
It polls GPU fences through BAR1, then requires CPU readback after the same
writeback sequence. Neither alias is CPU-assigned an expected fence value.
The BAR1 scratch check is never treated as a GPU fence. Live PBDMA
GP registers are sampled only for valid channel zero or its single-channel TSG zero;
idle/context-switch SRAM readouts are not interpreted as execution progress.

The cold path now follows Switchroot's bus/private-ring, clock, FIFO reset,
LTC routing, FB setup and BAR1/FB-flush order. Idle slowdown stays disabled
through execution and is restored on clock release. FIFO/FB/LTC use the
upstream force-run clock-gating profiles until a PMU governor exists. LTC
active counts feed both L2 and FBHUB; physical-security setup is restricted
to the upstream non-secure branch. Two bounded FB flushes must finish before
the channel becomes runnable. ABI 9 (236 bytes) records the initialization
stage, LTC/flush counts, BUS/PRI faults, FIFO binding/engine state and the
separate BAR1 step/expected/GPU/CPU values. Bounded L2 writeback and invalidation
timeouts stop submission; a successful FB flush alone is not DRAM visibility.
This is
a local cold-start candidate, not a hardware execution pass. Source reference:
[Switchroot NVIDIA driver](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/tree/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu).

The diagnostic accepts only owned physical buffers below 4 GiB. Switchroot's
GM20B path tags IOMMU addresses with bit 34 and leaves physical addresses
untagged. The probe therefore leaves Tegra's global GPU/GPUB ASID registers
unchanged; a nonzero ASID readback is not a failure for this physical path.
This does not provide a Windows IOMMU domain or authorize arbitrary DMA.

The GM20B platform leaves `honors_aperture` false, so Switchroot resolves
`nvgpu_aperture_mask` to VIDMEM even for SoC DRAM. Version 0.1.0.14 applies
that policy to the full chain: PTE target 0, small PDE target 1 plus VOL,
volatile PDB target 0, USERD, CCSR, runlist, BAR1 binding, MMU debug buffers
and TLB invalidation. Copying only the helper’s SYS_NCOH argument was
incorrect. This source correction does not establish why ABI 8 stalled or
prove a GPU execution pass. [Platform policy](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/blob/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu/common/mm/nvgpu_mem.c)

`windows/allocation.c` implements the real-WDK create/destroy/open/close
allocation callbacks. ABI 1 describes opaque GPU storage with a byte count and
optional CPU visibility; it carries no physical address. The adapter exposes
one explicit page-table segment, so ordinary buffers select VidMm's appended
implicit system-memory segment (ID 2). VidMm owns backing pages and GPU VA;
the KMD allocates only metadata, not a second heap or global handle table.
Sizes are rounded to 4 KiB, CPU-visible memory starts write-combined, and no
physical-access, cached/coherent, protected or scanout capability is implied.
Bulk validation/allocation failures unwind every new object while preserving
an existing resource. OS-reserved allocation input flags are retained.
OpenAllocation resolves D3DKMT handles through `DxgkCbAcquireHandleData` and
holds the OS reference until CloseAllocation. Allocation metadata belongs to
the adapter, not its creating device, allowing a resource to outlive that
device. Close drops local references before `DxgkCbReleaseHandleData`, which
may immediately destroy the allocation; the adapter operation stays retained
through that callback. A range helper checks device ownership, read-only opens
and bounds, but does not establish residency or validate a command stream.
Native ARM64 tests cover batch rollback, resource append failure, shared opens,
read-only/bounds rejection, duplicate close/destroy rejection, and synchronous
destruction from the final OS release. The callbacks populate the initialization
table but still require the complete adapter/UMD path and live VidMm validation.
[Microsoft GPUVA allocation example](https://github.com/microsoft/graphics-driver-samples/blob/de4a2161991eda254013da6c18226f5ea06e4a9c/compute-only-sample/coskmd/CosKmdAdapter.cpp)
and [handle lifetime contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkcb_acquirehandledata).

`windows/paging.c` translates real WDK `DXGK_PTE` entries into GM20B entries
for an uncached, idle address space. It validates the entire update before
publishing, supports repeat/invalid entries, and rejects unsupported flags.
The resolver must pin and validate GPU mappings. GM20B's 128 KiB tables need
a dedicated nonzero CPU-visible segment; WDDM limits segment-zero tables to
4 KiB. `windows/table_segment.c` provides the contiguous segment backing,
descriptor, bounded byte-offset resolver and uncached CPU mapping. It uses
WDDM 2.4 allocation callbacks with physical bounds of 2–4 GiB, leaves bit 34
clear, reports `PopulatedFromSystemMemory` for correct accounting, and
publishes no cache-coherence or sleep-retention capability.
The paging builder now uses its production `TgGpuResolveVidMmPage` resolver:
PTE segment zero is a resident physical byte address supplied by VidMm, while
the nonzero table segment uses offsets. The implicit allocation preference
ID 2 is not a PTE segment-zero substitute. Ordinary physical pages above
4 GiB are preserved; addresses carrying Tegra's bit-34 IOMMU tag are rejected
by this WDDM 2.4 physical path. The resolver does not pin memory or translate
logical DMA addresses. Native tests mix system and table pages in one queued
update, retain high address bits/read-only flags, and reject a bad trailing
page before emitting a partial DMA buffer.
VidMm remains responsible for allocations within the segment; there is no
second suballocator. The adapter must finish GPU work/drain/reset and stop
paging before destroying the segment. Failed frees retain the tracking handle.
Do not use the pool's own CPU VA to validate a different VidMm CPU mapping.
WDDM callback wiring and physical use of this segment remain open.
The CPU-update helper is valid for initialization/test-owned mappings; do not
advertise `CPU_VIRTUAL` updates for the dedicated memory segment without
resolving the documented local-segment restriction.

`windows/paging_buffer.c` builds bounded, private kernel records for
`UPDATE_PAGE_TABLE` and `FLUSH_TLB`. GPU-physical updates snapshot translated
entries without writing the table, support DMA-buffer multipass, and retain
no borrowed PTE array. The mandatory NULL-DMA CPU initialization path checks
every page of VidMm's uncached alias against the owned table backing.
At ordered submission, the worker helper publishes uncached entries before
real TLB MMIO. It rejects an invalid trailing record before any publication.
A TLB error returns incomplete progress, never permission to complete the
paging fence or recycle old pages. The adapter still has to supply residency,
address-space serialization, submission/DDI wiring, and fence/fault handling;
this library is not yet an advertised WDDM paging mode.

`windows/root_table.c` consumes real WDK root-table arguments. It rounds root
sizes to whole CPU pages, resolves the complete segment span, writes the RAMIN
PDB and address limit only for an idle context, and propagates real TLB errors.
A failed root move clears the software binding's valid flag, so the adapter
must not reschedule it with the old root. Flushes belong at the ordered paging
submission point, with old pages retained through completion/reset; they must
not run early from `BuildPagingBuffer`. The context DDIs below now consume
these helpers. The channel worker below implements hardware binding; complete
miniport registration and physical validation remain unfinished.
`platform/t214_mmu.c` performs the Switchroot TLB invalidate sequence and
reports timeout/read failures. Add `--windows-tests` to build `test-paging.exe`
against the real WDK types, then run it in ARM64 Windows/WinPE. This test uses
owned test memory and controlled MMIO/OS callbacks, and does not access a GPU.
It covers deferred/initial updates, multipass, alias rejection, PTE-before-TLB
ordering and incomplete progress after TLB failure, as well as segment
bounds, real-WDK callback shapes, allocation failure and cleanup retry with
controlled OS-call substitutes; it does not test real Dxgkrnl allocation.

`windows/query.c` supplies the memory-related `QueryAdapterInfo` requests:
`QUERYSEGMENT4`, `GPUMMUCAPS`, `PAGETABLELEVELDESC` and `PHYSICALADAPTERCAPS`.
It reports the existing table segment's actual backing, two 128 KiB table levels
covering a 40-bit GPU VA, and physical-mode ordered paging. The first segment
query changes only the count; the descriptor query preserves OS extension bytes.
Paging and context buffers share one size/private-record contract. These queries
claim no coherent memory, system IOMMU, large pages, standby retention or display
capabilities. `DRIVERCAPS` and private UMD queries remain unsupported until the
submission, recovery and UMD contracts are integrated; this component alone
cannot start a WDDM adapter. Native ARM64 tests exercise the real WDK callback,
including count-only queries, descriptor strides and invalid adapter/level inputs.

`windows/context.c` supplies the process/device/context creation and destruction
DDIs, root-table DDIs, completed-fence query and DPC callback. Its registration
helper fills those entries in the caller's initialization table; it does not
register a display adapter alone. Each context owns an uncached RAMIN page
allocated through Dxgkrnl. Parent references prevent teardown during pending
work; failed frees preserve the context for retry. Software contexts are
dynamically allocated, while hardware work uses one serialized execution slot.
Root changes cannot rewrite an active instance, and failed changes prevent
future submission with a stale binding.

The submission-worker helpers capture a bounded context/root snapshot separately
from activation. Only activation writes the complement of the private fence
marker, so admitting later work cannot overwrite the active marker. Before
notification, the GPU marker must match and the channel must be quiescent; the
context reference is then released. `DxgkCbSynchronizeExecution` delivers
`DxgkCbNotifyInterrupt` and the DPC notification using an engine-only snapshot,
so context destruction after completion cannot race a remaining context access.
A synchronization failure keeps that notification record for retry/reset and
prevents later fences overtaking it. Aborted work never advances the completed
fence. The virtual submission DDI, command validation, VidMm residency and
fault/TDR integration remain to be connected.
`--windows-tests` also builds `test-context.exe`; it calls the registered WDK
function pointers using explicit allocation, interrupt and GPU-write substitutes.
It checks lifetime, failed root changes, pending/duplicate notification, retained
references and failed-free retry, not live Dxgkrnl or GPU execution.

`windows/work_queue.c` admits multiple contexts in order using the nonpaged
private buffer supplied by Dxgkrnl for each DMA buffer. Context creation reports
the required private-data size; this initial queue accepts no UMD-private payload
or specialized submission flags. Admission allocates nothing and holds each
context/root reference. The single worker copies the head record into its own
storage, activates and submits it, polls its GPU marker, quiesces the channel,
then notifies completion. It never accesses the borrowed record afterward, even
if Windows immediately reuses it. Pending work does not touch shared RAMIN or
command/fence memory. A failed operation stops the queue; reset retains affected
references until isolation succeeds, then cancels pending work without reporting
its fences complete. Admission resumes only explicitly after recovery. Wake is
a synchronization event for the future adapter worker, which must wait on an IRQ
or timer while the active result is pending rather than busy-looping.
The queue and channel helpers are compiled backend components, not an exposed
`SubmitCommandVirtual` DDI: adapter dispatch, malformed-buffer fence handling,
command/residency validation and preemption/TDR notification still need wiring.
Native tests cover multiple roots, ordering, immediate private-buffer reuse,
partial submission/reset failure, restart admission and canceled notifications.
[Dxgkrnl private-buffer contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_submitcommandvirtual)

`windows/submit_worker.c` connects prepared work to `core/channel.c` after
exclusive cold initialization. The worker blocks scheduling, invalidates the
root TLB, binds RAMIN, enables the channel and submits the physical runlist
before allowing execution. Both ring entries are preloaded; this hot path
does not use the diagnostic probe's second BAR1 doorbell. Queuing alone never
notifies a completed fence. The work reference is released only after bounded
disable/preempt/runlist removal, unbind, FB flush and TLB invalidation succeed.
A timeout retains RAMIN, roots and commands. Recovery disables fetch, drains
MC, restores clocks and powers down; clamp/reset readback must confirm isolation
before releasing failed work. No caller-supplied "hardware retired" boolean
can bypass these conditions. Cold reinitialization is required after recovery.
MMIO/waits run outside the object spin lock, with an active reference held.
Native ARM64 tests exercise submission order, duplicate rejection, preempt
timeout/retry, failed reset acknowledgement and cancellation without MMIO using
controlled hardware substitutes. Physical execution and privilege enforcement
remain unverified. These are worker helpers, not a registered
[`SubmitCommandVirtual` DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual):
its allowed return values differ from the worker's internal failure statuses.

`windows/internal_va.c` reserves one leaf-table coverage range (64 MiB) through
`DxgkCbReserveGpuVirtualAddressRange` inside process creation, with user-mode
mapping disabled. A private 128 KiB leaf maps only the engine's command ring,
completion packet and USERD/fence pages, all using GM20B privileged PTEs. Ring
and packet mappings are read-only; RAMIN and the physical runlist are not mapped.
VidMm owns the reservation lifetime, while the driver owns the leaf and shared
command backing. The reserved PDE is restored before context preparation because
VidMm may invalidate it when the root becomes resident again. Hardware submission
must invalidate the TLB after these updates.

`core/submission.c` prepares an application GPFIFO entry with USER privilege,
followed by a KMD-generated KERNEL semaphore release with WFI enabled. Its fixed
command buffer is serialized across contexts; it is not a general VA allocator.
Preparation preserves RAMIN's PDB/VA limit and initializes only the complement
of the fence value. The context helper rejects application command ranges that
overlap the reservation. Native tests check the callback contract, PTE/packet
encoding, restored reserved PDE and separate fence backing. The channel worker
consumes this layout, but no physical submission or privilege-enforcement pass
is claimed. Workload barriers, IRQ/TDR and adapter dispatch remain required.

The existing `LitePerformanceCheck.exe` is the UI for both clock profiles and
the combined readback check. The optional probe requires matching firmware
resources and the `SWITCH_LITE_GPU` build of SwitchFan. The build emits unsigned
SYS/INF files; generate Windows catalogs and sign the final files before
installation. Host tests and compilation do not establish a hardware pass.
Use a **CA:FALSE code-signing leaf** issued by the trusted development root;
do not sign SYS/CAT files with the CA certificate itself. Before installation,
verify the SYS signature and the INF/SYS catalog membership with Windows
SignTool (`verify /pa /c package.cat file`). A successful PnPUtil import alone
does not establish a valid Authenticode signature or an active device binding.
The probe/bridge linker uses the working T214 kernel image profile: resident
code/data/unwind sections, discardable INIT, and `/tsaware:no`. The default
image profile compiled and signed but failed native driver loading with
Win32 error 487. The corrected images load in ARM64 WinPE with test signing;
that loader check does not exercise the Lite's ACPI resources or GPU.

The firmware consumes `platform/t214_resources.h` through its WindowsDrivers
submodule. Build it with `-D SWITCH_LITE_GPU=1` to expose the separate ACPI
render node. Keep the existing POST display path during bring-up.
