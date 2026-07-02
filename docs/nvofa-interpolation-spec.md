# NVOFA Interpolation — Investigation Spec

Status: **investigation — no implementation**. Written 2026-07-02 against the
`decoupled-capture-present` baseline. Successor item after blend in
`dual-device-capture-present-spec.md` ordering. Facts below verified against NVIDIA/Microsoft
docs on 2026-07-02 (links at bottom); items marked **(verify)** need a probe program or a
license read before they can be treated as settled.

## Purpose

Replace blend's ghosting with motion-compensated interpolation: estimate optical flow between
the bracket frames on the GPU's dedicated Optical Flow Accelerator (NVOFA — Turing+, faster on
Ada+), then synthesize the frame at target phase `w`. This is Path A's endgame for non-FG
sources (240→60 with no period-2 and no double-image), and the quality ceiling for everything
short of driver-level frame gen.

## The decision that shapes everything: FRUC library vs raw OF SDK

NVIDIA ships **two** ways to consume NVOFA:

### Option 1 — NvOFFRUC library (RECOMMENDED first attempt)

The Optical Flow SDK (4.0+) includes a Frame Rate Up Conversion library that does the entire
job as a black box: flow estimation + forward warp + occlusion/hole handling + blend, in one
call. Verified from the FRUC programming guide:

- API: load `NvFRUC.dll` → `NvFRUCCreate` → `NvFRUCRegisterResource` →
  `NvFRUCProcess` per frame → `NvFRUCUnregisterResource` / `NvFRUCDestroy`.
- **Interpolates at arbitrary phase between the two inputs** (explicitly documented: "any
  time-stamp between the two frames, e.g. 1.25, 1.50, 1.75") — maps 1:1 onto our bracket
  weight `w`. This is the killer feature for us: rate conversion needs arbitrary phase, not
  just midpoint doubling.
- Inputs: **D3D11 textures or CUDA pointers/arrays**; formats **NV12 or ARGB (8-bit)**.
  No 10-bit input. No D3D9 path.
- Requirements: Turing+ with OF support, Windows 10+, driver 511.65+ (all satisfied here).
- Uses NVOFA hardware + some CUDA cores internally (even via the D3D11 interface).

Pros: the warp/occlusion problem — the actual hard part, where all the visible artifacts live
— is NVIDIA's code, tuned on the same engine family their own products use. Integration is
plumbing, not CV research.
Cons: 8-bit only (output banding risk vs our 10-bit path); black box (artifacts are
take-it-or-leave-it); **(verify)** redistribution terms of `NvFRUC.dll` — read the SDK license
before shipping it next to the exe; **(verify)** per-call latency at 1440p — not documented,
must be measured (instrument `NvFRUCProcess` with QPC; budget analysis below).

### Option 2 — raw Optical Flow SDK (NvOFAPI) + our own warp

`NvOFExecute` produces a flow-vector grid (4×4 granularity everywhere; 2×2 and 1×1 available
on Ampere+) at quarter-pixel precision; interfaces exist for CUDA, D3D11, D3D12, and (SDK 5.0,
Ampere+) Vulkan. We would then upsample the flow field and warp/composite ourselves (D3D11
compute shader or CUDA kernel): bidirectional warp toward phase `w`, flow-consistency occlusion
test, hole fill, fallback blend.

Pros: full control (10-bit-preserving warp is possible — sample the original 10-bit ring
texture with 8-bit-derived flow); artifacts are ours to fix; no FRUC redistribution question
(the OF driver API is loaded from the driver itself).
Cons: the warp/occlusion compositor is genuinely hard — halos, stretching, disocclusion holes
are exactly the failure modes that take months of tuning; this is where FRUC's value is.

### Option 3 — non-NVIDIA routes (surveyed for completeness, all rejected for now)

- **Software optical flow** (Farneback/RAFT/etc. on CUDA or CPU): quality attainable but the
  compute budget at 1440p60 is hostile, and it abandons the dedicated-engine advantage. No.
- **AMD FidelityFX frame interpolation**: designed around game-supplied motion vectors; its
  optical-flow-only path is not a supported standalone product for arbitrary video. No.
- **DXVA/D3D11 VideoProcessor frame-rate conversion**: vendor black box, capability flags
  rarely honored for arbitrary content on NVIDIA. No.
- **Keep using driver Smooth Motion (Path B pass-through)**: complementary, not competing —
  Path B covers FG titles by re-stamping; NVOFA covers everything else. Both remain on the
  roadmap.

**Recommendation:** Option 1 first. If FRUC quality or latency disqualifies it, Option 2
reuses ~all of Option 1's plumbing (same interop, same formats, same architecture slot) and
adds the custom compositor — nothing is thrown away by trying FRUC first.

## Architecture: D3D11 sidecar between ring and present

The present stack **stays D3D9** — every validated t:vsync/DWM behavior (T8) is tied to the
current windowed D3D9 present; swapping the present API would invalidate the baseline and is
not on the table. NVOFA work happens on a new D3D11 device ("interp device") that shares
surfaces with both existing devices:

```
capture dev (D3D9Ex) ──ring slots (shared)──► present dev (D3D9Ex) ──quad/StretchRect──► DWM
                          │                        ▲
                          └──► interp dev (D3D11): │ (shared output texture)
                               8-bit convert ► NvFRUCProcess(before, after, w) ► out
```

Per output frame (present thread, between deadline and present):

1. `FindBracket(target)` → before/after ring indices + `w` (unchanged).
2. Interp device: format-convert both bracket slots 10-bit→8-bit ARGB (one trivial pixel/CS
   pass each — cache: a slot's 8-bit twin is reusable until the ring wraps, so at steady state
   only the *newest* bracket member needs converting each frame).
3. `NvFRUCProcess(before8, after8, phase=w)` → interpolated 8-bit frame.
4. Sync interp→present (see coherency below), then D3D9 `StretchRect` the shared output
   texture → backbuffer (D3D9 handles ARGB8→A2R10G10B10 conversion in StretchRect), `PresentEx`.

### Interop mechanics (the load-bearing details)

- **Ring slots into D3D11**: D3D9Ex-created shared textures open on a D3D11 device via
  `ID3D11Device::OpenSharedResource` (documented Microsoft path for D3D9Ex↔D3D11 sharing).
  Implementation prerequisite: `CaptureRing::Start()` currently **discards** the shared
  HANDLEs after opening the present-device aliases — they must be retained (tiny change,
  `Slot::sharedHandle`) so the interp device can open the same resources. **(verify)** 10-bit
  (`D3DFMT_A2B10G10R10` ↔ `DXGI_FORMAT_R10G10B10A2_UNORM`) cross-API sharing: the canonical
  format table doesn't explicitly bless it; probe at setup and fail loud (same
  either-works-or-doesn't posture as direct-write). Fallback if it fails: capture-side 8-bit
  conversion (see Open questions).
- **Output texture back to D3D9**: create the FRUC output as a shared D3D11 texture, open it
  on the present device. Direction (11-created → 9-opened) is the less-common one **(verify
  in probe)**; fallback: create it on the *present* device (D3D9, shared) and open it in
  D3D11 — same trick the ring already uses, known-good direction.
- **Coherency**: D3D9Ex shared surfaces are explicitly *unsynchronized* across APIs (MS docs:
  "you must add manual synchronization"). No keyed mutex exists on D3D9-created shares. We
  already own this problem and its solution — T4's event-query flush. The same discipline
  applies at each hand-off: ring slots are already capture-flushed before publish (T4, done);
  the interp device must flush (D3D11 event query + `Flush`, drain `GetData`) after
  `NvFRUCProcess`+copy before the present device reads the output. The wait lands on the
  present thread — see budget.

### Timing budget (T10 analysis)

Everything in steps 2–4 executes between the pacing wait and `PresentEx`, so it must be (a)
well under one present period and (b) near-constant. Unknowns to measure in the probe:
`NvFRUCProcess` wall time at 2560×1440 **(verify — not documented)**, conversion passes
(sub-ms, near-certain), interp-side flush (expect the same ~100–350 µs class as T4's).
Expectation: low-single-digit ms total, constant → an acceptable constant offset. Two
pre-planned fallbacks if measurement disagrees:

- **Pipelined variant**: kick interpolation for deadline N+1 immediately after presenting N
  (the bracket for N+1 usually already exists — lag ≥ one source period guarantees it); the
  present thread then only waits for a *finished* result. Costs one more period of constant
  latency, removes FRUC time from the critical path entirely.
- **Third-thread variant** (only if FRUC time is *variable*): dedicated interp thread
  producing into a 2-slot output queue keyed by deadline; present thread consumes or falls
  back to blend/nearest on miss. This is the full producer-consumer generalization of the
  two-thread design; do not build it speculatively.

### Where it plugs in

Same slot as blend: the compositor strategy (`IFrameCompositor::Compose(bracket, w, dest)`).
Nearest-pick = StretchRect one input; blend = shader lerp; NVOFA = the sidecar above. One
temporal loop, three compositors, mode strings `t:*` / `b:*` / `o:*` (naming TBD). Blend
remains the *runtime fallback* whenever the sidecar fails (setup probe failure, per-frame
error, driver too old) — quality degrades, pacing survives.

## Prerequisites (ordered)

1. **Adaptive bracketing delay** — identical blocker as blend (interpolation needs the
   after-frame unconditionally).
2. **Blend mode shipped and validated** — it is the fallback path, the compositor plumbing,
   and the A/B control for judging whether NVOFA's artifacts beat blur.
3. **Shared-handle retention in CaptureRing** (one-line-per-slot change, can ride any commit).
4. **Probe program** (see below) before any relay integration.

## Probe program (first implementable artifact — small, standalone)

A tiny console tool, not a relay mode: create D3D9Ex + D3D11 devices on the source adapter,
create one D3D9Ex shared 10-bit texture, open in D3D11 (**verify #1: 10-bit sharing**),
create/open the reverse-direction output (**verify #2: direction**), load `NvFRUC.dll`,
register, feed two synthetic frames (moving box), request phases 0.25/0.5/0.75, time 1000
iterations at 1440p (**verify #3: latency**), write outputs to BMP for eyeball check
(**verify #4: does it interpolate what we think**). Also settles the license question by
forcing a read of the SDK EULA to obtain the DLL (**verify #5**). Everything downstream of
this probe is conditional on its five answers.

## FG interaction note

With keep-real collapse the ring is base-cadence reals — NVOFA interpolates between real
frames and *replaces* what Smooth Motion's generated frames were doing, at our output rate
instead of the display's. For SM titles this competes with Path B (pass-through of SM's own
gen frames, re-stamped); which wins is a quality/latency product decision to make with both
in hand, per the existing strategic fork in the project record. They share no code conflict:
Path B is capture-side, NVOFA is compositor-side.

## Validation

Blend's validation notes apply (dupe detector is defeated; pacing/Roughness + reverse + log
reconciliation carry the load), plus:

- **Halo/artifact review**: UFO frame-step — NVOFA's failure modes are edge halos and
  background dragging around movers; compare frame-by-frame against blend's double-image at
  the same `w`. The 186-pair review methodology from the FG work applies directly.
- **Same-scene SSIM harness** (from blend spec): now *more* valuable — interpolated frame at
  w=0.5 vs the real captured mid-frame from a 240 Hz ground-truth capture is an objective
  quality score for the whole pipeline, per scene.
- Latency check: `jit`/`pdt` unchanged vs blend same-session (constant offset is invisible
  there; a *variable* FRUC cost shows up as pdt jitter immediately).
- Configs: `240_x1_o_60_ufo` (headline), `120cap_x2_o_vsync_kcd` (production stack:
  collapse under, NVOFA over), plus the blend controls same-session.

## Open questions

1. 10-bit shared-surface interop (probe #1) — if it fails: convert to 8-bit on the *capture*
   device into parallel shared 8-bit slots (capture-side cost, doubles slot memory) or accept
   an extra 10→8 copy through a D3D9-shared intermediate. Decide on probe results.
2. FRUC latency/variability at 1440p (probe #3) → chooses inline vs pipelined vs third-thread.
3. `NvFRUC.dll` redistribution terms (probe #5).
4. 8-bit banding on gradients: is FRUC-then-StretchRect-to-10-bit visibly worse than the
   10-bit nearest/blend path? A/B on gradient-heavy content; if yes, Option 2 (custom 10-bit
   warp) is the only cure and its cost is then justified by data.
5. NV12 vs ARGB input to FRUC: NV12 halves conversion bandwidth and may be FRUC's
   best-exercised path (video products feed it NV12), but costs a color-space round trip.
   Probe both; pick by artifact + latency comparison.

## Sources

- [NVOFA FRUC programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvfruc-programming-guide/index.html) — API flow, formats, arbitrary-phase support, requirements
- [Optical Flow SDK docs hub](https://docs.nvidia.com/video-technologies/optical-flow-sdk/index.html) / [NVOFA programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html) — grid sizes, API interfaces (CUDA/D3D11/D3D12/Vulkan)
- [Ada FRUC blog](https://developer.nvidia.com/blog/harnessing-the-nvidia-ada-architecture-for-frame-rate-up-conversion-in-the-nvidia-optical-flow-sdk/) — engine generation notes
- [Surface sharing between Windows graphics APIs](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/surface-sharing-between-windows-graphics-apis) — D3D9Ex↔D3D11 sharing, *unsynchronized* semantics, manual-sync requirement
- [Deprecation notices](https://docs.nvidia.com/video-technologies/optical-flow-sdk/deprecation-notices/) — FRUC not deprecated as of SDK 5.0 (stereo-disparity mode is)
