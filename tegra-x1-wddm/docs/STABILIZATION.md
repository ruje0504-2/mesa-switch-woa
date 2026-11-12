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
already been installed. Fable 5.2/max independently reviews current main on
the VPS; its branch must not push to GitHub. The main agent will retrieve and
review its commits and port only applicable fixes. That review does not block
this stabilization work.

The initial frozen-stack retry reaches Code 0 but is interrupted by another
boot before its PNG completes; `.59` recovery is verified. The retained MPO
submissions contain one full-screen plane with matching VSync addresses,
while native shell geometry shows an empty Start child. See the
[MPO contract and observation audit](MPO_CONTRACT_AUDIT.md). This branch is
still a stabilization starting point, not a stable-driver release.
