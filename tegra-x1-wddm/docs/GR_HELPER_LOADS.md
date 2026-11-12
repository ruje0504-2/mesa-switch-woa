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
