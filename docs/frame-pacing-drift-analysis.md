# Frame-Pacing Drift Analysis (background for clock-based mode fixes)

> ## ⚠️ RESOLVED — root cause below was misdiagnosed (corrected 2026-06)
>
> The hitch was **not** drift between the CPU clock and the display vblank crystal, as the
> "Root cause" section below claims. The actual cause was **relative-timer wake-latency
> accumulation**: `TimerCaptureMode` re-armed a *relative* waitable timer (`SetWaitableTimer`
> with a negative interval) every iteration, re-anchoring the schedule to "now" each frame.
> Per-frame OS wake latency therefore accumulated, the effective rate crept, and that beat
> against the display refresh → the ~1 Hz hitch.
>
> **Fix (shipped, dev `fb0e6e3`):** schedule presents against an **absolute** QPC timeline
> (`seed + N*period`) using the high-resolution waitable timer, plus a catch-up clamp for
> transient stalls. No vblank phase reference, no extra thread, no DXGI dependency.
>
> **The vblank phase-lock machinery in this doc was unnecessary.** The three sibling specs
> below, and the `DxgiPhaseLockedCaptureMode` built from the DXGI one, all *worked* — but
> the phase-lock was never the active ingredient: disabling the periodic resync (commenting
> out the snap-to-vblank line) changed the result by **zero dupes**. Their smoothness came
> from the absolute scheduling they happened to also use, not from the vblank reference.
> All three spec branches are archived (`git tag -l 'archive/*'`).
>
> **Why the original diagnosis was wrong:** crystal drift is real but tiny — quartz is
> ~±50 ppm, so the CPU clock slips one 16.667 ms frame vs the display only every ~5–6
> minutes. That cannot produce ~10 dupes in a 10-second clip. The signal the table below
> attributes to "drift" was relative-timer jitter, not crystal drift.
>
> **Validation (UFO test):** Timer mode went from 32 dupes / 30 s → **0** with the absolute
> schedule; `std(Δ)` 1.9 → 0.43; `min(Δ)` 0.01 → 12.8 — matching the vsync baseline. Over a
> 5-minute capture there is **no steady drift**; the only residual dupes were short
> *clustered* bursts (upstream/source content duplication + OBS startup), which a vblank
> resync would not have helped anyway.
>
> Everything below is retained as the investigation record. Read it as history, not as a
> spec to implement.

---

This document captures the (since-superseded) diagnosis of frame-pacing drift in the
clock-based capture modes (`TimerCaptureMode`, `GPUSleepCaptureMode`) and the validation
method that any proposed fix had to satisfy. Three sibling specs proposed distinct fixes;
this was their shared background.

The validation method (UFO test + `detect.py` MAD metrics) below is still correct and useful.
The sibling specs are historical:

- `phase-locked-timer-dwm-spec.md` — periodic phase resync via `DwmGetCompositionTimingInfo`
  (also unworkable on its own terms: that API is primary-display-only since Win8.1, so it
  could not reference a non-primary capture-card target)
- `phase-locked-timer-dxgi-spec.md` — periodic phase resync via `IDXGIOutput::WaitForVBlank`
- `vblank-driven-scheduler-spec.md` — restructure loop so vblank events drive the schedule

`GPUSleepCaptureMode` (per `docs/gpu-sleep-capture-mode-spec.md`) is treated as a dead end
for two reasons:

1. **`NvFBCToDx9VidGPUBasedCPUSleep` appears to not be a real, implemented API.** It is
   declared in NVIDIA's `nvFBCToDx9Vid.h` header (`inc/NvFBC/nvFBCToDx9Vid.h:257`) but
   project owner reports that earlier attempts to use it caused crashes; the current
   implementation in `GPUSleepCaptureMode.cpp` calls it but does not verify behavior beyond
   checking the return code. Whatever the May-16 `g-60` test recording captured (8 dupes
   in 10 s) is the result of *something* happening at runtime — possibly the API silently
   no-opping and the loop spinning, possibly fallback behavior. The drift signal is
   empirically real but the underlying mechanism is unknown and unreliable.
2. **Even if the API worked, it would not solve drift.** It is a sleep primitive, not a
   phase reference. Any sleep against a CPU clock has the same drift behavior as
   `TimerCaptureMode` against the target display vblank.

Don't carry `GPUSleepCaptureMode` forward. The fix for drift makes it redundant on its
best behavior; the API uncertainty makes it actively harmful as a baseline.

## Symptom

`VsyncCaptureMode` (`samples/NvFBC/NvFBCR/VsyncCaptureMode.cpp`) produces perceptually
smooth output. `TimerCaptureMode` (`TimerCaptureMode.cpp`) at 60fps and `GPUSleepCaptureMode`
(`GPUSleepCaptureMode.cpp`) at 60fps both exhibit a visible periodic hitch on the target
display, roughly once per second, despite each mode using a high-precision timing primitive.

## Root cause (SUPERSEDED — see correction at top; the real cause was relative-timer jitter, not crystal drift)

`TimerCaptureMode` uses `CreateWaitableTimerEx` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`,
which drives off the CPU's high-resolution timer (TSC / HPET). `GPUSleepCaptureMode` measures
elapsed work with `QueryPerformanceCounter` and sleeps the residual via `NvFBCToDx9VidGPUBasedCPUSleep`.
Both schedule presents from a clock that is **not phase-locked to the target display's vblank
crystal**.

The target display (capture card in this project's typical configuration) generates its own
60.000 Hz refresh cadence from its display PLL. Over time, the CPU clock and the display PLL
drift relative to each other. Even sub-1ms drift accumulates: at ~0.1% drift over a 10-second
window, cumulative error reaches one full 16.667 ms frame interval, at which point a Present
either lands during scan-out (frame skip / tear depending on mode) or arrives too late and
the display repeats the previous frame (capture sees a dupe).

`VsyncCaptureMode` doesn't have this problem because `D3DPRESENT_INTERVAL_ONE` blocks the
loop on the display's vblank, so the entire schedule is by construction phase-locked.

## Measured evidence (Blur Busters UFO test, 10 s each, 60 fps capture)

Three clips were recorded with NvFBCR in three modes against the same source (UFO test,
constant uniform horizontal scroll, the canonical frame-pacing reference). Per-frame mean
absolute difference (MAD) computed at half-resolution gray from the recorded mp4:

| Clip | std(Δ) | min(Δ) | 1%(Δ) | frames Δ<2.0 | strongest period sigma |
|---|---|---|---|---|---|
| `vsync` | **0.44** | 13.54 | 13.57 | **0** | none below P30 |
| `g-60` (GPUSleep) | 1.75 | **0.015** | 0.025 | **8 (1.3%)** | P4 at 2.08σ |
| `_60` (Timer) | 1.98 | 0.015 | 0.024 | **10 (1.6%)** | P8 at 2.10σ |

On constant uniform motion the MAD per frame should be nearly constant (std ≈ 0 in
principle; ~0.44 on vsync due to encoder noise floor). The clock-based modes show std
4–5× higher and a long tail of near-zero deltas at the 1st percentile — those are the
dupes from drift slipping a full frame against the display vblank.

`detect.py`'s `pacing` subcommand under-reports this because its detection threshold
(3σ on FFT period peaks) was calibrated for the much larger DLSS-FG-missed-frames signal.
The data signal is real — the verdict bar is just miscalibrated for this case. See "Detector
shortcomings" below.

## Existing capture modes (`samples/NvFBC/NvFBCR/`)

All modes implement `IFrameCaptureMode` (see `IFrameCaptureMode.h`). Relevant to this work:

- `VsyncCaptureMode.cpp` — `PresentEx(...INTERVAL_ONE)`, blocks on vblank. Works correctly.
- `TimerCaptureMode.cpp` — `CreateWaitableTimerEx(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`,
  `Present(IMMEDIATE)`. Drifts.
- `GPUSleepCaptureMode.cpp` — QPC elapsed + `NvFBCToDx9VidGPUBasedCPUSleep`,
  `Present(IMMEDIATE)`. Drifts identically.
- `VsyncTemporalCaptureMode.cpp`, `VsyncBlendCaptureMode.cpp` — vsync-locked variants
  for temporal/blend processing. Not directly in scope for these fixes but share
  presentation timing with `VsyncCaptureMode`.

The fix should preserve the existing `IFrameCaptureMode` contract and command-line entry
(`NvFBCR.cpp:ParseCaptureMode`). A new mode is generally preferable to mutating an existing
mode, since the existing modes are useful for A/B comparison.

## Why clock-based modes matter (project goals)

`VsyncCaptureMode` is not sufficient on its own because the project aims to support:

1. **Capture/present rates ≠ source display refresh** (e.g., 60 fps CFR output to a capture
   card when the source display is 240 Hz). Vsync mode would present at source vblank, not
   at the target capture card's rate.
2. **NVOFA-interpolated output at exact 16.667 ms cadence from a variable-rate source.**
   This requires explicit scheduling so the interpolation workload can be started in
   advance of each present deadline. A blocking vsync loop offers no advance scheduling
   point — the deadline is only revealed when vblank fires.

Both goals require an explicit schedule. The clock-based modes attempt this but currently
free-run against the target's vblank crystal. The fix is to add a phase reference.

## Phase reference: target display vs source display

In the typical NvFBCR setup:

- **Source display** = the monitor being captured (game's primary). Its vblank is what
  produces new frames into NvFBC's internal buffer.
- **Target display** = the capture card or secondary monitor. Its vblank is when frames
  presented from NvFBCR's D3D9 swapchain reach scan-out and thus reach the capture card's
  input.

The drift that produces hitches is between the schedule clock and the **target** display's
vblank, because that vblank gates when the presented frame becomes visible to the capture
card. All proposed fixes phase-lock to the target display, not the source.

`g_pD3D9Device` is created on `source.dxAdapterIndex` (see `NvFBCR.cpp:InitD3D9`), but the
window is positioned on the target display via `CreateWindowEx` at `target.position.left/top`.
The swapchain's actual scan-out cadence is the target monitor's, regardless of which adapter
the device was created on. Implementations that need to query target-specific timing must
do so by HMONITOR / adapter index of the target display, not the source.

## Validation method

Every proposed fix is validated with the same procedure:

1. Source: Blur Busters UFO Test (testufo.com), 60 fps mode, scrolling background visible
   (e.g., League-of-Legends style scrolling backdrop) for high motion content. Display the
   test on the source display.
2. Target: capture card recording via OBS. The only hard requirement is **zero dropped
   frames during capture** — a recording-side drop is indistinguishable from the drift
   dupes this analysis hunts for, so it invalidates the clip. Use whatever encoder/bitrate
   your pipeline can sustain without dropping (check OBS Stats → skipped/missed frames = 0).
   CBR is fine: the reference clips for this analysis (`frame-drop-analysis/bb_60_x_1_*.mp4`)
   are x264 CBR ~6 Mbps and produce a clean signal (vsync `min(Δ)`=12.8 / 0 dupes; timer
   `min(Δ)`=0.01 / 10 dupes). A true duplicate frame encodes as a near-zero-residual / skip
   P-frame even under CBR — rate control can't manufacture motion that isn't there, it just
   spreads spare bits as sub-MAD-unit noise. The dupe lands at MAD≈0.01 while real motion
   sits at ~13, and the fuzzy MAD-based detection in `detect.py` keys on that 0.01-vs-13
   gap, which CBR's fractional-unit perturbation gets nowhere near closing. (Earlier guidance
   here demanded CRF 15 to avoid CBR "perturbing identical frames"; that was wrong — verified
   against the CBR reference clips. Near-lossless CRF 15 also tends to overwhelm the encoder
   and *cause* dropped frames, which is the one thing that genuinely breaks the analysis.)
3. Record 10 seconds of capture.
4. Run analysis (the inline numpy script in `frame-drop-analysis/` from the diagnostic
   session, or equivalent) and verify against `vsync` baseline:

| Metric | Vsync baseline | Acceptable for fix | Failed fix |
|---|---|---|---|
| `std(delta)` | ~0.44 | ≤ 0.7 | > 1.0 |
| `min(delta)` | ~13.5 | ≥ 10 | < 5 |
| `count(delta < 2.0)` | 0 | 0–1 | ≥ 3 |
| FFT period peaks (P2..P10) | all ≤ 1.0σ | all ≤ 1.5σ | any ≥ 2.0σ |

A fix is considered successful if all four metrics fall in the "acceptable for fix" column.
A 10-second clip is enough — drift slips show up within 1–2 seconds at 60 fps.

Both `detect.py dupes` and `detect.py pacing` are insufficient for this validation as
currently implemented:

- `dupes` uses MD5 on decoded pixels, which encoder noise perturbs. It reports 0 dupes even
  when the underlying motion delta is near zero.
- `pacing` uses period-sigma thresholds calibrated for larger signals (3σ).

Validation should use the percentile-based metrics in the table above, not `detect.py`'s
verdict output.

## Detector shortcomings (out of scope for these specs, noted for context)

`detect.py` would benefit from:

- Perceptual (MAD-threshold) dupe detection instead of hash equality.
- Reporting the `1%(Δ)` and `min(Δ)` percentiles directly so dupe events surface even when
  rare and aperiodic.
- A "smoothness floor" relative metric (e.g., flag clips where `std(Δ)` exceeds 5× the
  smoothest clip in the comparison set).

These are detector improvements, not capture-mode fixes, and belong in `frame-drop-analysis/`,
not here.

## Out of scope

- Changes to NVOFA interpolation logic. The phase-locked clock is a prerequisite for clean
  NVOFA output, but the interpolation itself is in separate specs
  (`samples/NvFBC/NvFBCR/optical_frame_gen_*.md`, `multithreaded_fruc_spec.md`).
- HDR / DX11 output path. Independent of timing fix.
- Source-display VRR handling. The capture target is the phase reference; source-side VRR
  affects what content NvFBC delivers, not when we present it.
- Improvements to `VsyncCaptureMode`. It works correctly.
- Removal of `GPUSleepCaptureMode` / `TimerCaptureMode`. They remain useful for A/B
  comparison until a fixed clock mode lands.
