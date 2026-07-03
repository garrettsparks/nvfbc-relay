# Raw NVOFA Flow + Custom Warp (`-interp flow`) — Feature Spec

Branch: `claude/nvof-warp` (stacked on `claude/fruc-compositor`; shares its D3D11 sidecar,
conversion pipeline, and cross-API output path — the diff is the flow engine + warp + backend
switch). Status: **warp pipeline complete; OF session-init gated on one header drop.**
This is the favored long-term bet: same NVOFA silicon FRUC uses, but we own the compositor —
dimming is impossible by construction, artifacts are ours to fix, 10-bit end-to-end is a
future option, and the only NVIDIA runtime dependency is `nvofapi64.dll`, which ships in the
driver (R455+) — no SDK redistributable at all.

## Backend selection

`-interp fruc|flow` on the `o:*` modes. **Default is currently `fruc`** (runnable today);
flips to `flow` once the session-init below is verified. A failed backend falls back to blend
per the sidecar's ladder — never silently to the other backend (attribution stays clean).

## What's complete vs gated

**Complete (compiles and runs now):** flow-texture plumbing (R16G16_SINT at grid resolution =
`NV_OF_FLOW_VECTOR` S10.5 layout per the vendored v5.0 common header), the warp pass
(vs_5_0/ps_5_0), constant buffer, sidecar integration (stateless per present: convert both
bracket frames → flow → warp straight into the shared output → T4 flush).

**Gated on `nvOpticalFlowD3D11.h`** (not in the archive's vendored set — drop it from your
Optical Flow SDK into `third_party/NvOFSDK/` and rebuild; `__has_include` picks it up):
`FlowWarpEngine::CreateFlowSession` and the per-frame `nvOFExecute`. The gated block contains
the documented call sequence as VERIFY-ON-HEADER-DROP comments — diff against the real header
on first build; the session init deliberately fails loud until completed, so the mode cannot
silently run warp-without-flow (which would be plain blend wearing a costume).

## Warp v1 (deliberately minimal)

```
F      = forward flow, before→after, 4×4 grid, S10.5 fixed point (÷32 → pixels)
out(p) = lerp( before(p − w·F(p)), after(p + (1−w)·F(p)), w )
```

"Basically a lerp with a weight" — correct, after moving the pixels first. Known v1 limits,
in expected-severity order:

1. **Occlusion-blind**: disocclusions sample stale content from both sides — halos around
   movers, worst at high w-distance from both frames. This is THE quality question v1 answers
   cheaply: are 4×4-grid + occlusion-blind halos better or worse than blend's double-image?
2. **One flow field for both directions**: assumes locally linear motion across the bracket.
3. **Grid granularity**: 4×4 universal; 1×1 available on Ampere+ (future caps-gated knob).

Pre-planned v2 upgrades, each with a measurable trigger: backward flow
(`NV_OF_PRED_DIRECTION_BACKWARD` or `bwdOutputBuffer`) + occlusion test by forward/backward
consistency (trigger: halos disqualify v1); cost-buffer confidence weighting → fall back to
blend per-region (trigger: flow noise on flat regions); 10-bit warp sampling the original
ring textures with 8-bit-derived flow (trigger: banding measured on gradients).

## Shared-with-FRUC verify points

Input byte order (ABGR8 vs BGRA8 — flow quality collapses if channels are swapped since the
engine sees scrambled luminance), timestamp-independent (flow uses frames only; phase enters
through our warp `w` — one less failure mode than FRUC's timestamp API), phase-pull applies
identically (locked integer ratios never invoke the engine).

## Validation

Same captures as the FRUC branch, `-interp flow` vs `-interp fruc` vs `b:*` same-session —
the three-way comparison the compositor enum exists for. Log: `pick=interp` census +
`InterpSidecar` init line names the backend. The 180cap×2 KCD capture (1.5:1) is the
decisive artifact-quality judge; the UFO 240→60 run is the flow-correctness smoke test
(uniform motion field — any warp bug is glaring on the test pattern).
