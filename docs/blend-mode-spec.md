# Blend Mode — Design & Implementation Spec

Status: **implemented on branch `claude/blend-mode`** (stacked on
`claude/adaptive-bracketing-delay`, which it requires). Investigation written 2026-07-02
against the dual-device baseline; Implementation Decisions at the bottom record what was
built.

## Purpose

Replace nearest-pick's "choose before OR after" with a weighted combination of both bracket
frames at the output instant. First interpolator: removes stride quantization at non-integer
rate ratios (the 90→60 1.5:1 case is the poster child — see `180cap_x2_t_vsync_sb2_kcd`) at
the cost of motion blur proportional to the bracket gap. The *shape* of the change (consume
`FrameBracket{before, after, w}` → produce one frame) is identical to the NVOFA stage that
later replaces it.

## What the ring already provides

`FindBracket()` needs no changes: `beforeTexture`/`afterTexture` are present-device texture
aliases (ring slots are RENDERTARGET textures specifically so shaders can sample them), and
`weight = beforeDiff/(beforeDiff+afterDiff)` is already computed and logged as `w=`.

## Lessons already paid for (do not re-derive)

1. The old pre-ring blend's *principle* was correct (gated on a future frame, genuinely
   interpolated); its foundation (polled NOWAIT timestamps → garbage weights) was the broken
   part. The ring fixes exactly that.
2. Its shader machinery was proven: vs_3_0/ps_3_0 fullscreen-quad `lerp(t0,t1,c0)`, compiled
   at setup with error logging (retrievable:
   `git show dev:samples/NvFBC/NvFBCR/FrameBlendCaptureMode.cpp` @ cf28f40-era history).
3. Bracketing latency is inherent: interpolating across T requires a frame past T; output
   runs ~1 lag behind. Accepted under the T10 latency policy.

## Prerequisite: adaptive bracketing delay (hard blocker — the stacked parent branch)

Nearest-pick degrades gracefully without an after-frame (repeats); blend cannot — no
after-frame means nothing to blend toward. The parent branch guarantees the bracket:
lag = max(present period, 1.25 × measured source period), slew-limited.

## Options considered

- **A — pixel-shader lerp on the present device (SHIPPED)**: proven machinery, 10-bit
  precision end-to-end, natural home for gamma-correct math later. Cost: sub-ms fullscreen
  textured draw, constant (T10-compliant). Restores the real `d3dcompiler` import.
- **B — fixed-function `D3DRS_BLENDFACTOR` two-pass**: no shader compile, but caps-dependent,
  two passes, and no path to correct color math. Rejected.
- **C — StretchRect tricks**: no blending exists in StretchRect. Rejected.

## Color math (measure before upgrading)

Naive lerp in the stored encoding is only correct for linear content; ours is
`NVFBC_TODX9VID_ARGB10` with `bHDRRequest=TRUE` and has never been characterized (SDR gamma
in 10 bits? PQ under Windows HDR?). Gamma-space lerp of adjacent video frames is a small,
acceptable first-ship error (classic crossfade dimming, worst at w≈0.5 across high-contrast
edges). Ship plain lerp → characterize the encoding with gradient content → upgrade the
shader only if measurement demands. If content is PQ, decode→lerp→encode becomes necessary.

## Validation plan

**Instrument note: blended output defeats `detect.py dupes`** — every output frame is a
unique pixel pattern; only true stall-repeats remain detectable. Weight shifts to:

- `pacing` Roughness + period-2 on video: the point of blend is killing quantization —
  90→60 (`180cap_x2_b_vsync_kcd` vs the sb2 nearest control) is the headline; 240→60
  (`240_x1_b_60_ufo`) should stay clean.
- `reverse` still valid (blend of two forward frames never reverses).
- Log: `w` distribution sweeps smoothly (sawtooth at the rate ratio); `pick=stall` ≈ 0 with
  the adaptive lag; `pick=blend` dominant.
- Visual: UFO frame-step — correct temporal blending shows *two* ghosted UFOs at
  intermediate positions; no luminance pumping; no single-frame flashes.
- Objective harness (offline, optional): synthesize lerp(N, N+1, 0.5) in Python from a
  240 Hz capture; SSIM against the relay's blended output for the same scene catches
  weight/shader bugs without eyeballing.

Configs: `240_x1_b_60_ufo`, `60_x1_b_60_ufo` (matched-rate: expect near-passthrough at
w≈0/1 — verify no needless softening), `180cap_x2_b_vsync_kcd` (the 1.5:1 judder-killer
demo), `120cap_x2_b_vsync_kcd` (production-adjacent).

## Risks → instruments

| risk | instrument |
|---|---|
| garbage weights | `w` distribution (smooth sawtooth, no spikes) |
| gamma dimming | luminance histogram across w sweep; step-edge probe |
| blend cost blows present budget | `jit`/`pdt` p95 vs nearest same-session |
| stall fallback misbehaving | `pick=` census vs source/present ratio |
| shader compile fails on user box | loud LOGERR, mode refused at Setup |

## Implementation decisions (2026-07-03, branch `claude/blend-mode`)

- **`BlendRenderer.{h,cpp}`**: salvaged pipeline re-hosted as a standalone renderer;
  gamma-space lerp first per ship order; the original quad's missing half-texel offset kept
  verbatim (1:1 sizes, previously validated) and noted in code.
- **Structural**: not a full `IFrameCompositor` interface yet — an `m_blend` flag on
  `TemporalCaptureMode` selects the compose arm. One loop, no class grid (the deleted
  17KB-duplicated blend twins remain the cautionary tale). When NVOFA makes it three compose
  variants, extract the interface then.
- **Selection semantics**: blend arm uses no hysteresis and no Schmitt band — no discrete
  pick exists to oscillate; monotonic schedule targets guarantee monotonic content time.
  `lastShownTs` untouched in blend mode.
- **Fallbacks**: one-sided bracket → present that side unblended (`pick=stall`; adaptive lag
  makes it rare); empty bracket → `pick=repeat`, no draw; shader compile failure → mode
  refused loudly at Setup.
- **Log vocabulary**: `blend` and `stall` join the pick labels; `w=` is now the rendered
  weight. `GetModeName()` = "TemporalBlend".
- **Defender shim removed**: `BlendRenderer`'s static `D3DCompile` reference keeps the
  `d3dcompiler.dll` import unconditionally — which is all the WinMain no-op shim existed to
  fake (see defender false-positive record).
- **Modes**: `b` / `b:vsync` (DWM present, nominal 60), `b:<rate>` (timer present).
