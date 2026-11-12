# Native desktop observation with .158

On November 12, 2026, `stabilize158-native-20261112` completes the first
retained 90-second native desktop screenshot sequence with the full taskbar.
All four original PNGs show the bottom Start/pinned icons and right-side
network, audio, battery and clock area in their expected positions. The user
also reports that the physical desktop appears normal and comparatively stable
during this observation. This is a short desktop observation pass; sustained
stability and interaction coverage remain incomplete.

## Exact components

| Component | Source or SHA256 |
| --- | --- |
| KMD source | `6c854e9f394c3d761fab85885bead4bbd4588581`, `stabilize/native-desktop-153` |
| Signed KMD `.158` | `46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088` |
| D3D UMD | `77dea046dde0777d6c67e62769c983c5249595a9cc9e3dd7f8323805854f9cdf` |
| NVK ICD | `6e2fad99d742a69033cc59925093fb4a7c8489728a3f7e5e9ebb523c017e0323` |

The candidate starts from the selected `.153` rendering baseline. It includes
the sampler-view operand correction, SAMPLE_I lowering, execution-ordered
inherited-state validation, and the GR helper-invocation load correction with
write-readback verification. It excludes the `.155–.156` additional diagnostic
instrumentation, the remote SM exception-policy change and unfinished worker
timer changes. Its Mesa `src/` matches `ee7b0d50`, retaining the previously
verified UMD and its twelve assertion-enabled translator checks.

Host regressions, the actual NETB initialization model and the WDK ARM64 build
pass. The Windows ARM64 context fixture exits 0 at 03:33:15 UTC while `.59`
remains Code 0; executable SHA256 is
`3f12dc372d0150ae333a6c4e2ec3cfc713492b7de8998c5cf6cc4d400f0607c0`.
Embedded signatures and all 13 catalog members pass verification. These
controls precede the native observation; they are not hardware results.

## Desktop and presentation evidence

Native startup records Code 0, hardware status 0 and a consumed one-shot
permit at 03:42:23 UTC. The observation uses Tegra LUID `00000000000bc22f`,
720x1280 portrait and DPI 96. The native task exits 0 at 03:44:19 after captures
scheduled at 0, 15, 45 and 90 seconds. All captures record `.158`/Code 0 before
and after, with the same Explorer PID 4472 and a visible desktop composition
content bridge. The taskbar bounds remain `(0,1232)-(720,1280)`.

| Capture | PNG SHA256 |
| --- | --- |
| 0 seconds | `1bcad00c93a91fbc9e5c4cdc16fb3c2d720bac8ec6fa548a7220369b606ee38e` |
| 15 seconds | `156784932e878d8c62db53bdebf3d4fb118777da2d534f800bd47000dac04715` |
| 45 seconds | `156784932e878d8c62db53bdebf3d4fb118777da2d534f800bd47000dac04715` |
| 90 seconds | `b5eaea08246ac592e84172729c25c0b6825c0106eea824ba47b4853f7f5996f0` |

The finalized ETL contains 266,522 exported records, including 23,802 UMD
events. The exporter reports `invalid=0`, `events_lost=0`, `buffers_lost=0`, a
nonzero EndTime and 1,161 written buffers. Its binary export SHA256 is
`a8ee812eeed8aaad1e240dcfa872535d84b3a3a3e37a97913ecbd1288236b679`.

Dwm-Core's hardware-device event selects **Tegra X1 (GM20B)** at LUID
`bc22f`, Feature Level **10_0**. The same DWM PID 768 records successful Zink
initialization for that LUID and **79 successful flip Presents** across
**100.5441046 seconds** of ETW time. All 79 native MPO requests use one full
720x1280 plane; every flip address appears in a subsequent native VSync before
the next flip. The trace retains 8,128 native multi-plane VSync records.

No UMD RuntimeError/ZinkFailure or KMD recovery record is retained. The two
`VidSchMarkDeviceAsError` records, with numeric reasons 14 and 6, both precede
native `DdiStartDevice`; their reason values remain uninterpreted. There is no
such record after native start in this trace. One known Dwm-Core event 203
payload/template mismatch excludes 1,297 events from the decoded analysis;
their contents are not inferred.

## Recovery and scope

Collection completes normally at 03:44:50, followed by the scheduled `.59`
recovery. The only boots in this trial are the setup boot at 03:39:55 and the
planned recovery boot at 03:46:07. At 03:48:56, `.59` Code 0, original ACPI,
temporary-registration/task cleanup and unchanged power settings are verified.
Saved portrait verification is a no-op at 03:49:33. The restored desktop PNG
at 03:51:05 also has a complete taskbar and tray, SHA256
`e748bc62ca950626e0c0c8695b2962361fa468e84c5a116e71dab02fc837ff3c`.

All **184 files / 100,622,505 bytes** in the returned trial evidence match
device-side hashes and lengths. Native images were fetched early and match
the subsequently collected originals. Evidence is retained in the external
build root under `stabilize158-native-20261112` and
`desktop-after-stabilize158-20261112`; `screenshot-summary.json` and
`presentation-summary.json` separate image inspection from ETW analysis.

The first visible improvement does not isolate a single cause: this candidate
combines several independently justified fixes. It does not establish the
Start-menu popup's behavior, interactive stress, long-duration stability,
hardware landscape, Win32 vkcube, full Vulkan conformance or game compatibility.
Keep this exact component set for the next extended desktop/interaction trial
before changing rendering behavior. Main retains later diagnostic source for
future use; it is not the exact `.158` build tree pinned above.
