# Native desktop .160 trial

The November 12, 2026 `.160` trial is **interrupted and not accepted**.
It has no retained native pixel-readback, desktop-capture or `vkcube` result.
The device is restored to `.59` / Code 0. This record describes the observed
test sequence and recovery; it does not identify a restart cause.

## Components

| Component | Source / SHA256 |
| --- | --- |
| KMD `.160` | main `7a771aa6602d2c94cb4cd362855777290a2503f2`; signed SYS `e271fdf4e981260263523ad6481186df66b120594fa07bcd6670a192ed23294a` |
| Zink D3D UMD | `177a6c1dfabb3623a578d4f078594e15e69b2bbb`; DLL `9d1e5ff34fb298559eb97e780aedf31a52498e046d3c91ce8f749a3ddf19b23f` |
| NVK ICD | `7a771aa6602d2c94cb4cd362855777290a2503f2`; DLL `ad6865784890f937a7c29aab0f108841b6d99cbffdca985b7dafbffc1a953625` |

The UMD includes shared UNORM/sRGB storage, persistent shader caching and
the new [runtime/NVK submission bridge](D3D_RUNTIME_SYNC.md). The KMD catalog
and all members pass Windows signature verification. Five ARM64 CPU/API
controls pass on `.59` before installation; their KMT/Vulkan substitutes do
not establish hardware behavior of this component pair.

## Retained sequence (UTC)

- 14:49:53: `.59` / Code 0, interactive desktop preflight passes. CPU readback
  is 1,996,800 kHz; fan driver is version 12. Sleep/display policy is captured.
- 14:51:46: PnP installs `.160` as `oem88.inf`. It remains Code 43 behind
  the closed startup gate; PnP installation success is not GPU startup success.
- 14:52:25: the planned setup boot. The automatic controller begins at
  14:54:24, then records the one-shot start request at 14:54:33.
- 14:55:05: a new system boot is observed before a usable native result is
  retained. `demand-result.json` (586 bytes) and `startup-completion.json`
  (113 bytes) contain only zero bytes, not valid result objects. They cannot
  prove Code 0, hardware initialization, or rendering success.
- 14:56:30: `.160` is again Code 43 behind the closed gate. No
  `mode-stage.json`, `native-progress.json` or `native-result.json` is retained.
- 14:57:12: the repeated controller invocation sees its existing attempt
  marker and selects recovery instead of another native start. The restoration
  task is confirmed running at 14:57:25; restoration is staged at 14:57:35.
- 14:58:06: the planned recovery boot. At 15:00:42, `.59` / Code 0 is
  confirmed with no remaining `.160` trial tasks.

The planned workload was ordinary D3D drawing, sampling, shared-layout pixel
checkpoints and a 90-second desktop observation. No completed result for
that workload exists. The user did not identify an observation of the
physical screen for this run, so physical appearance is also unverified.

## Recovery and evidence

At 15:02:21, the installed `.59` SYS matches
`de5e9cd492d35a73ea3d3f5f1723fd82f88922f6e91ef5966b430360f5eb5646`,
and the ACPI table matches the original
`b829b50944a90745fb6f6f7f44f453a6b8573fe4019a95fefd0013a983703518`.
The startup permit, volatile grant, temporary ICD/UMD registration and
temporary system Vulkan loader are absent. The before/after power reports
are identical; CPU remains 1996.8 MHz and fan remains `1.0.0.12`.

At 15:04:55, the NVK opt-in and loader-debug environment values match their
saved state. At 15:04:57, saved display-preference restore/verify both return
0 with `changed=false` on BasicDisplay, 720x1280, DPI 96. This is restoration
evidence, not native landscape validation.

All 106 collected records, totaling 172,477 bytes, match device lengths and
SHA256 values. They are retained locally under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/desktop160-native-20261112/evidence/`.
The component manifests, scripts and original failed/incomplete records are
preserved. No restart-cause analysis or game-specific debugging was performed.
