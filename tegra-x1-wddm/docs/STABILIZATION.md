# Native desktop stabilization baseline

The user selected the last verified `.153` component set on November 12,
2026. Stabilize complete desktop composition and sustained presentation before
adding further features or hardware rotation. The main development history is
preserved separately; this branch starts at `931383e6f3a1cd4bdb7d2557ebcac2bf8a3c5c0d`,
before the `3531f0ee` SAMPLE_I change.

| Component | Frozen identity |
| --- | --- |
| KMD | `.153`, signed SYS prefix `94bcb716d824`, source `110d9882` |
| D3D UMD | `e99798fa966844429f5721a1799d3f2ff7fc8a4fa514ec3090ad5462e53360a5`, source `931383e6` |
| NVK ICD | `6e2fad99d742a69033cc59925093fb4a7c8489728a3f7e5e9ebb523c017e0323` |

The KMD's core/platform/windows source is unchanged between `110d9882` and
this branch's baseline. Use the exact archived signed package and runtime
binaries together for the initial observation; do not combine these with the
`.147` runtime or the 48-byte primary producers from a different package.

`shared-usage153-native-20261111` passed 21,758,982 shared-sampling pixel
comparisons and clean teardown. It retained 78 DWM flip callbacks over 72.794
seconds. A subsequent physical observation still showed missing/displaced
taskbar content and shell restarts. This is a rendering baseline, not a complete
or stable desktop pass. The previously captured Explorer SAMPLE_I abort and
later sampler-view compiler defect remain known problems; any correction
must be isolated and validated rather than importing the entire later stack.

Work order:

1. Freeze component identities, portrait 720x1280 and the existing Never power
   settings. Keep the 512-MiB dynamic GPU ceiling and existing recovery path.
2. Capture the actual native desktop early, with driver/LUID/timestamps and
   taskbar geometry. Inspect taskbar, Start menu and right-side notification
   area; successful Presents alone are insufficient.
3. Fix the first demonstrated desktop defect in a small change, then repeat
   only the affected rendering and desktop checks. Track sustained operation
   separately from a single screenshot.
4. Resume hardware landscape, Win32 vkcube and later Vulkan work after this
   desktop baseline is stable.

The device remains on recovered `.59` until a bounded trial is explicitly
staged and started. Source rollback is not an assertion that a driver has
already been installed. Fable's completed desktop-review commits and five
unfinished files are retained locally. Following the user's confirmation of
restored quota and explicit authorization to modify source, Fable 5.2/medium
is implementing game-facing DXVK in a separate remote branch. No remote GitHub
push or desktop DXVK development is authorized. This does not block desktop
stabilization.

The initial frozen-stack retry reaches Code 0 but is interrupted by another
boot before its PNG completes; `.59` recovery is verified. The retained MPO
submissions contain one full-screen plane with matching VSync addresses,
while native shell geometry shows an empty Start child. See the
[MPO contract and observation audit](MPO_CONTRACT_AUDIT.md). This branch is
still a stabilization starting point, not a stable-driver release.

## First isolated correction

`b0d377f6` ports only the `TGSI_FILE_SAMPLER_VIEW` resource-operand case from
`ee7b0d50`. The sole production-code difference from `931383e6` is that one
line in `ttn_get_src`; SAMPLE_I lowering and `.154–.156` KMD changes are not
imported. This removes the ordinary-SAMPLE undefined behavior previously
reproduced in a debug build; it does not implement the known missing SAMPLE_I
instruction or establish a complete desktop fix.

The independent release build produces UMD
`9cdf0374821021c8d45ce10aef889a4a950628265bade443568d7e45134e4896`.
Its exact DLL and runtime shared/primary/Present/ownership controls pass on
recovered `.59` at 02:58:42 UTC, with 114 valid ETW events. The independent
`debugoptimized`, `b_ndebug=false` translator executable
`2453f859a13d56ef577fb7aecf950d6ba0fb1447a891d68461df821f60df38b5`
passes all six `TgsiSample` tests at 03:00:01 UTC, exit 0 and no timeout.
Device status stays `.59` Code 0 before and after. These are user-mode controls;
the new UMD has not been installed or tested with native GPU execution.

Evidence directories: `stabilize153-sampler-view-20261112-umd`,
`stabilize153-sampler-view-control-20261112` and
`stabilize153-sampler-view-debug-20261112` in the external build root.

## GR helper-load correction

Fable's independent local commit `52e35569` is fetched over SSH (no remote
GitHub push or credential transfer) and ported as `d2edb249`. It establishes
NVK's fragment helper-invocation memory-load policy in the golden GR context.
The main agent independently checks nouveau `gk110_grctx_generate_r419f78`
and NVK's skipped `_WIN32` private-register update. Static parsing of the
actual NETB (SHA256 `5b285e5de99b935c2619e54b1656b8a2f271ba845e80952faadfd09960d6eea3`)
finds region 5, byte offset 5,196 (file offset 46,506), writing `0x10beb` to
`0x419f78`. Bit 3 disables the helper loads; the corrected policy is `0x10be3`.
This proves the initialization input, not a live register observation or the
cause of the captured native hang.

The port also checks the written value before saving the golden image and
fails initialization if the write did not take effect. Its regression uses
the actual NETB value, accepts the vendor stream's initial write, and checks
the final policy before FECS save. An ignored policy write fails instead of
saving an unusable image. Independent negative controls stop at that save
assertion with both original `.153` source and the remote source before the
readback correction; corrected host models, the real external NETB stream
and the full WDK ARM64 build pass.

This KMD candidate is numbered `.157`, based on `.153` without importing
`.154–.156` queue/diagnostic changes. Signing and package verification pass at
03:12:34 UTC on November 12; it is not installed. Evidence
is in `helper-load-independent-20261112` and `helper-load157-20261112`.
Taskbar content, continuous rendering and the `.155` stalled instruction
remain unproven until native validation. Only this completed remote correction
has been ported; the separate SM exception policy and unfinished worker edits
remain unadopted.

## Integrated stabilization candidate .158

`71d67992` restores SAMPLE_I lowering and its translator/pixel probes from
`3531f0ee`; the original frozen UMD has a captured Explorer abort on that
instruction. `6c854e9f` restores execution-ordered inherited-state validation
from `760d8405`; its prior Windows regression demonstrates the old rejection of
a valid queued MME packet. These address demonstrated defects without importing
the extra `.155–.156` diagnostic machinery.

The production/test changes are based on the frozen source plus the sampler-view
and GR helper-load corrections above. All Mesa `src/` files match `ee7b0d50`, so
the existing UMD `77dea046dde0` and its twelve assertion-enabled translator
checks remain applicable. The intended ICD is unchanged `6e2fad99d742`.

The new candidate uses `.158` to distinguish it from the already signed `.157`
package. Its host regressions, actual NETB stream checks and full WDK ARM64
build pass from source `6c854e9f394c3d761fab85885bead4bbd4588581` in
`stabilize158-20261112`. The Windows ARM64 context model exits 0 at 03:33:15
UTC. Signed SYS `46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088`
and all 13 catalog members verify successfully.

## Native .158 observation

`stabilize158-native-20261112` completes the 90-second screenshot sequence.
All four images show the complete bottom taskbar and right-side tray, and the
user reports normal, comparatively stable physical output. Each image retains
`.158` Code 0, Explorer PID 4472 and the composition content bridge. DWM selects
Tegra LUID `bc22f` at FL10_0. The finalized ETL contains 79 successful DWM flip
Presents across 100.5441046 seconds; every native scanout address matches a
subsequent VSync. No native-era device error, UMD failure or KMD recovery is
retained, and the exporter reports no lost events or buffers.

All 184 returned files (100,622,505 bytes) match device hashes. The scheduled
recovery returns to `.59` Code 0 with cleanup and unchanged power verified at
03:48:56 UTC; portrait verification at 03:49:33 is a no-op. The recovery PNG
at 03:51:05 shows a complete taskbar. Keep this exact component set for an
extended desktop/menu interaction trial. This short observation does not prove
long-duration stability, Start-menu popup behavior or hardware landscape.
