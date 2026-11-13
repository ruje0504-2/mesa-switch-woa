# Fragment helper-invocation loads

Fable 5.2/max independently identified a missing GM20B context policy and
committed `52e35569` on its VPS clone. The main agent fetched that source
over SSH, verified the upstream basis and actual firmware input, and added
write-readback verification. The `.153` stabilization branch carries the
port in `d2edb249` and its checked form in `e447a1fe`. This correction does
not establish the cause of the retained `.155` GPU stall or fix the taskbar
without native validation.

## Evidence and correction

NVK's `nvk_push_draw_state_init` clears bit 3 of `0x419f78` for helper-lane
memory loads through `NVK_MME_SET_PRIV_REG`, but the Windows build excludes
that call and does not allow the privileged macro. The KMD did not establish
the policy in the golden context. Linux nouveau's
[gk110_grctx_generate_r419f78](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/nouveau/nvkm/engine/gr/ctxgk110.c)
performs the corresponding mask update during context generation; GM107 uses
that helper as well.

Independent parsing of the exact external NETB finds a context-stream entry
at region-5 offset 5,196 (file offset 46,506): address `0x419f78`, index 0,
value `0x10beb`. The production `context_stream` writes that entry's value.
Bit 3 is set in that input; clearing only it gives `0x10be3`.
NETB SHA256: `5b285e5de99b935c2619e54b1656b8a2f271ba845e80952faadfd09960d6eea3`.
This is static initialization evidence, not a live register readback.

The KMD now enables the helper loads after the context/bundle/method streams,
while the temporary context is bound, and before FECS saves the golden image.
All application contexts inherit that policy. It preserves other bits,
rejects failed register reads, and checks that a requested write took effect
before permitting the save. GPU completion and error reporting are unchanged.

## Validation and limits

The updated model checks the order relative to the vendor streams and golden
save, preservation of unrelated bits, an already-enabled state, failed reads
and an ignored policy write. It accepts the real NETB's initial write before
requiring the policy correction. The full host suite, including that exact
NETB input, and the WDK ARM64 `.157` build pass on the `.153` stabilization
worktree. Independent negative controls fail at the golden-save assertion
against original `.153` source and the remote source without write-readback
verification (in the ignored-write case).

The signed `.157` package, including the embedded SYS/stub signatures and all
catalog members, is verified on Windows at 03:12:34 UTC on November 12, 2026.
It is **not installed**. Signed SYS SHA256:
`26dcb539364b83c7fbde76a41b9312b1046dae76170d3d026e8045a4a41dbb8f`.
Its source is `e447a1fe`, based on `.153`; it does not include `.154–.156`
queue/diagnostic changes merely because those remain in main. This package
has no live GPU or complete taskbar pass.

External evidence: `helper-load-independent-20261112/netlist-helper-load.json`,
the archived upstream files, and `helper-load157-20261112` build, negative
controls, source/signing/catalog records. Fable's broader review continues.
Its second local commit `9092e512` changes shader-exception reporting policy;
that separate change is fetched for review but is not part of this candidate.

## November 13: shader-object output exception policy

The frozen `.158` KMD (`6c854e9f394c3d761fab85885bead4bbd4588581`)
already contains the helper-load correction above. Carbon High's revised
Antigravity audit identified a separate omitted policy: NVK also clears bit 14
of `gr_gpcs_tpcs_sms_hww_warp_esr_report_mask`, at `0x419e44` on GM20B.
The reference is the MIT-licensed
[fixed mesa-switch NVK draw-state initialization](https://github.com/danfromtico/mesa-switch/blob/0ff52079ca9e9e837ed27eb95a8d239f439a1864/src/nouveau/vulkan/nvk_cmd_draw.c),
also present in the Windows ICD source `7c677212`. It applies to MAXWELL_B
and later, with different register addresses from Volta onward. This KMD
implementation is specific to its GM20B context generator.

NVK documents the policy for independently compiled shader stages with unused
outputs, including shader objects. Its explanation of the hardware behavior is
explicitly conjectural, informed by SPH documentation and an OpenGL CTS case.
It is not proof that the owner's weather/browser failures hit this exception.
The Windows ICD excludes the privileged macro; the frozen KMD does not provide
the corresponding context initialization.

Independent parsing of the exact NETB with SHA256 shown above finds region 5,
address `0x419e44`, index 0, value `0x00d3eff2`: bit 14 is set. The new KMD
source clears only that bit after the vendor streams and helper-load setup,
before FECS saves the golden context. The resulting mask is `0x00d3aff2`.
Unrelated exception bits are preserved; unreadable registers or a failed
write/readback prevent saving an invalid context. This differs from the
earlier Fable proposal `9092e5126cb87389cf195e2e1ba890e8eab49170`, which
zeroed both warp and global masks; that broader change was not imported.

The model with the real NETB input fails at the golden-save assertion against
the preceding source and passes with this correction. It checks save ordering,
preservation of other bits, the already-cleared case, unreadable registers and
ignored writes. The changed ARM64 kernel translation unit compiles with WDK
10.0.26100.0 and the production kernel flags. Outputs are retained under
`shader-output-context-controls/`; these checks do not execute GPU work.

No new signed KMD has been built or deployed for this change. The current
physical test still uses the original `.158` SYS with SHA256
`46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088`.
Its five-minute desktop result belongs to that unchanged KMD, not this patch.
Any subsequent KMD trial must preserve the frozen baseline outside this
specific policy change and record the new source and binary hashes.
