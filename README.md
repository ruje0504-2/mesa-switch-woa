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
Status ABI 3 includes PBDMA enable and USERD GET/PUT observations; a queued
command or enabled channel is not proof of execution. `--probe --keep-fan`
rebuilds only the GPU probe when the matching SwitchFan bridge is verified.

The diagnostic accepts only owned physical buffers below 4 GiB. Switchroot's
GM20B path tags IOMMU addresses with bit 34 and leaves physical addresses
untagged. The probe therefore leaves Tegra's global GPU/GPUB ASID registers
unchanged; a nonzero ASID readback is not a failure for this physical path.
This does not provide a Windows IOMMU domain or authorize arbitrary DMA.

`windows/paging.c` translates real WDK `DXGK_PTE` entries into GM20B entries
for an uncached, idle address space. It validates the entire update before
publishing, supports repeat/invalid entries, and rejects unsupported flags.
The resolver must pin and validate GPU mappings. GM20B's 128 KiB tables need
a dedicated nonzero CPU-visible segment; WDDM limits segment-zero tables to
4 KiB. `windows/table_segment.c` provides the contiguous segment backing,
descriptor, bounded byte-offset resolver and uncached CPU mapping. It uses
WDDM 2.4 allocation callbacks with physical bounds of 2–4 GiB, leaves bit 34
clear, and publishes no cache-coherence or sleep-retention capability.
VidMm remains responsible for allocations within the segment; there is no
second suballocator. The adapter must finish GPU work/drain/reset and stop
paging before destroying the segment. Failed frees retain the tracking handle.
Do not use the pool's own CPU VA to validate a different VidMm CPU mapping.
WDDM callback wiring and physical use of this segment remain open.
`platform/t214_mmu.c` performs the Switchroot TLB invalidate sequence and
reports timeout/read failures. Add `--windows-tests` to build `test-paging.exe`
against the real WDK types, then run it in ARM64 Windows/WinPE. This test uses
owned test memory and does not access a GPU. The same executable covers segment
bounds, real-WDK callback shapes, allocation failure and cleanup retry with
controlled OS-call substitutes; it does not test real Dxgkrnl allocation.

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
