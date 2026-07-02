# Blend Mode — Investigation & Design Spec

Status: **investigation — no implementation**. Written 2026-07-02, against the
`decoupled-capture-present` baseline (dual-device ring, spec:
`dual-device-capture-present-spec.md`). Successor item 3 in that spec's ordering; blocked on
successor item 2 (adaptive bracketing delay).

## Purpose

Replace nearest-pick's "choose before OR after" with a weighted combination of both bracket
frames, presented at the output instant. This is the first interpolator: it removes selection's
period-2 stride quantization (the known 240→60 judder — `temporal-capture-mode-spec.md` R4) at
the cost of motion blur proportional to the bracket gap. It is a stepping stone: the *shape* of
the change (consume `FrameBracket{before, after, w}` → produce one frame) is identical to the
NVOFA stage that replaces it, so everything structural built here carries forward.

## What the ring already provides

`FindBracket()` was designed for this and needs **no changes**:

- `beforeTexture` / `afterTexture` — present-device texture aliases (ring slots are
  `D3DUSAGE_RENDERTARGET` textures specifically so a shader can sample them — capture-ring
  design comment says so explicitly).
- `weight = beforeDiff / (beforeDiff + afterDiff)` — already computed and logged as `w=` in
  every t-mode run. Blend presents `lerp(before, after, w)`.
- Hysteresis/monotonicity semantics carry over unchanged (see Selection semantics below).

## Lessons already paid for (do not re-derive)

From `temporal-blend-future-frame-findings.md` (the pre-ring blend review):

1. **The old blend's *principle* was correct** — it gated presents on a post-target frame
   existing and genuinely interpolated. What was broken was its *foundation*: polled NOWAIT
   capture → no honest timestamps → garbage weights. The ring fixes exactly that. The old mode
   was deleted for its foundation, not its blend math.
2. **The shader machinery is proven and salvageable.** `FrameBlendCaptureMode.cpp` @ dev
   (`cf28f40`, deleted on this branch, retrievable via
   `git show dev:samples/NvFBC/NvFBCR/FrameBlendCaptureMode.cpp`) contains a complete, working
   vs_3_0/ps_3_0 pipeline: fullscreen-quad vertex buffer + declaration, two-sampler pixel
   shader doing `lerp(t0, t1, c0)`, `D3DCompile` at setup with error logging, sampler/render
   state setup. Port the mechanism; leave its capture/history logic dead.
3. **Bracketing latency is inherent, not a bug** — interpolating across T requires a frame
   past T; the output runs ~1 present period behind. Accepted under the T10 latency policy
   (constant offset in service of pacing).

## Prerequisite: adaptive bracketing delay (successor item 2 — hard blocker)

Nearest-pick degrades gracefully when the after-frame is missing (repeats). Blend cannot: no
after-frame means nothing to blend toward, every miss is a fallback. Today's lag is one
*present* period (16.7 ms), which assumes source ≥ present rate; at 30-base FG the after-frame
usually doesn't exist. Required first:

- `lag = max(presentPeriodQpc, ~1.25 × measured source period)`.
- The measured source period comes from the capture side (it already computes `dt` per wake);
  expose it across the seam as one number — e.g. `CaptureRing::EstimatedSourcePeriodQpc()`
  (EMA over recent non-intra-batch gaps, capture-thread-written, atomically published).
  This is the *only* new crossing of the capture→present seam.
- Fallback policy when the after-frame is still missing at present time (source stall,
  regime change): present `before` unblended (weight clamps to 1.0 — `FindBracket` already
  returns `w=1.0` for the no-after case). Log it (`pick=stall` or similar) so validation can
  reconcile.

## Implementation options considered

### Option A — pixel shader lerp on the present device (RECOMMENDED)

Fullscreen quad, `ps_3_0`, `SetTexture(0, before)` / `SetTexture(1, after)`,
`SetPixelShaderConstantF(0, &w)`, draw into the backbuffer, `PresentEx`. This is exactly the
salvaged machinery. Cost: one full-screen textured draw per output frame — trivially sub-ms at
1440p, constant → satisfies T10.

Pros: proven code exists; renders directly at 10-bit backbuffer precision; the shader is the
natural place for gamma-correct blending later; the same draw-a-quad scaffolding is needed by
any future shader work.
Cons: brings `D3DCompile` (and the `d3dcompiler` import that Defender-watchers may note
reappears — cosmetic).

### Option B — fixed-function constant-factor alpha blend (no shaders at all)

Two passes: draw `before` opaque, then draw `after` with
`D3DRS_BLENDFACTOR = w` (`D3DBLEND_BLENDFACTOR` / `D3DBLEND_INVBLENDFACTOR`). No D3DCompile,
no shader objects.

Pros: smallest possible surface; no shader compile at startup.
Cons: needs `D3DPBLENDCAPS_BLENDFACTOR` caps check; two passes instead of one; blend happens
in the output-merger (fine at 10-bit RT, but no path to gamma-correct math later); still needs
the quad/vertex scaffolding, so it saves less than it appears to.

### Option C — StretchRect tricks

None exist. `StretchRect` has no blending. Rejected.

**Recommendation: A.** The scaffolding cost is shared with everything that follows (NVOFA
output composition, any debug visualization), and A is the only option that can grow the
correct color math.

## Color math (open question — resolve by measurement, not assumption)

Naive `lerp` in the stored encoding is only correct if the encoding is linear. Ours is not
known precisely:

- The pipeline runs `NVFBC_TODX9VID_ARGB10` with `bHDRRequest = TRUE` into
  `D3DFMT_A2R10G10B10`. What transfer function the captured pixels actually carry (SDR gamma
  in 10 bits? PQ when Windows HDR is on?) has never been characterized — capture something
  with known gradients and check.
- If content is gamma-encoded, gamma-space lerp slightly darkens mixed regions (classic
  crossfade dimming). For 50/50 blends of adjacent video frames the visual error is small —
  likely acceptable for a first ship, and *measurably* checkable (blend of identical frames
  must be identity; blend across a step edge should not produce a luminance dip).
- If content is PQ-encoded (real HDR), gamma-space lerp is meaningfully wrong and the shader
  needs decode→lerp→encode. Do not build this until the encoding is confirmed.

Ship order: plain lerp first (matches the old validated shader), characterize, upgrade the
shader only if measurement says so.

## Selection semantics under blend

Blend replaces the *pick* step, not the selection contract:

- Both bracket frames newer than `lastShownTs` → blend with `w`, advance `lastShownTs` to
  `afterTs`? **No — design decision needed.** Advancing to `afterTs` skips ever re-using
  `after` as a future `before` at a later weight (each source frame participates in ~N/M
  blends at rate ratio N:M). Proposal: hysteresis tracks the *target*, not shown frames —
  monotonic targets are guaranteed by the schedule, so re-use of a frame in successive blends
  at increasing weights is correct and intended. The dupe concept ("repeat") then only applies
  to the genuine stall case (present `before` unblended twice).
- This changes what `detect.py dupes` sees — see Validation.

## Mode naming

The `<selection>:<present>` framework extends naturally: `b:60`, `b`, `b:vsync` (blend
selection, timer/DWM present) — the letters the deleted pre-ring modes used, now meaning the
ring-based rebuild. Class: one `BlendCaptureMode` (or a compositor strategy inside
`TemporalCaptureMode` — see below) with the same `vsyncPresent` flag. Do **not** resurrect
per-present-variant classes (see the class-grid lesson in `temporal-capture-project` memory).

**Structural option worth weighing at implementation time:** blend differs from nearest-pick
only in the final compose step. Either (a) new mode class sharing `CaptureRing` +
`PresentScheduler` (duplicates the loop skeleton once), or (b) a small `IFrameCompositor`
strategy (`Compose(bracket, w, backbuffer)`) injected into the existing temporal loop, with
nearest-pick and blend as two implementations. (b) keeps one loop and is the shape NVOFA slots
into; it is the recommended direction, but decide against the code, not in this doc.

## Logging

Keep the `temporal` per-present line unchanged where possible (`dl/tgt/before/after/w/jit/pdt`
all still apply); `pick=` becomes the compose outcome (`blend` | `before-stall` | `repeat`).
One-time setup line gains the compositor name + shader compile confirmation. No logging in the
draw path beyond the existing per-present line.

## Validation plan (extends V1–V7 methodology)

Instrument change to acknowledge up front: **blended output defeats the dupe detector.**
Every output frame is a unique pixel pattern, so `detect.py dupes --mad` loses its meaning as
a pacing instrument (only true stall-repeats remain detectable). Validation shifts weight to:

- `pacing` Roughness + period-2 metrics on the *video* (the point of blend is killing the
  240→60 period-2 — pass = period-2 component gone vs same-session `t:60` control).
- `reverse` still valid (blend of two forward-moving frames never reverses).
- Log reconciliation: `w` distribution should sweep smoothly (sawtooth across the rate ratio);
  stall fallbacks ≈ nearest-pick's repeat rate at the same config.
- Visual: UFO frame-step — blended frames must show *two* ghosted UFOs at intermediate
  positions (that is what correct temporal blending of sub-frame motion looks like), no
  luminance pumping, no single-frame flashes.
- New objective harness idea (cheap, offline): capture 240 Hz UFO once; the blend of frames
  N,N+1 at w=0.5 can be synthesized in Python from the decimated stream and compared (SSIM)
  against the relay's actual blended output for the same scene — catches shader/weight bugs
  without eyeballing.

Configs: `240_x1_b_60_ufo` (the judder killer — the headline test), `60_x1_b_60_ufo`
(matched-rate: blend should degenerate to w≈0/1 near-passthrough — verify no unnecessary
softening), `120cap_x2_b_vsync_kcd` (production config with FG collapse underneath).

## Risks → instruments

| risk | instrument |
|---|---|
| wrong/garbage weights (bracket bug) | `w` distribution in logs (must sawtooth, no spikes) |
| gamma dimming on blends | luminance histogram across w sweep; step-edge probe |
| blend cost blows present budget | `jit`/`pdt` p95 vs nearest-pick same-session |
| stall fallback misbehaving | `pick=` census vs source/present ratio |
| shader compile fails at runtime on user box | loud LOGERR + refuse mode (already the old code's behavior) |

## Sources

- `temporal-blend-future-frame-findings.md` (pre-ring blend review; principle + vestigial analysis)
- `git show dev:samples/NvFBC/NvFBCR/FrameBlendCaptureMode.cpp` — salvageable shader machinery
- `dual-device-capture-present-spec.md` — FindBracket contract, T10 latency policy, successor ordering
