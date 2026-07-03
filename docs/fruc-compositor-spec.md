# FRUC Compositor (`o:*` modes) — Feature Spec

Branch: `claude/fruc-compositor` (stacked on `claude/phase-pull`). Status: implemented,
**expectation of quality problems is on record** — this is a fair re-trial, not a bet.
Co-primary: the raw-flow variant (`claude/nvof-warp`, stacked on this branch).

## Evidence hygiene (read first)

The prior FRUC attempt (archive/garrett-fruc-optical-flow-interpolation) concluded "dimmed
frames, basically a lerp." **Those conclusions are contaminated**: it ran without decoupled
capture, with guessed source timestamps, and — unknown at the time — fed [generated, real]
ε-pairs from Smooth Motion into the flow estimator. Garbage flow degenerates to exactly a dim
lerp. What survives from the archive: the vendored SDK headers (facts) and the CUDA-interop
pain (reason to use D3D11 this time). The dimming's prime suspects — the alpha byte and the
conversion path — are neutralized and instrumented here.

## Architecture

`InterpSidecar`: a D3D11 device alongside the two D3D9 devices, slotting into the temporal
loop's compose step (now a three-way `TemporalCompositor` enum: nearest / blend / interp —
the lightweight form of the compositor interface the blend spec deferred).

Per present: bracket frames' D3D11 aliases (opened once from the ring's retained shared
handles) → fullscreen-pass conversion to BGRA8 with **alpha forced 1.0** → fed to NvOFFRUC
with QPC-derived double-second timestamps (before with `bSkipWarp=1` if unseen, after with
the output request at the target instant) → output `CopyResource` to a shared texture →
event-query flush (T4 discipline) → D3D9 StretchRects it to the backbuffer. Phase-pull
applies to interp exactly as to blend, so at locked integer ratios FRUC runs on real-frame
targets (w≈0) and the artifact surface shrinks to genuinely-converted ratios.

Written against the REAL `NvOFFRUC.h` (vendored from the archive into `third_party/NvOFSDK/`,
2022 SDK vintage): documented proc names via GetProcAddress, `NvOFFRUC_CREATE_PARAM`
(DirectX11Resource + ARGBSurface), min-3 resource registration (input ping/pong + output),
`bHasFrameRepetitionOccurred` out-flag, per-frame Process contract.

## Fallback ladder

Setup failure (DLL absent, create/register error, cross-API share refused) → loud LOGERR,
session runs as blend. Runtime Process failure → blend for that frame; 3 consecutive →
sidecar disabled for the session (loud once). Blend renderer is always initialized in `o:`
modes for this reason.

## Open verify points (carried into testing)

1. **DLL provenance**: loader tries `NvOFFRUC.dll` then `NvFRUC.dll`, app-local then system
   search, and LOGs the resolved path. The archived guide claims driver-shipped; the SDK
   docs say redistributable — whichever loads settles it. DLL is gitignored
   (`third_party/NvOFSDK/.gitignore`); check the license's redistributable attachment before
   shipping it in release zips.
2. **ARGBSurface byte order**: BGRA8 chosen (matches the old attempt's `D3DFMT_A8R8G8B8`).
   Channel-swapped colors ⇒ flip to R8G8B8A8 in `CreateConversionPipeline`.
3. **2022-vintage header vs current DLL**: if Create/Process return INVALID_PARAM against a
   newer driver-side DLL, refresh `third_party/NvOFSDK/NvOFFRUC.h` from the current SDK.
4. **Timestamp scale**: seconds (double) per the old attempt's usage; if interpolation phase
   looks wrong, try milliseconds.

## Validation / characterization

Instrumentation: `pick=interp|blend|stall` census (fallback visibility), `LastProcessUs`
available for a timing log line if needed, plus the standard temporal line.

| test | what it isolates |
|---|---|
| **Identity test first**: static desktop, `o:60` — output must be pixel-identical to input; ANY dimming = engine/format bug isolated from motion entirely | the old dimming claim, cleanly |
| `240_x1_o_60_ufo` vs same-session `b:60` | motion-comp quality vs blend ghosting at 4:1 (phase-pull locked: both should be ≈ passthrough — verifies pull applies to interp) |
| `180cap_x2_o_vsync_kcd` vs `b:vsync` | the 1.5:1 judder-killer with real motion comp vs blend blur; reticle/HUD halo inspection — the expected failure mode |
| `pdt`/`jit` vs blend same-session | FRUC cost on the present path (inline architecture; pipelined variant is the pre-planned fallback if pdt jitters) |

Decision input, not a gate: this branch exists to characterize. If HUD halos or engine
quality disqualify it, the finding transfers to `claude/nvof-warp` (same sidecar, our warp).
