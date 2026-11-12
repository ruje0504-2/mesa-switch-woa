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
| KMD source | `6c854e9f394c3d761fab85885bead4bbd4588581`, retained under [baseline/native-desktop-158](https://github.com/ruje0504-2/mesa-switch-woa/tree/baseline/native-desktop-158) |
| Signed KMD `.158` | `46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088` |
| D3D UMD | `77dea046dde0777d6c67e62769c983c5249595a9cc9e3dd7f8323805854f9cdf` |
| NVK ICD | `6e2fad99d742a69033cc59925093fb4a7c8489728a3f7e5e9ebb523c017e0323` |

The former `stabilize/native-desktop-153` development branch is merged into
`main` and retired.
The archival tag `baseline/native-desktop-158` retains its final source and
observation record at `6cff3356d465bc87e9bbd7fb73ea05b4e7503617`, including
the exact KMD commit above. This tag preserves the historical short trial;
later interaction showed visual corruption, so it is not a stable release.
See [current status](STATUS.md) for subsequent work and results.

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
Main retains later diagnostic source for future use; it is not the exact `.158`
build tree pinned above. The later attempts below supersede the instruction to
repeat the frozen set before further source work.

## Later attempts

`stabilize158-extended-native-20261112` plans 300 seconds of desktop/menu
observation followed by a separate 32-MiB VidMm query. It retains twelve capture
records through 195 seconds, then returns failure after the 210-second capture
times out. The budget probe is not reached. Its archived recovery record confirms
`.59` Code 0, no remaining trial tasks and unchanged power at 05:00:29 UTC.
These records do not establish a completed sustained-interaction pass.

`native-vkcube158-20261112` uses the exact KMD, UMD and ICD hashes listed above.
It plans unmodified Khronos `vkcube` at 256x256, FIFO and 360 frames, desktop
captures, then the separate 32-MiB VidMm probe. The executable is the previously
built Vulkan-Tools `1d3fbab1ff5abce9f3eb9f3e7fcf66f455ae4475` sample, SHA256
`69ca28a36d20da41b6643573e0fe28527109e7d03454000696aa31002402ca38`.
The trial preserves portrait and requests no hardware rotation.

The initial setup boot is 07:03:03 UTC on November 12. Native startup records
Code 0, hardware status 0 and a consumed permit at 07:05:29. There is no retained
`mode-stage.json`, `cube-window.json`, `cube-result.json`, `native-result.json`,
native PNG or memory-probe result. Consequently even cube execution is
unproven. The user reports two automatic restarts; no cause is assigned or
investigated. The repeat-start guard records recovery at 07:08:43, restoration
is staged at 07:09:23, and the recovered OS boot is 07:09:57.

At 07:14:04 UTC, `.59` Code 0, the original ACPI hash, removed test permit,
temporary ICD/UMD registrations, loader/debug environment and task cleanup,
and unchanged display/sleep settings are verified. Saved 720x1280 portrait is
unchanged at 07:14:19. The recovery PNG at 07:15:22 shows the complete taskbar
and tray, SHA256
`9d24d4255750c0202cb6fbd9a51367ab5569d7f120ad5992970c8aef25137c30`.
This is the recovered software desktop, not a native test image.

All **137 returned files / 113,708,587 bytes** match device hashes and lengths.
The records are retained under the main checkout's
`build/native-vkcube158-20261112/evidence/`; the host-side user observation and
summary are alongside it. No native test remains active. Continue source work
and non-device checks; hardware **1280x720 landscape** still needs its own
asymmetric-image, dimensions/pitch, latch and physical-output verification
before another cube acceptance run.

## Five-minute observation and physical corruption

`stability158-five-minute-20261112` uses the exact frozen components above.
Native startup is Code 0 at 07:25:17 UTC on November 12, LUID `a79e7`.
Observation begins at 07:25:35 and finishes at 07:30:47 with exit 0 and
311.0569514 observed seconds. All six scheduled captures (0, 60, 120, 180,
240 and 300 seconds) record `Shortcut=None`; this script sends no shortcut
or click inputs. The timed window retains no intervening reboot.

The user supplies four physical photographs and reports tearing, stalls and
heat. Two photographs clearly show large white polygons covering Quick
Settings. **Visual acceptance fails despite the completed timed observation.**
Neither the photographs nor the timer result isolate a stride, shader,
translation or synchronization defect. Do not assign the cause to Zink or
stride without evidence from the responsible path.

The planned recovery boot is at 07:32:34, after the observation completes.
At 07:35:54, `.59` Code 0, original ACPI, test-registration/task cleanup and
unchanged power settings are verified. Portrait remains unchanged at 07:36:12.
The recovered desktop PNG at 07:37:04 has SHA256
`09833d9a0af32e410a1f6da7f79d767ef0ac9d33c1f2954c365d6f97cf8ab620`.
It is recovery evidence, not an additional native result.

All **196 files / 112,332,491 bytes** match the device-side manifest. The
unchanged physical-photo originals and their hashes are retained separately
under `build/stability158-five-minute-20261112/physical-photos/`; collection
and observation records are alongside `evidence/verified.json`. Hardware
landscape and vkcube have not been advanced past this failed visual gate.

The 300-second system PNG also contains white triangular corruption in the
Quick Settings Bluetooth tile, SHA256
`51ae7ecb7fc66edf036403d0a319cf5cfe6dde34ca15e679016e52781d68ab3a`.
The defect is therefore observable in the captured desktop image as well as
on the physical screen. This does not isolate its rendering/shared-storage/
synchronization cause, and does not support treating it solely as a final
display-controller stride problem.

Normal-presentation analysis covers a retained **243.6166229-second slice**
of the ETW export, not the entire 311-second observation. DWM PID 440 has
796 DXGI Present entries, all `SyncInterval=1`, and 795 successful returns.
All 795 native flips use interval 1. The single adapter's 14,606 VSync records
have spacing p50/p95/p99 of 16.6664/16.6854/16.7083 ms. These records do not
show an absent VSync limit. Excluding one ambiguous overlapping entry, the
794 paired DXGI Present calls take 25.084/130.6247/265.581 ms at p50/p95/p99
in wall-clock time. This is not CPU
execution time, GPU time or full-frame latency: future performance comparison
must separate rendering, fence waits and presentation queuing. Counting these
mixed idle/interactive Presents is not a throughput benchmark. The decoded
export and `presentation-pacing-summary.json` remain under the trial directory;
the known Dwm-Core event-203 payload mismatch excludes 1,803 records
from decoded analysis.

## Matched-component retest: failed onscreen acceptance

The later November 12 run `desktop158-zink-sync-20261112` is **not an
onscreen success**. The owner explicitly reports no successful native
output. No `mode-stage.json`, native pixel result, native desktop PNG or
`native-result.json` is retained. The five-minute observation never reaches
a retained result; hardware rotation and `vkcube` are not attempted.

This is a different component combination from the historical short pass:

| Component | Source | SHA256 |
| --- | --- | --- |
| Frozen KMD `.158` | `6c854e9f394c3d761fab85885bead4bbd4588581` | `46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088` |
| Zink UMD, shared-image view usage and submission bridge | `accd9f1e5a9f1c649536e400828b7428e8aca367` | `8a1a765de323906bfef4f2c13c8d7bb60527e89d49667928efc5a94eceee3ffe` |
| NVK ICD, shared timelines | `7a771aa6602d2c94cb4cd362855777290a2503f2` | `ad6865784890f937a7c29aab0f108841b6d99cbffdca985b7dafbffc1a953625` |

The staging tool is retained in main `befdd503`. It replaces an obsolete
staging script that expected an old ICD hash and reused old UMD control
results. At 16:18:09 UTC the current ARM64 surface control passes all 16
cases, exit 0 in 1300 ms; it uses Vulkan substitutes and performs no GPU
rendering. All 33 scripts parse in Windows PowerShell, and the closed-gate
and interactive-desktop preflight checks pass on `.59` / Code 0.

Retained controller sequence (UTC):

- 16:20:56: `.158` is installed as `oem89.inf`, Code 43 behind the closed
  startup gate. The planned setup boot follows at 16:21:39.
- 16:23:42: the automatic controller starts. The one-shot native start
  request is issued at 16:23:54. The retained startup completion at
  16:23:58 records `Hardware=0`, `Problem=0`, `Permit=0`; this proves neither
  drawing nor physical output.
- 16:24:26: a new boot is observed before the native pixel/capture stage
  has produced records. No completed restoration record precedes this boot;
  it must not be described as a completed test or the planned final recovery.
  This record does not determine its cause.
- 16:26:34: the existing attempt marker selects recovery rather than a
  second native-start attempt. The restore task is observed running at
  16:26:49 and writes `restore-staged.json` at 16:26:56.
- 16:27:29: the planned recovery boot. At 16:28:50 the device is `.59` /
  Code 0 with no remaining `TgGpuDisplay158*` tasks.
- 16:31:39: full recovery verification completes. The `.59` SYS and ACPI
  hashes match the originals. Temporary ICD/UMD registration, system loader,
  opt-in/debug environment, startup permit, grant and tasks are cleared or
  restored. Power reports match; saved display preference verifies unchanged
  on BasicDisplay, 720x1280. CPU remains 1996.8 MHz, fan version 12 and Wi-Fi
  0.5.7.0 / Code 0.

The owner's earlier report of a normal desktop was not tied to successful
native presentation and is not used as acceptance. Their subsequent explicit
report of no native onscreen output establishes the failed physical result.

The retained UMD export contains 28,331 records. Its 28 failure-bit statuses
are residency `E_PENDING` results; runtime-context signal and wait callbacks
each return success 18 times. This is evidence that the callbacks were
accepted, not that GPU dependencies completed or pixels reached the panel.
The interrupted ETL has no completed end timestamp, so its tail cannot be
treated as a complete execution history or used to identify a restart cause.

All 134 collected files, totaling **98,331,502 bytes**, match device hashes
and stream lengths. Directory enumeration reported different lengths for
`loader-debug.csv` and `trace-flush.bin`; a second device-side measurement
through open file streams confirms the downloaded lengths and hashes. Both
the original manifest and correction response are retained under
`runs/desktop158-zink-sync-20261112/` in the local build workspace.

Before another hardware trial, audit the new submission/shared-image path
against actual runtime/KMT contracts. A later five-minute visual acceptance
must exercise the known Quick Settings corruption case as well as desktop
captures; an idle-desktop sequence alone does not cover that regression.

## Present interval audit

The retained UMD exports distinguish DWM scanout requests from application
Blt requests:

| Trial | DWM Present calls | Flags | Requested interval | Other Blt calls, interval 0 |
| --- | --- | --- | --- | --- |
| Historical short `.158` observation | 79 | Flip (`2`) | One VBlank (`1`) | 20 |
| Failed matched-component retest | 5 | Flip (`2`) | One VBlank (`1`) | 13 |

All of these retained callbacks return `S_OK`. Microsoft's
[flip-interval definition](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ne-dxgiddi-dxgi_ddi_flip_interval_type)
defines interval one as a VSync-paced flip. `RuntimePresent` forwards the
opaque DXGI context unchanged and leaves the interval-override fields unset,
following the [runtime callback contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgiddicb_present).
The records therefore do not demonstrate that DWM requested unbounded
immediate flips. Interval-zero application Blt callbacks must not be counted
as immediate physical panel flips. These counts do not establish actual
scanout frequency, GPU completion, correct pixels or sustained presentation;
the failed trial remains failed. They do not justify adding a CPU timer to
every DWM Present in place of verifying the existing synchronization path.

## Import-content desktop trial

`desktop158-import-content-20261112` is a new candidate combination. It keeps
the frozen `.158` KMD (`46d17576…`) and timeline ICD (`ad686578…`) identified
above. Its UMD comes from `ca4053872319e81f71207c1021fbc4f6133f6b4f`,
SHA256 `0e840c55f61cc58cee11c00b36c1bce2586d577de5f8e385a09f8ea4bb6a0f02`.
The runtime pixel probe has SHA256
`5747dff91b8049f1d9476f731f779ae3c2dd0832c41c0952542f8e84b0a0a57f`.
This adds preservation of imported image contents; the preceding failed
retest used a different UMD and remains a failed onscreen result.

The surface control and registered-loader preflight pass on `.59`, with
all 28 trial scripts parsed by Windows PowerShell. At 18:49:12 UTC, the
interactive preflight verifies the saved 720x1280 portrait preference
unchanged and a visible taskbar window at the expected bounds. These are recovery-configuration
checks, not native desktop acceptance. The candidate binds as `.158` /
Code 43 behind the closed startup gate at **18:50:41**. UMD/ICD registration
and the planned setup reboot request follow at **18:50:49**.

The worker first requests a native desktop capture, then ordinary and
sampled D3D pixel checks and `--shared-layout-load`, without producer/import
CPU readback checkpoints. A subsequent 300-second observation requests
desktop/Start captures and five Quick Settings open/dismiss cycles. It uses the
previously exercised shortcut capture helper, SHA256
`ff22b95bba31e37c74e82e967e651ed14cfc8dcc4a368cbdc10c51b89fd15351`.
The existing recovery removes the native task and this trial's temporary
ICD registration, including interruption before normal collection.

Records recovered after SSH reconnect establish the following, separately
from the device's later `.59` recovery state. All times below are UTC.

- At 18:53:38, the active display path and native KMT adapter both have LUID
  `000000000008239d`, `.158` / Code 0. DWM PID 776's loader record at
  18:53:33.865 identifies this trial's NVK DLL and GM20B device.
- The ordinary draw, sampling and shared-load probes use that native LUID
  and the identified UMD/ICD. They match **16,384**, **24,576** and
  **3,686,400** pixels respectively, with zero mismatches. The shared-load
  case preserves the producer's contents across an imported partial blend,
  with no intermediate producer/import CPU readback. These are **offscreen
  GPU results**, not a physical presentation acceptance.
- The capture worker runs from 18:53:43.193 to 18:58:55.725. Its 300-second
  observation begins at 18:53:51.068 and retains 15 images, including the
  initial pre-probe image. Their metadata records `.158` / Code 0. DWM PID
  776 remains the same in the first and last process snapshots. The owner
  identifies the partially opened Quick Settings images as transition
  animation; they must not be called texture corruption from those stills.
- The retained UMD trace has 157 successful DWM Flip/interval-one callbacks,
  spanning 18:54:22.006–18:58:43.910 (261.904 seconds), with matching kernel
  MMIO flip records. The 17,413 retained hardware VSync events have a median
  interval of 16.6666 ms. This supports native presentation activity and
  approximately 60 Hz VBlank timing, not 60 rendered frames per second.

**Five-minute physical desktop acceptance is still unverified.** The worker
explicitly records `PhysicalPixelsUnverified=true`; screenshot collection
completion is not that acceptance. The ETL reports **121,523 lost events**,
so neither missing records nor the retained flip span prove an uninterrupted
five-minute history. The old ShellExperienceHost PID 4636 also records 35
failed Present calls (`88760870`), 74 teardown device-removed reports and
one resource-create `8007000e`; a later PID 5848 has 13 successful Presents.
The submit-side error needs attribution before declaring the desktop stable.
No restart cause is inferred from these records.

The controller records collection complete at 18:59:17.210, later takes its
existing-attempt recovery branch at 19:02:20.801 and stages `.59` at
19:02:39.567. The recovery boot is 19:03:14.227. At 21:13:25, the original
SYS/ACPI, temporary registration/loader/environment cleanup, removed tasks
and unchanged power settings verify. The 21:22:25 check additionally verifies
CPU 1996.8 MHz, fan version 12, Wi-Fi 0.5.7 / Code 0, removed trial loader
registry value and the saved portrait preference. **`.59` / Code 0 is the
post-trial BasicDisplay configuration, not the native test configuration.**

All **244 files / 137,721,253 bytes** match device-side hashes and lengths
under `runs/desktop158-import-content-20261112/evidence/` in the external build
workspace. SHA256 identities:

| Record | SHA256 |
| --- | --- |
| Retained archive | `b20f70adb297ca4cb385784e396f7f0886c935fd38383d0261bb28d298d58fff` |
| Worker result | `285143c5626201aab65d76aee1ac4c1832f91f72a0ca0f3b2b9d12f30c9d3b51` |
| Shared-load pixels | `e0dc93d5ff2cf58647205571df1d958888de827cfd1f707f965c8d84000240f6` |
| Active-path observations | `ce2191026a1898fe43f996c1ba8afa59c5975d1d29478009a900a9a22e2100c8` |
| Recovery verification | `3f0ccca0f26fc6196dab4f5d857e8c670ae0f872265ea52bbeae164205bc9e4b` |

The next observation is explicitly onscreen-only. Before starting its timer
and around each capture, `Assert-NativeDesktop.ps1` must retain a fresh active
source/target LUID matching native GM20B, the expected KMD/Code 0 and the
current DWM process's loader record for the trial ICD. A BasicDisplay desktop
cannot satisfy this check. Its records still do not replace physical screen
observation. Reuse the offscreen result above instead of counting another
offscreen probe as progress on presentation.
