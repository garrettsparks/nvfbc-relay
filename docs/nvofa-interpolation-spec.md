# NVOFA Interpolation — Investigation Spec + Probe

Status: **probe scaffolded on branch `claude/nvofa-fruc-probe`** (stacked on
`claude/blend-mode`); relay integration deliberately NOT implemented — everything downstream
is conditional on the probe's answers. Facts verified against NVIDIA/Microsoft docs
2026-07-02; items marked **(verify)** are what the probe answers.

## Purpose

Replace blend's ghosting with motion-compensated interpolation on the GPU's dedicated Optical
Flow Accelerator (NVOFA — Turing+, faster on Ada+). Path A's endgame for non-FG sources and
the quality ceiling short of driver frame gen.

## The shaping decision: FRUC library vs raw OF SDK

**Option 1 — NvOFFRUC library (recommended first attempt).** Optical Flow SDK 4.0+ ships a
Frame Rate Up Conversion library doing flow + warp + occlusion + blend as one call:
`NvFRUC.dll` → `NvFRUCCreate` → `NvFRUCRegisterResource` → `NvFRUCProcess` →
`NvFRUCUnregisterResource`/`NvFRUCDestroy`. Verified from the FRUC programming guide:
- **Interpolates at arbitrary phase** between two inputs ("any time-stamp between the two
  frames") — maps 1:1 onto our bracket weight `w`. The killer feature for rate conversion.
- Inputs: **D3D11 textures or CUDA pointers**; formats **NV12 or ARGB (8-bit)**. No 10-bit,
  no D3D9.
- Requirements: Turing+, Windows 10+, driver 511.65+ (all satisfied).
- The hard part (warp/occlusion/hole-fill, where visible artifacts live) is NVIDIA's tuned
  code. Cons: 8-bit only (banding risk vs our 10-bit path), black box, **(verify #5)**
  NvFRUC.dll redistribution license, **(verify #3)** per-call latency at 1440p undocumented.

**Option 2 — raw OF SDK (NvOFAPI) + our own warp.** `NvOFExecute` yields a flow-vector grid
(4×4 everywhere; 2×2/1×1 on Ampere+) at quarter-pixel precision via CUDA/D3D11/D3D12/Vulkan
interfaces; we upsample and warp ourselves (D3D11 CS): bidirectional warp toward phase w,
flow-consistency occlusion, hole fill. Full control, 10-bit-preserving warp possible, no
FRUC license question — but the warp/occlusion compositor is genuinely hard and is exactly
FRUC's value. Fallback if FRUC disqualifies; reuses all of Option 1's plumbing.

**Option 3 — non-NVIDIA routes** (software flow, AMD FidelityFX, DXVA VideoProcessor FRC):
surveyed and rejected — compute budget, game-motion-vector dependence, and vendor black-box
unreliability respectively.

## Architecture: D3D11 sidecar between ring and present

The present stack **stays D3D9** (all T8 validation is tied to it). A D3D11 "interp device"
shares surfaces with both existing devices:

```
capture dev (D3D9Ex) ──ring slots (shared)──► present dev (D3D9Ex) ──StretchRect──► DWM
                          │                        ▲
                          └──► interp dev (D3D11): │ (shared output texture)
                               8-bit convert ► NvFRUCProcess(before, after, w) ► out
```

Per output frame: FindBracket → convert bracket slots 10→8-bit (cache: a slot's 8-bit twin
is valid until the ring wraps, so steady state converts only the newest member) →
`NvFRUCProcess(phase=w)` → sync → D3D9 StretchRect shared output → PresentEx.

### Interop mechanics (the load-bearing details)

- **Ring slots into D3D11**: D3D9Ex-created shares open via `OpenSharedResource`
  (documented). Prerequisite shipped on this branch: `CaptureRing` now RETAINS the slot
  shared handles (`SlotSharedHandle(i)`) — they were previously discarded after the
  present-device open. **(verify #1)** 10-bit cross-API sharing isn't explicitly blessed by
  the format table — probe TEST 1/2.
- **Output back to D3D9**: create on D3D11 with `D3D11_RESOURCE_MISC_SHARED` (legacy share,
  no keyed mutex) and open on D3D9Ex — the less-common direction, **(verify #2)** probe
  TEST 3. Fallback: create the output on the *present* device (D3D9, shared) and open it in
  D3D11 — the ring's own known-good direction.
- **Coherency**: D3D9Ex shares are *unsynchronized* across APIs (MS: "you must add manual
  synchronization"); no keyed mutex exists on D3D9-created shares. The T4 event-query flush
  discipline applies at each hand-off: ring slots are already capture-flushed before publish;
  the interp device must Flush + drain an event query after Process+copy before D3D9 reads.

### Timing budget (T10)

Interp work sits between the pacing wait and PresentEx — must be well under one present
period and near-constant. Unknowns → probe: FRUC wall time at 1440p **(verify #3)**,
conversion passes (sub-ms, near-certain), interp flush (expect the T4 ~100–350 µs class).
Pre-planned fallbacks if measurement disagrees: **pipelined** (kick interpolation for
deadline N+1 right after presenting N — one extra period of constant latency, FRUC off the
critical path) or **third-thread** (only if FRUC time is *variable*; the full
producer-consumer generalization — do not build speculatively).

### Where it plugs in

The compose step — alongside nearest (StretchRect) and blend (BlendRenderer). Three variants
= the moment to extract the compositor interface the blend spec deferred. Blend remains the
runtime fallback whenever the sidecar fails (probe failure, per-frame error, old driver).

## The probe (this branch): `samples/NvFBC/NvFBCFrucProbe/`

Standalone console app, NOT in the relay solution (CI untouched). Two parts:

- **Part 1 (builds with Windows SDK alone)** — answers the interop verifies now:
  TEST 1 D3D9Ex→D3D11 10-bit share; TEST 2 pixel coherency through the share after event
  flush; TEST 3 D3D11→D3D9Ex reverse-direction share; TEST 4 the 8-bit BGRA pair FRUC
  actually eats. Exit code = failure count.
- **Part 2 (`NVFRUC_SDK_AVAILABLE`)** — skeleton for the FRUC latency/quality probes
  (synthetic moving-box frames at phases .25/.5/.75, BMP dumps, 1000-iteration QPC timing,
  NV12-vs-ARGB comparison). Gated because the SDK header/DLL require a download and a
  **license read (verify #5)** — the skeleton's struct usage must be diffed against the real
  `NvFRUC.h` before first build; the programming guide documents the flow, not byte-exact
  layouts.

## Prerequisites (ordered)

1. Adaptive bracketing delay — shipped (parent branch).
2. Blend mode shipped — the fallback path, the plumbing, and the A/B control (parent branch).
3. Shared-handle retention — shipped (this branch).
4. Probe Part 1 run on the Windows box → interop verdicts.
5. SDK download + license read → probe Part 2 → latency/quality verdicts.
6. Only then: relay integration (own branch, own spec addendum).

## Open questions the probe answers

1. 10-bit D3D9↔D3D11 sharing (TEST 1/2) — if it fails: capture-side 8-bit parallel slots or
   an extra 10→8 copy through a D3D9-shared intermediate.
2. Share direction for the output texture (TEST 3) — picks the fallback if needed.
3. FRUC latency/variability at 1440p → inline vs pipelined vs third-thread.
4. 8-bit banding: FRUC-then-StretchRect-to-10-bit vs the 10-bit nearest/blend path on
   gradient content — if visibly worse, Option 2's custom 10-bit warp earns its cost.
5. NvFRUC.dll redistribution terms.
6. NV12 vs ARGB input: NV12 halves bandwidth and is FRUC's best-exercised path (video
   products feed it NV12) but costs a color-space round trip — compare artifacts + latency.

## FG interaction note

With keep-real collapse the ring is base-cadence reals — NVOFA interpolation *replaces* what
Smooth Motion's generated frames did, at our output rate. For SM titles this competes with
Path B (pass-through of SM's own gen frames re-stamped); product decision to make with both
in hand. No code conflict: Path B is capture-side, NVOFA is compositor-side.

## Sources

- FRUC programming guide: https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvfruc-programming-guide/index.html
- OF SDK hub / NVOFA programming guide: https://docs.nvidia.com/video-technologies/optical-flow-sdk/index.html
- Ada FRUC blog: https://developer.nvidia.com/blog/harnessing-the-nvidia-ada-architecture-for-frame-rate-up-conversion-in-the-nvidia-optical-flow-sdk/
- Surface sharing between Windows graphics APIs (unsynchronized D3D9Ex semantics): https://learn.microsoft.com/en-us/windows/win32/direct3darticles/surface-sharing-between-windows-graphics-apis
- OF SDK deprecation notices (FRUC not deprecated as of 5.0): https://docs.nvidia.com/video-technologies/optical-flow-sdk/deprecation-notices/
