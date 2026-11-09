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
callbacks. `GR_FIRMWARE` is a local directory containing NVIDIA's external
`NETB_img.bin`, `fecs.bin` and `gpccs.bin` for `tegra21x`/`gm20b`. The files
are supplied by the operator, not stored in this repository. `ACR_FIRMWARE`
contains the external NVIDIA 32.7.6 `nv_acr_ucode_prod.bin`, `pmu_bl.bin`,
`gpmu_ucode_image.bin`, `gpmu_ucode_desc.bin`, `pmu_sig.bin` and `fecs_sig.bin`.
Both sets are required for the secure T214 probe. From the repository root:

```sh
git clone --depth 1 https://github.com/danfromtico/mesa-switch.git build/mesa-switch
git -C build/mesa-switch checkout 0ff52079ca9e9e837ed27eb95a8d239f439a1864
python3 drivers/TegraGpu/tools/build.py \
  --mesa build/mesa-switch \
  --wdk-include "$WDK_INCLUDE" --sdk-include "$SDK_INCLUDE" \
  --out build/tegra-gpu --probe --gr-firmware "$GR_FIRMWARE" \
  --acr-firmware "$ACR_FIRMWARE"
```

`TegraGpuCore.lib` contains WDDM resource/private-memory helpers, GM20B 4 KiB
page-entry and GPFIFO encoders, the unchanged mesa-switch report encoder, and
T214 power/clock control. The default target is **729.6 MHz**, with an opt-in
**1228.8 MHz** profile. C1 NA PLL calibration and the MAX77812 M1 voltage follow
the conservative Switchroot B01 CVB table and are checked by a GPCCLK counter.
OUT5 at 204 MHz is a separate power/reference clock. The KMDF probe uses a
private uncached arena, bounded read-only firmware mappings and one channel
to release two FIFO semaphores with different values. On non-secure hardware it
loads GR before channel binding. Secure T214 instead runs the signed ACR
bootstrap, starts the authenticated PMU RTOS, waits for its WPR reply, then
starts secure FECS and non-secure GPCCS before binding the channel.
Both paths power down afterward. This does not
create a graphics context or execute shaders. The future WDDM adapter uses the
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
The shared cold-probe/hot-channel fault poll W1C-acknowledges a lone PBDMA
`ENGINE` event (bit 24), then its FIFO summary, following the GM20B ISR. The
upstream fatal/restartable masks do not include that event; treating every raw
PBDMA bit as fatal had stopped 0.1.0.15 before publishing GP_PUT. After the
acknowledgement all fault sources are re-read. Sticky events, concurrent true
faults, unknown bits and new faults arriving during the acknowledgement still
stop submission. Nothing writes an expected completion value or converts an
unchanged sentinel to a pass. The physical result is tracked in the status page.
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
the channel becomes runnable. ABI 16 (504 bytes) records the initialization
stage, LTC/flush counts, BUS/PRI faults, FIFO binding/engine state and the
separate BAR1 step/expected/GPU/CPU values. Bounded L2 writeback and invalidation
timeouts stop submission; a successful FB flush alone is not DRAM visibility.
After FECS reports ready, its temporary DMA context is invalidated, the watchdog
is configured, and image/zcull/PM size queries initialize the firmware RAM chains.
The reported sizes are not allocated graphics contexts or shader execution.
The GPC MMU then inherits the FB policy/debug-page addresses and active LTC count
using the GM20B setup sequence. This routing step does not create a golden context.
This is
a local cold-start candidate, not a hardware execution pass. Source reference:
[Switchroot NVIDIA driver](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/tree/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu).

Before channel binding, the cold probe now also performs the GR reset/access
preamble from `gk20a_init_gr_prepare`: release PGRAPH/BLG/PERFMON while preserving
FIFO and memory enables, apply the published GM20B GR force-run gate profiles,
set both engine clocks to RUN with power AUTO, and enable GR FIFO/semaphore
access. It records MC enable, GR access and both gate controls. Stage 307
identifies failure in this preamble; frontend stage 11 means only this preamble
completed. Stage 308 separately identifies GR firmware bootstrap failure;
a ready mailbox is not a GPU execution pass. Full GPU reset/power
retirement still follows every probe. This helper is for exclusive cold ownership,
never an adapter reset while another channel is active.

`core/gr_firmware.c` prepares Switchroot's NETB/FECS/GPCCS data for the next GR
initialization step. It reads unaligned little-endian region tables, accepts
byte-sized Falcon code, validates typed register streams and builds the six
256-byte-aligned boot/code/data segments with zero-filled word and segment
padding. It selects the T21x DMEM-header format from the upstream bootloader
checksum and encodes the mapped 40-bit GPU addresses. The checksum is an ABI
selector, not a firmware signature. Inputs remain external and are not copied
into the source repository. The probe package carries them as separate files. Host tests cover malformed ranges, unknown boot
versions, unaligned payloads, padding and insufficient storage; the same test
also accepts three local paths for validation against actual NVIDIA files:

```sh
build/tegra-gpu/test-gr-firmware NETB_img.bin fecs.bin gpccs.bin
```

`platform/t214_gr.c` follows Switchroot's non-context pack, scrub/GR-idle wait,
instance binding, FECS/GPCCS DMA bootloader transfer and FECS-ready handshake.
It waits for Falcon DMA IDLE to become set, retains failure-stage/register
information, and refuses the non-secure route when PRIV security is enabled.
The secure path uses `platform/t214_acr.c`: discover the existing protected WPR,
reset/scrub PMU, select the hardware production/debug signature profile, and
run the vendor HS bootloader with bounded authentication/HALT checks. It never
changes security fuses or maps WPR into the CPU.
`platform/t214_pmu.c` writes the versioned 32-byte RTOS arguments, clears the
HS halt event, and starts the LS CPU through its alias. It validates the initial
DMEM queue descriptors, posts one fixed WPR initialization command, and matches
the response unit/type/sequence and result. Queue wrap, bounds and waits are
bounded; trace, GC6 and the PMU power governor remain off. Only a successful
response admits the cold secure GR path: GPCCS uses the GM20B non-secure loader,
while FECS retains its authenticated code and starts through CPUCTL_ALIAS.
No host upload overwrites secure FECS IMEM/DMEM. Golden graphics contexts remain
separate implementation work.
`windows/gr_firmware_io.c` reads only fixed administrator-protected driver paths
at PASSIVE_LEVEL before GPU power-up. The packed surface is copied into a
64 KiB private arena region mapped read-only, with a separate firmware instance.
The ACR surface occupies another 128 KiB with its own PMU instance, within the
same owned 512 KiB arena. Its page entries are installed before BAR1; packing
uses the hardware WPR/DMEM sizes and completes FB/L2 synchronization before
PMU DMA. Only the supported GM20B firmware version 20490253 and its 32-byte
command-line layout are accepted. Stage 309 reports ACR errors; stage 310
reports PMU RTOS/queue failures with initialization headers, queue pointers and
WPR reply status. Authentication or an RTOS reply cannot pass the FIFO test.
Teardown resets PMU/GR/FIFO before
MC drain; backing memory is retained until power
retirement. No shader or Vulkan execution claim follows from these local checks.

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
Live WDDM registration and physical use of this segment remain open.
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
A TLB error returns incomplete progress, never permission to complete its
paging fence or recycle old pages. Address-space serialization and normal
completion are now connected to the shared queue; complete residency,
submission/fault dispatch and the remaining paging operations are still open;
this library is not yet an advertised WDDM paging mode.

`windows/paging_dispatch.c` implements the `BuildPagingBuffer` DDI for these
two operations. It validates KMD process/allocation handles, allocation-relative
ranges and the optional system context, keeps the process/engine alive through
scratch allocation, and snapshots a kernel-only CPU/GPU buffer descriptor.
Continued builds check the exact write offset/capacity and never retain VidMm's
PTE array. Mandatory NULL-DMA CPU initialization requires an initial update of
the system paging process with no active GPU slot. Microsoft documents that
[paging-process bootstrap](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/system-paging-process)
separately from ordinary queued updates.

`windows/paging_submit.c` serializes paging and rendering in the same execution
slot. It drains the memory caches before publishing uncached PTE updates and
executes requested TLB invalidations in record order. Only complete success
arms synchronized OS fence notification; a split buffer containing just updates
does not imply that a later buffer's flush has already run. Partial failure
pins the slot until verified reset, and canceled fences are never reported as
complete. OS private/DMA storage and residency remain valid until notification.
The native context fixture now covers a pending render followed by split paging
buffers, immediate private-buffer reuse, malformed inputs, OOM and TLB/reset
failure. These are controlled OS/GPU models, **not live WDDM or hardware proof**.

`windows/root_table.c` consumes real WDK root-table arguments. It rounds root
sizes to whole CPU pages, resolves the complete segment span, writes the RAMIN
PDB and address limit only for an idle context, and propagates real TLB errors.
A failed root move clears the software binding's valid flag, so the adapter
must not reschedule it with the old root. Flushes belong at the ordered paging
submission point, with old pages retained through completion/reset; they must
not run early from `BuildPagingBuffer`. The context DDIs below now consume
these helpers. The channel worker below implements hardware binding; complete
miniport registration and physical validation remain unfinished.
The WDDM `GetRootPageTableSize` callback reports a full **128 KiB / 16,384-entry**
root from the first allocation, using the documented ability to increase the
[actual entry count](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_getrootpagetablesize).
This makes the queued DDI's full-root alias valid instead of presuming that a
smaller OS allocation has 128 KiB of backing. The generic helper above still
supports explicitly sized roots.
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
capabilities. The UMD-private query now implements the 48-byte
`windows/adapter_abi.h` contract: the caller initializes its version/size
header in an in/out buffer; the response supplies GM20B identity, the **512 MiB
budget ceiling** and the OS-provided adapter LUID when available. Aliased KMT
input/output buffers are supported. The native-submit flag remains unset.
`DRIVERCAPS` stays unsupported until submission/recovery are integrated.
Native ARM64 tests cover real WDK callback shapes, count-only queries, strides,
bad ABI requests, aliased query buffers and preservation of extension bytes.

`windows/adapter.c` composes the existing resource, table, context and queue
modules into AddDevice/StartDevice/StopDevice/RemoveDevice callbacks. The
adapter handle starts with the context engine, so every existing DDI receives
the same handle. Start validates the WDDM 2.4 callbacks, maps only declared
GPU/LIC resources, creates a **2 MiB page-table segment** and the private
command area, initializes the queue and records `DXGK_START_INFO.AdapterLuid`.
It reports zero video-present sources/children and does not acquire the POST
display. These table/command bytes are separate from the application budget;
no 512 MiB physical carve-out is allocated.

Stop closes admission, joins the optional worker, cancels/resets queue work,
retires hardware, closes the platform bridge, and then releases the engine,
table backing and MMIO mappings in order. Live objects, failed frees or failed unmaps retain their
tracking state for recovery/retry; Remove cannot free an adapter with retained
ownership. A failed start uses the same cleanup path. Controlled native ARM64
tests exercise missing LIC, partial map/unmap failure, each allocation failure,
live-device stop, cleanup retry and stop/start cycles; the resource-only
lifecycle test asserts that it performs **zero GPU MMIO**. This is library integration only:
there is still no miniport DriverEntry/INF registration, live DRIVERCAPS,
complete submission/remaining paging operations, IRQ completion or TDR adapter hookup.
The probe uses the shared cold-bootstrap, BAR1 remap and hot channel helpers
described below; this lifecycle module still has no live adapter hookup.

`TgGpuStartAdapterHardware` explicitly connects an already initialized adapter
to `windows/hardware.c`; ordinary StartDevice still only acquires resources.
The trusted platform must supply declared-resource I/O, the PMIC-owner bridge,
temperature preflight and loaded firmware. The hardware state allocates a
512 KiB boot arena and 2 MiB work area through Dxgkrnl, acquires GPU power,
starts the fixed 729.6 MHz profile and calls the shared cold GR bootstrap.
It remaps the retained BAR1 USERD alias to the engine's command/completion
page, copies the golden template, and only then marks submission ready.
Bootstrap schedules no application work and its fixed report sentinels are
not accepted as GPU completion. Firmware source files can be released after
their private copies have been consumed; boot/work arenas remain owned.

`HardwareOwner` prevents context-engine shutdown from freeing Commands while
BAR1 still references that page, including a partial startup. Stop detaches
the queue first, disables fetch/Falcons, drains MC, restores clocks and powers
down. Explicit clamp/reset readback proves isolation before any backing is
freed. Drain, reset or allocation-free failures retain tracking for retry;
an isolated retry performs no more GPU MMIO. A clock-restore error is reported
even when reset safely retires DMA. Full stop advances the graphics generation.
The initial hardware-start API requires a fresh engine/template; live TDR
restart and miniport registration remain open.
`test-hardware.exe` covers startup stages, OOM, queue ownership, isolation,
clock errors and free retry with **explicit GPU/OS substitutes**. It does not
authenticate firmware or access physical GPU registers.

`windows/platform_io.c` supplies borrowed, declared-resource MMIO and a kernel
file handle to the existing `\\Device\\SwitchBacklight` PMIC owner. It opens no
second SPB target and never writes global MC ASIDs, identity fuses or CPU PLLP.
The PLLP query and GPU-rail operations use the owner's existing kernel-only
IOCTL; temperature/address preflight precedes power acquisition. Bridge lifetime
is serialized at PASSIVE with special kernel APCs enabled. Aligned mapped
register access also serves the ISR; PMIC/PLLP queries and sleeping delays stay
on PASSIVE paths. Failed close retains the handle and borrowed resources.
The probe uses this same provider, retaining it through partial IRQ restoration.
`test-platform.exe` covers the bridge protocol, MMIO bounds, read-only ownership,
thermal refusal and close retry with explicit OS substitutes.

`TgGpuStartPlatformAdapter` composes bridge/preflight, trusted firmware loading,
cold hardware initialization and `windows/worker.c`. The kernel thread waits on
stop/queue events; only pending work uses a 1 ms polling timeout. Faults pause
admission and retain references for recovery without reporting canceled fences
complete. Stop joins the thread before queue reset or releasing any I/O backing;
failed join/close preserves tracking for retry. `test-worker.exe` exercises these
states with a controlled dispatcher/thread substitute, **not an actual kernel
thread**. Full platform-adapter startup and queue execution still have no live
WDDM evidence; complete paging/submission/IRQ/TDR dispatch and registration remain unfinished.

`windows/context.c` supplies the process/device/context creation and destruction
DDIs, root-table DDIs, completed-fence query and DPC callback. Its registration
helper fills those entries in the caller's initialization table; it does not
register a display adapter alone. Each ordinary context owns one uncached
**976 KiB allocation** through Dxgkrnl: a RAMIN page followed by its packed GR
image/global buffers/permission map/patch data. System contexts retain a RAMIN
page without an application graphics mapping. Parent references prevent teardown during pending
work; failed frees preserve the context for retry. Software contexts are
dynamically allocated, while hardware work uses one serialized execution slot.
Root changes cannot rewrite an active instance, and failed changes prevent
future submission with a stale binding.

`windows/graphics.c` owns the WDDM graphics format and mapping path. After the
cold backend obtains a successful **1 GPC / 2 TPC / 4 ZCULL** golden save,
`TgGpuInitializeGraphicsTemplate` takes an independent immutable 128 KiB-capacity
copy. A context's first preparation clones the image, rebases its 21 global
patches and creates the restricted 30-register access map. It runs the large
uncached initialization outside the object spin lock while the work reference
prevents reset/destruction. Repeated submissions preserve the GPU-saved image
and global buffers. The parent allocation has one owning handle; no per-context
golden copy or second allocation needs a partially failing construction unwind.

The packed payload contains a 128 KiB context image, 8 KiB bundle, 32 KiB
pagepool, 288 KiB attribute buffer, 512 KiB access map and 4 KiB patch page.
Its 243 privileged PTEs occupy the process reservation at **base + 1 MiB**;
access-map/patch pages are GPU read-only. Full 40-bit virtual addresses are
retained in RAMIN and image pointers. Only the active context's backing is
mapped, after the previous hardware channel/GR context has retired. An owner
physical address prevents destruction of an idle context from removing another
context's mapping. RAMIN is never included in these PTEs. Destruction clears
its own stale mapping before freeing, and a failed free retains the object.

Verified full reset advances the graphics generation. Existing initialized
contexts are rejected on capture/activation; the UMD must recreate them after
device loss. Unsubmitted, uninitialized contexts can still be prepared from the
immutable template. Stop releases the template after context teardown; a failed
template free retains its owning handle for retry.

Full ARM64 compilation and native controlled WDK tests pass for topology/source
rejection, independent golden ownership, packed headers/global pointers above
4 GiB, PTE privilege/permissions/bounds, mapping replacement, preserved saved
state, failed-free retry and reset generations. **These tests use synthetic
golden data and substituted OS/GPU callbacks.** The packed allocation, privilege
enforcement and WDDM dispatch have no physical GPU pass yet. The explicit
hardware API now composes the template and command-page BAR1 mapping, but its
complete adapter dispatch and adapter/ICD registration remain unfinished.

The submission-worker helpers capture a bounded context/root snapshot separately
from activation. Only activation writes the complement of the private fence
marker, so admitting later work cannot overwrite the active marker. Before
notification, the GPU marker must match and the channel must be quiescent; the
context reference is then released. `DxgkCbSynchronizeExecution` delivers
`DxgkCbNotifyInterrupt` and the DPC notification using an engine-only snapshot,
so context destruction after completion cannot race a remaining context access.
A synchronization failure keeps that notification record for retry/reset and
prevents later fences overtaking it. Aborted work never advances the completed
fence. The submission DDI below now connects structural admission and recovery
notification; command/privilege validation, VidMm residency and actual TDR
reset/restart remain incomplete.
`--windows-tests` also builds `test-context.exe`; it calls the registered WDK
function pointers using explicit allocation, interrupt and GPU-write substitutes.
It checks lifetime, failed root changes, pending/duplicate notification, retained
references and failed-free retry, not live Dxgkrnl or GPU execution.

`windows/submit_ddi.c` supplies the library's `SubmitCommandVirtual` entry.
It returns only the documented [SUCCESS or malformed INVALID_PARAMETER](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual).
Internal OOM, unavailable hardware, lost roots and queue failures request
recovery rather than being mislabeled as application formatting errors. Capture
also checks the actual GPFIFO packet-length/address bounds. This is structural
admission, **not a complete command/residency validator**.

The DDI reserves a nonpaged node before validation, so a malformed packet does
not depend on its invalid private buffer remaining alive. Its software marker
follows all older work. Adjacent rejected markers advance the driver's fence
notion before the preceding real DMA notification; an idle rejected marker
advances synchronously. No extra DMA completion is generated for a rejected
packet. A PASSIVE dispatcher mutex serializes DDI admission with worker steps;
empty/null-render/context-switch packets retire in order without GPU accesses.
The ordinary internal admission helper remains allocation-free/DISPATCH-safe.

`windows/recovery.c` pauses admission and makes one synchronized
`DXGK_INTERRUPT_GPU_ENGINE_TIMEOUT` recovery request while preserving the
first internal reason and incomplete fences. Failed synchronization can be
retried, and duplicate requests do not generate duplicate notifications.
The worker calls it after internal service/wait errors. Stop joins the worker
and retains all references until retirement/isolation. **Actual WDDM
reset/restart/preemption callbacks are still missing**, as are the physical
packed USER/KERNEL batch and privilege/residency validation. Native-submit stays
unset; no DriverEntry or INF registers this unfinished adapter. The native
fixtures cover fence order at notification, idle rejection, software packets,
OOM and recovery-notification retry using controlled OS/GPU substitutes.

`windows/work_queue.c` admits contexts and kernel paging records in order using the nonpaged
private buffer supplied by Dxgkrnl for each DMA buffer. Context creation reports
the required private-data size; this queue accepts no UMD-private payload.
Internal admission allocates nothing and holds each
context/root reference. The single worker copies the head record into its own
storage, activates and submits it, polls its GPU marker, quiesces the channel,
then notifies completion. It never accesses the borrowed record afterward, even
if Windows immediately reuses it. Pending work does not touch shared RAMIN or
command/fence memory. A failed operation stops the queue; reset retains affected
references until isolation succeeds, then cancels pending work without reporting
its fences complete. Admission resumes only explicitly after recovery. Wake is
a synchronization event consumed by the adapter worker; pending work waits on
that event, stop, or a polling timeout rather than busy-looping.
The queue and channel helpers are compiled backend components, not an exposed
`SubmitCommandVirtual` DDI: adapter dispatch, malformed-buffer fence handling,
command/residency validation and preemption/TDR notification still need wiring.
Native tests cover multiple roots, ordering, immediate private-buffer reuse,
partial submission/reset failure, restart admission and canceled notifications.
[Dxgkrnl private-buffer contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_submitcommandvirtual)

`windows/submit_worker.c` connects prepared work to `core/channel.c` after
exclusive cold initialization. The worker blocks scheduling, invalidates the
root TLB, binds RAMIN, enables the channel and submits the physical runlist
before allowing execution. Saved RAMFC and USERD GP_PUT start **empty**.
`tg_gpu_channel_bind` leaves them empty; `tg_gpu_channel_publish` notifies
through the owned BAR1 USERD alias only after binding. The helper validates
the active BAR1 instance/snoop base and increasing non-wrapping indices,
posts GP_PUT, requires its readback, and retains ownership after a failed
write/readback or a subsequent fault. The WDDM wrapper publishes both entries
as one batch: USER workload plus the KERNEL completion packet.

The cold backend must map the shared command area's USERD page into its
retained BAR1 root and mark it ready. The worker refuses submissions before
that mapping is ready; verified reset invalidates that readiness, and queue
resume requires the new mapping. Live cold-backend hookup remains unfinished.
The queue now calls `TgGpuObserveContextWork`: require a matching GPU BAR1
fence, check FIFO/GR faults, write back/invalidate L2 and require the same CPU
readback before retiring/notifying. These waits run outside the object spin
lock with hardware ownership held. `TgGpuContextWorkFinished` remains a raw
CPU-fence query; the worker does not use that query as its completion proof.
The controlled native test deliberately places the expected value only in the
CPU alias and requires pending, then models a GPU write and cache visibility.
Queuing alone never notifies a completed fence. The work reference is released
only after bounded disable/preempt, **FECS CURRENT_CTX.VALID clearing** when
GR is enabled, runlist removal, unbind, FB flush and TLB invalidation succeed.
The helper observes GR unloading and never clears its valid bit itself;
the remaining instance bits are not interpreted as a resident context.
A timeout retains RAMIN, roots and commands. Recovery disables fetch, drains
MC, restores clocks and powers down; clamp/reset readback must confirm isolation
before releasing failed work. No caller-supplied "hardware retired" boolean
can bypass these conditions. Cold reinitialization is required after recovery.
MMIO/waits run outside the object spin lock, with an active reference held.
Native ARM64 tests exercise submission order, duplicate rejection, preempt
timeout/retry, failed reset acknowledgement and cancellation without MMIO using
controlled hardware substitutes. Physical execution and privilege enforcement
of the complete WDDM path remain unverified. These are worker helpers, not a registered
[`SubmitCommandVirtual` DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual):
its allowed return values differ from the worker's internal failure statuses.

`windows/internal_va.c` reserves one leaf-table coverage range (64 MiB) through
`DxgkCbReserveGpuVirtualAddressRange` inside process creation, with user-mode
mapping disabled. A private 128 KiB leaf maps the engine's command ring,
completion packet and USERD/fence pages, plus the current context's packed GR
payload, all using GM20B privileged PTEs. Ring/packet and GR permission/patch
mappings are read-only; RAMIN and the physical runlist are not mapped.
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

Version 0.1.0.30 / ABI 18 adds an opt-in cold graphics-context diagnostic:
`LitePerformanceCheck.exe --gpu-json --gpu-gr-729`. It uses a separate owned
1 MiB allocation, the NETB context/method/bundle streams and the fixed GM20B
one-GPC/two-TPC topology to initialize global buffers and save a golden context
through FECS. The original `--gpu-729` FIFO entry point remains separate.
`gr_context_pass` additionally requires successful bind/save replies, an image
header and changed image payload after L2 synchronization, successful FIFO
readback and full resource retirement. An unchanged golden-image sentinel or
stale mailbox cannot pass. No shader, Vulkan ICD or drawing is supplied by this
diagnostic. See the status page for the actual physical validation state.

Version 0.1.0.31 corrects the topology gate to the physical
`0x502608 = 0x00040002`: one GPC, two TPCs and four ZCULL banks, matching
the pinned GM20B `proj_scal_litter_num_zcull_banks_v()` definition. It keeps
two SMs/tiles in the non-floorswept setup while programming four active banks;
other bank/TPC layouts are rejected before GR writes. ABI 18 is unchanged.
One physical Lite run on 2026-11-08 passed the golden-context diagnostic at
729.6 MHz: bind/save replies 0x10/0x1, image magic 600DC0DE, 19,328 changed body
words and hash C28E60AD. Subsequent FIFO values 551E4780/AAE1B87F both matched;
faults, restore status and retained-resource count were zero. This establishes
GR initialization/golden save and FIFO completion, not shader execution or a
Vulkan drawing pass. That GR run used oem61.inf / ABI 18; the rolling status
records the current package, signing, backup paths and physical stdout.

Version **0.1.0.32 / ABI 19 (608 bytes)** introduced the separate
`--gpu-json --gpu-irq-729` diagnostic. It appends a FIFO nonstall notification
after each of two independent reports and admits the second only after the
first ISR/DPC has arrived. `irq_pass` requires both GPU readbacks, two genuine
FIFO acknowledgements and DPCs, and successful restoration/retirement. IRQ
receipt alone never writes or completes a fence. The accepted FIFO and golden
entry points remain separate.

IRQ testing requires `SWITCH_LITE_GPU_IRQ=1` in the paired firmware profile.
This adds only LIC bank 4's `0x60004414/0x1C` resource to GPU0; it changes no
wake policy. The older table remains usable for FIFO/GR and rejects IRQ runs
before GPU power-up. Only nonstall SPI 158 / GSI 190 and LIC bit 30 are enabled.
The handler acknowledges FIFO channel events without clearing unrelated
PBDMA/MMU faults. Windows owns the GIC. Teardown masks/synchronizes the ISR,
cancels/waits for its private DPC, and restores LIC bits; failed restoration
retains ownership until retry or verified GPU reset/LIC retirement.

Version **0.1.0.34 / ABI 20 (616 bytes)** has one physical Lite IRQ pass at
729.6 MHz on 2026-11-08: both independent GPU report values
`1FAA904A/E0556FB5` matched, with two acknowledged ISRs, two DPCs, GSI 190,
Stage 306, zero execution/restore/fault status and zero retained resources.
It does not establish live WDDM dispatch or shader execution. The original
GR/golden entry point's physical baseline remains 0.1.0.31.

The first IRQ arm attempt stopped before submission. ABI 20 added
`interrupt.stage/last_register/last_value`, frozen at the first failure across
cleanup; .33 then identified `MC_MASK1 (0x57000644) = FFFFFFFF` as the rejected
snapshot. Version .34 accepts full 32-bit MC/LIC bitmap values, including all
ones, while still checking every written owned bit. Status/identity all-ones
checks remain. The model checks full-bitmap save/restore, an ignored `0x100`
mask write, failed-read diagnostics and restoration after partial arm.
[MC mask programming](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/blob/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu/common/mc/mc_gm20b.c)
and [LIC bitmap operations](https://github.com/torvalds/linux/blob/v6.12/drivers/irqchip/irq-tegra.c)
define the reference register semantics.

The host models, sanitizers, real NETB checks and real-WDK ARM64 build pass.
The native ABI-20 JSON fixture passes 34 cases with explicit device-I/O
substitutes; the earlier allocation/context/resource fixtures remain local
contract evidence. The rolling status records package hashes, the unchanged
GPU-only test table, rollback paths and hardware availability.

Version **0.1.0.35 / ABI 20 unchanged** introduced
`core/gr_image.{c,h}` and the channel-image helpers in `t214_gr_context`.
They clone the immutable golden image, initialize per-context header fields,
disable unbacked PM/SMPC/trace/CTA features, encode all 40 GPU-VA bits and
apply the pinned 30-register GM20B access map using `USE_MAP`, never allow-all.
The existing 21 global-state writes now share a register/value builder with
the FECS patch list, so bundle/pagepool/attribute addresses can be rebased.
`tg_gpu_gr_context_prepare_channel` installs a separate 2 MiB private layout,
including a 512 KiB access map and a patch page, both GPU read-only.
`tg_gpu_gr_context_load_image` requires a completed golden save and cleared
FECS current context, builds the clone/map/patches, then synchronizes CPU
writes. The accepted golden diagnostic retains its 1 MiB layout.
Byte-format/bounds/immutable-source tests, preparation/visibility failure
models, real NETB, sanitizers and the ARM64 build pass locally. This loader
is connected to the distinct .36 probe operation described below. WDDM
dispatch remains separate and unregistered.

Version **0.1.0.36 / ABI 21 (640 bytes)** adds the separate write-access
`TG_GPU_IOCTL_3D_RUN` and `--gpu-json --gpu-3d-729`. It loads the per-channel
image, binds Maxwell B (`B197`) on subchannel zero and submits two ordered
NO_OPERATION/WAIT_FOR_IDLE + unchanged Mesa 3D report packets. The second
submission follows the first engine report, and CPU readback follows L2 sync.
The original FIFO, golden and nonstall IRQ paths remain distinct.
`gr_engine_pass` also requires successful golden initialization, image load,
B197 class, zero GR/FECS fault status and complete cleanup. ABI 21 reports
image readiness/result, hardware current context and save/restore counters.
The existing tool shares its actual acceptance predicates with host failure
fixtures; the native JSON fixture covers write access, new-mode conflicts,
image/class/GR faults, sentinel readback and retained-resource rejection.

**Physical Lite pass, 2026-11-09 00:43:26 Pacific:** one .36 run exited 0,
Stage 306 at 729.6 MHz, `gr_engine_pass=true`. Both GPU reports matched
`F59F3501/0A60CAFE`; FECS current context was `800FDE14`, restore count 1,
image ready/result 1/0, and power/clock/execution/GR/PRI/FIFO/PBDMA/MMU/
restoration/owned states were zero. Golden bind/save and image evidence passed.
This proves graphics-engine reports from the loaded context, not shader or
Vulkan drawing. Native SYS/CAT/11-member verification and all 47 ABI-21 JSON
fixture cases pass; the latter substitute device I/O. The rolling status lists
raw evidence, installed package and next work.

Version **0.1.0.39 / ABI 23 (720 bytes)** adds a separate fixed ROP clear
diagnostic: `--gpu-json --gpu-clear-729` / `TG_GPU_IOCTL_CLEAR_RUN`.
`core/gr_clear` encodes a 16x16 A8B8G8R8 pitch target (128-byte rows), blue
background and central 8x8 red rectangle, each followed by a Mesa 3D report.
The target occupies a private page in the existing 2 MiB GR mapping. CPU
initialization writes only `DEADBEEF`; after both reports and L2 visibility,
the checker compares all 256 pixels plus 768 guard/padding words. Sampling
two pixels alone cannot pass. `pixel_pass` requires this complete comparison
and the inherited golden/image/engine/retirement conditions.

The .37 clear stopped at Stage 304/E17 with GR class-error `00100000`, no
GPU reports and clean retirement. ABI 23/.38 added read-only trapped-method,
data, class-error and FE/MME/SCC diagnostics before reset; it identified
`0x0FB8 (SET_ANTI_ALIAS_RASTER)=0`, class-error 4. This extra write is absent
from pinned NVK's initialization; .39 removes it and its packet regression
rejects reintroduction. No unknown error is cleared or accepted as success.

**Physical Lite clear pass, 2026-11-09 00:59:21 Pacific:** one .39 run exited
0 at 729.6 MHz, Stage 306, `pixel_pass=true`. Reports `0C8E163C/F371E9C3`
matched, all 256 pixels matched, guards remained intact, image hash
`F20AF985`, first background/foreground `FFFF0000/FF0000FF`, FECS restore
count 1. GR/FECS/PRI/FIFO/PBDMA/MMU faults, result/restore and retained
resources were zero. This is hardware off-screen color clearing/readback,
not shader execution or a Vulkan API pass. The native ABI-23 tool fixture
passes 59 cases with substituted device I/O, and local packet/pixel/guard
models, sanitizers, real NETB and ARM64 builds pass. The accepted signed
.39 package has native SYS/CAT and all 11 catalogue memberships verified.

### Fixed NAK shader draw diagnostic

Version **0.1.0.41 / ABI 24 (752 bytes)** adds `--gpu-json --gpu-draw-729`
and the separate `TG_GPU_IOCTL_DRAW_RUN`. It retains the accepted blue clear
as its first packet. The second packet contains no `CLEAR_SURFACE`: a
vertex-ID triangle and a central 8x8 scissor must execute a constant-red
fragment shader to produce the expected foreground. The complete pixel and
guard checker is shared with the clear diagnostic. `shader_pass` additionally
requires exact requested/checked SM53 shader metadata and code hash, both
reports, successful GR restore, fault-free completion and complete retirement.
This fixed private workload is not a Vulkan API draw or an installed NVK ICD.

`tools/build_draw_shaders.py --mesa <pinned-checkout> --out <new-directory>`
reproduces the committed `core/draw_sm53.h` from `tools/shaders/rect.{vert,frag}`.
It archives Mesa `0ff52079`, creates a compiler-only host overlay and invokes
the unchanged SPIR-V frontend/NIR/NAK compiler. The overlay omits Linux device
backends, hardware-test bindings and unrelated NVK runtime/CLC dependencies;
NVK root-descriptor layout/constants are extracted unchanged. It neither
modifies the external checkout nor builds an ICD. Host prerequisites are in
the helper's docstring; Rust 1.90 is sufficient. Source/SPIR-V/header/code
SHA256 values are recorded in `tools/shaders/manifest.json`.

The same helper accepts `--windows-cross-file <meson-cross.ini> --build-only`
to build a **Windows ARM64 compiler executable**, without running it on the
build host. The cross file selects LLVM-MinGW's AArch64 C/C++ tools and Rust's
`aarch64-pc-windows-gnullvm` target, sets `needs_exe_wrapper=true`, isolates
pkg-config from host libraries, and statically links the C++ runtime. Bindgen
must use the matching Windows target, MinGW sysroot/includes and libclang.
`tools/mesa-windows-nak.patch` preserves the seven four-byte NIR flag words'
GNU bitfield layout under MinGW, uses `ENUM_PACKED` for the mesh enum, and
removes the Linux ioctl-header dependency from Windows format definitions.
NAK/NIR algorithms and NVK descriptor constants remain unchanged.

The clean helper cross-build and native Windows execution both pass. On this
Lite, the VS/FS compiler returns exit 0, SM53, 24 GPRs and 160/64 code bytes;
both stages' code, 80-byte headers, metadata and disassembly match the accepted
host output **byte-for-byte**. This is native CPU shader compilation, not GPU
submission, a Vulkan API pass or an ICD DLL. The build-host Mesa CLC and
`vtn_bindgen2` tools are also available for the later full NVK runtime build.

The generated VS/FS use 160/64 bytes of code, 24 GPRs each, no descriptors,
constant data, scratch, call stack or shared memory. Each occupies one GPU
read-only page at work offset `0x1a0000` / `0x1a1000`. Following NVK's
pre-Volta upload convention, a header begins at byte `0x30`, its 80-byte SPH
places the first instruction at `0x80`, and pipeline offsets are `0x30/0x1030`
relative to the program region. The pixel page remains separate at `0x190000`.
Only code/header data and report/pixel sentinels are initialized by the CPU.
Code FNV `FB041DD2` is checked before publication and after completion; this
does not replace pixel verification. Model packet decoding rejects clear
methods, wrong shader bindings, bad bounds, corruption and rejected `0x0FB8`.
The first .40 physical attempt completed the blue-clear report, then stopped
at Stage 305/E17 with `GR_INTR=0x10`, trapped method `0x1528=0`; retirement
and retained resources were zero. `SET_SHADER_EXCEPTIONS` is a software
method emitted only by NVK's Horizon path, not ordinary hardware state.
.41 omits it, preserves golden fault handling and rejects it in the decoder.

**Physical Lite shader pass, 2026-11-09 01:43:22 Pacific:** .41/oem71 exited
0 at 729.6 MHz, Stage 306, `shader_pass=true`. Reports `556A590A/AA95A6F5`
matched; 256 pixels and all 768 guard/padding words matched, image hash
`F20AF985`, NAK code hash `FB041DD2`, FECS restore count 1. GR/FECS/PRI/FIFO/
PBDMA/MMU faults, execution/restore results and retained resources were zero.
The first packet clears only blue; the second shader packet produces red.
This establishes fixed NAK shader execution/readback, not live VidMm/WDDM
dispatch, a Vulkan API draw or a Windows NVK ICD. The matching ABI-24 tool's
74 native JSON fixtures pass with substituted device I/O. Native SYS/CAT and
all 11 catalogue memberships pass, and no reboot was needed.

### Shared cold graphics bootstrap

Version **0.1.0.43 / ABI 24** extracts the existing initialization prefix into
`tg_gpu_boot_graphics`: FIFO/MMU/BAR1, authenticated firmware, GR/golden save
and prototype graphics-image loading retain their original hardware order.
The entry does **not** bind a runnable channel, publish a runlist or write
GP_PUT. `boot_ready` distinguishes successful initialization from a partially
owned failure; `channel_bound` gates USERD snapshots independently of stages.
Success retains the power state and both private arenas, so stop, MC drain and
verified power retirement remain mandatory before freeing them. Firmware
source buffers may be released after their copies have been consumed.

The normal diagnostic now calls that same prefix. **Physical Lite regression,
2026-11-09 02:35:59 Pacific:** .43/oem72 returns `shader_pass=true`, Stage 306,
729.6 MHz, reports `8CEBD4EC/73142B13`, restore count 1, **256 pixels and 768
guards matching**, image/code hashes `F20AF985/FB041DD2`, and zero faults,
restore error or retained ownership. Build/model/real NETB checks and native
SYS/CAT/all 11 memberships pass; installation needed no reboot. This validates
the shared prefix through the diagnostic, not the standalone held-ready entry,
per-context WDDM graphics images or a live submission backend.

### Shared hot channel validation

Version **0.1.0.44 / ABI 24** routes the shader diagnostic through the same
`core/channel.c` bind, BAR1 publication and retirement helpers used by the
WDDM worker. It starts empty, publishes the clear and draw entries separately,
verifies both GPU reports and the complete pixel/code readback, then preempts
TSG zero and requires **FECS CURRENT_CTX.VALID=0** before successful retirement.
The earlier FIFO/golden/IRQ/3D/clear diagnostics keep their existing bind path.

**Physical Lite pass, 2026-11-09 03:07:00 Pacific:** .44/oem73 returns exit 0,
`shader_pass=true`, 729.6 MHz, Stage 306, reports `E6794A33/1986B5CC`, restore
count 1, **256 pixels and 768 guards matching**, image/code hashes
`F20AF985/FB041DD2` and zero faults, restore error or retained resources.
After retirement, the observed GR context is `000FDDF9`: its instance bits
remain, but the valid bit is clear. SYS/CAT/all 11 memberships, the full build,
model/sanitizer checks and native WDK context/paging fixtures pass. Installation
needed no reboot. The native fixtures cover empty preparation, BAR1 ordering,
unready mapping rejection and readiness invalidation across reset.

This physical run validates the shared hot helpers with the private shader
arena. It does not validate WDDM's distinct command-page BAR1 mapping, its
two-entry USER/KERNEL batch, per-context graphics images, live VidMm dispatch
or Vulkan API calls. Those paths remain the next integration work.

**Physical BAR1-remap pass, 2026-11-09 03:58:47 Pacific:** .46/oem74 uses
`tg_gpu_boot_map_userd` to replace the USERD alias with the separate final
page of the owned GR allocation before binding. The helper disables snoop,
publishes its PTE/RAMFC pointer, synchronizes memory, invalidates the boot-root
TLB and re-enables snoop. GP_GET/GP_PUT must both start at zero; no workload is
published during mapping. Actual draw returns `bar1_phase=7`, `shader_pass=true`,
GP_GET/GP_PUT **2/2**, reports `AC742EBE/538BD141`, **256 pixels and 768 guards
matching**, hashes `F20AF985/FB041DD2`, 729.6 MHz, Stage 306, restore count 1,
and GR context `000FDE3E` with VALID clear after retirement. Faults, restore
error and owned resources are zero. Full build/real NETB/sanitizers, native
hardware/context/paging fixtures and SYS/CAT/all 11 memberships pass; no reboot
was needed. This proves the shared remap with a distinct physical USERD page
in the private diagnostic. The complete WDDM startup/queue and packed context
path still have controlled-model evidence only; no Vulkan API pass is implied.

**Physical platform-provider pass, 2026-11-09 04:27:04 Pacific:** .47/oem75
uses the shared WDM bridge/MMIO provider above, with no KMDF I/O target.
The separate IRQ run receives **2 ISR / 2 DPC** on GSI 190. The subsequent
draw matches all **256 pixels and 768 guards**, reports `A1F83223/5E07CDDC`,
and retains the .46 hashes, phase 7, GP_GET/PUT 2/2 and retired context.
Faults, restoration errors and owned resources are zero; no reboot was needed.
All five native fixtures and signed catalogue checks pass. The kernel worker
and complete platform-adapter startup remain library/model evidence only.

The owner-selected WDDM/NVK UMA budget is a **512 MiB maximum GPU working
set**, allocated dynamically within VidMm's actual budget. The earlier 1 GiB
pressure-test target is withdrawn. This is not a boot-time carve-out or a preallocated probe pool.
The physical diagnostic still allocates only 512 KiB arena + 2 MiB GR work
temporarily and frees them after reset/drain. Live WDDM residency and its
memory-pressure behavior remain unverified.

The latest build runs `test-gr-context` with model MMIO/GPU-write substitutes
and, when provided, the real external NETB file. It checks owned mapping bounds,
stream-port write ordering, buffer encodings, failed replies/timeouts and image
visibility; it does not count as a physical golden-image save.
The probe/bridge linker uses the working T214 kernel image profile: resident
code/data/unwind sections, discardable INIT, and `/tsaware:no`. The default
image profile compiled and signed but failed native driver loading with
Win32 error 487. The corrected images load in ARM64 WinPE with test signing;
that loader check does not exercise the Lite's ACPI resources or GPU.

The firmware consumes `platform/t214_resources.h` through its WindowsDrivers
submodule. Build it with `-D SWITCH_LITE_GPU=1` to expose the separate ACPI
render node. Keep the existing POST display path during bring-up.
