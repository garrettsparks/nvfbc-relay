# Vblank-Driven Scheduler Capture Mode

Read `frame-pacing-drift-analysis.md` first. This spec assumes its diagnosis and validation
method.

This is option 3 of three sibling proposals. The other two
(`phase-locked-timer-dwm-spec.md`, `phase-locked-timer-dxgi-spec.md`) propose periodic-
resync approaches that keep a CPU clock as the primary scheduler. This proposal eliminates
the CPU clock entirely and uses vblank events as the schedule.

## Approach

Restructure the capture loop so that **each target-display vblank fires one capture +
present cycle**. There is no CPU timer driving the schedule — `IDXGIOutput::WaitForVBlank`
is the primary scheduling primitive. The CPU clock is only used for measuring elapsed
time within a frame (to know how long work took), not for deciding when to act.

By construction, this design cannot drift relative to the target display, because the
target's vblank *is* the schedule. There is no second clock to drift against.

This differs from `VsyncCaptureMode` in one important way: `VsyncCaptureMode` blocks
inside `Present(INTERVAL_ONE)`, which means the loop only resumes *after* vblank — i.e.,
into the new scan-out period — with no opportunity to do scheduled work *before* the
present deadline. This mode blocks on `WaitForVBlank` *before* `Present(IMMEDIATE)`, which
lets work happen in the gap between waking and presenting. That gap is the natural home
for NVOFA interpolation work scheduled against the upcoming present deadline.

## When this mode is appropriate

This mode is only appropriate when the target capture rate is exactly equal to the target
display refresh rate, **or** is a clean integer divisor of it (e.g., capture at 60 fps
against a 120 Hz target display by skipping every other vblank). Arbitrary rates
(e.g., 60 fps against 144 Hz, or 59.94 fps against 60 Hz) cannot be cleanly served by
this design — you'd need a separate clock to produce the divergent rate, at which point
you're back to the phase-locked-timer approaches.

For the project's current shipping configuration (60 fps capture against a 60 Hz capture
card) this mode fits cleanly. If the project later supports arbitrary capture rates,
one of the phase-locked-timer options is the right choice instead.

## Design

### New class: `VblankDrivenCaptureMode`

Add a new mode alongside the existing ones. Do not modify `TimerCaptureMode` or
`VsyncCaptureMode`.

`samples/NvFBC/NvFBCR/VblankDrivenCaptureMode.h`:

```cpp
#pragma once
#include "IFrameCaptureMode.h"
#include <dxgi1_2.h>

class VblankDrivenCaptureMode : public IFrameCaptureMode {
private:
    IDXGIOutput* m_targetOutput;     // ref-counted COM, target display's output
    int m_vblanksPerPresent;         // 1 for native rate, 2 for half rate, etc.
    float m_framerate;               // for logging only — derived from display refresh
    HMONITOR m_targetMonitor;        // captured at construction for locating output

    HRESULT FindTargetOutput();

public:
    VblankDrivenCaptureMode(float framerate);  // pass 0.0f to mean "match display"
    virtual ~VblankDrivenCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(NvFBCToDx9Vid* nvfbcDx9,
                     NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
                     IDirect3DDevice9Ex* device,
                     HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
```

`GetPresentationInterval()` returns `D3DPRESENT_INTERVAL_IMMEDIATE` — the loop owns the
vblank wait via DXGI; Present must not block on D3D9-side vsync.

### Rate selection

Constructor takes a framerate. At setup:

1. Query the target display refresh rate via `IDXGIOutput::GetDisplayModeList` or simpler
   `g_pD3DEx->GetAdapterDisplayMode(target.dxAdapterIndex, &mode)` → `mode.RefreshRate`.
2. If framerate == 0 or matches display refresh, `m_vblanksPerPresent = 1`.
3. If display refresh is a clean integer multiple of framerate (within tolerance, e.g.,
   2.5%), `m_vblanksPerPresent = round(displayRefresh / framerate)`. Log the actual
   effective rate.
4. Otherwise, fail setup with a clear error: this mode does not support arbitrary
   capture rates; recommend `phase-locked-timer-*` mode instead.

### Locating the target IDXGIOutput

Identical to the DXGI phase-lock spec — see `phase-locked-timer-dxgi-spec.md` →
"Locating the target IDXGIOutput". Reuse that logic.

### Run loop

```cpp
void VblankDrivenCaptureMode::Run(NvFBCToDx9Vid* nvfbcDx9,
                                  NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
                                  IDirect3DDevice9Ex* device,
                                  HWND hwnd) {
    MSG msg = {};
    while (true) {
        // 1. Wait for next target vblank
        HRESULT hr = m_targetOutput->WaitForVBlank();
        if (FAILED(hr)) {
            LOGERR("WaitForVBlank failed (0x%08x); target output may be lost", hr);
            break;
        }

        // 2. If subsampling, consume additional vblanks
        for (int i = 1; i < m_vblanksPerPresent; i++) {
            if (FAILED(m_targetOutput->WaitForVBlank())) goto out;
        }

        // 3. Capture (nowait — returns latest available frame)
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);
        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated");
            break;
        }

        // 4. (Future: NVOFA interpolation work happens here, in the gap
        //     between vblank wake and the present below. The gap is approximately
        //     one full frame period minus capture time, which gives ~14ms of
        //     headroom at 60Hz for a few microseconds of NvFBC capture.)

        // 5. Present immediately — lands on current scan-out
        device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

        // 6. Pump messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;
    }
out:;
}
```

Key properties:

- **No CPU timer.** The schedule is the target's vblank cadence, period.
- **No drift possible.** There is no second clock.
- **Present is non-blocking.** The vblank wait happens explicitly via DXGI; Present(IMMEDIATE)
  posts the frame for the *next* scan-out without blocking.
- **The work gap before Present is the NVOFA insertion point.** Currently capture is the
  only work done there, taking microseconds. NVOFA work, when added, slots in between
  capture and present (step 4 above).

### Why this differs from VsyncCaptureMode

| Property | VsyncCaptureMode | VblankDrivenCaptureMode |
|---|---|---|
| What blocks the loop | `Present(INTERVAL_ONE)` | `IDXGIOutput::WaitForVBlank` |
| Order | capture → present (which blocks until vblank) | wait for vblank → capture → present (immediate) |
| Time available for pre-present work | None — Present *is* the wait | ~one full period — Present runs after work |
| Drift behavior | Phase-locked by construction | Phase-locked by construction |
| Supports rates ≠ display refresh | No (only display refresh) | Yes, via integer divisor only |
| Hook point for NVOFA | None — work would have to land in next-frame budget | Between vblank-wake and Present, current-frame budget |

If NVOFA interpolation is the primary downstream goal, this mode is strictly better than
`VsyncCaptureMode` because it exposes the pre-present work gap. If NVOFA is not in scope,
this mode and `VsyncCaptureMode` produce equivalent output at the same rate — they differ
only in where the program spends its time within each frame period.

### Edge cases

- **Target output lost / adapter restart.** `WaitForVBlank` fails. Loop exits cleanly.
- **Frame work (capture + future NVOFA) exceeds one period.** Present will still fire after
  the work completes, but it will land in a later scan-out than intended. Subsequent
  vblank wait sees the next vblank correctly; net effect is one dropped output frame, not
  cascading drift. Worth logging when this happens (compare QPC at wait-wake vs at
  present-call).
- **Display refresh changes at runtime** (mode change, monitor swap). `WaitForVBlank`
  continues to track the new vblank rate, but `m_vblanksPerPresent` is fixed at setup so
  the effective output rate would change. Detect via periodic re-query of refresh rate
  and either re-initialize the mode or fail with a clear message.
- **Rate mismatch detected at setup** (capture rate is not an integer divisor of refresh).
  Fail setup with an error message recommending one of the phase-locked-timer modes.

### Wiring

Add a branch in `NvFBCR.cpp::ParseCaptureMode`. Suggest identifiers `v:60`, `v:vsync`
("v" for "vblank-driven") — distinct from existing `t:` (timer), `b:` (blend), and the
phase-locked variants. Update help text accordingly. Discuss naming with the project owner
before settling.

## Validation

Per `frame-pacing-drift-analysis.md`. Same target metrics:

- `std(Δ) ≤ 0.7`
- `min(Δ) ≥ 10`
- `count(Δ < 2.0) ≤ 1`
- All P2..P10 sigma ≤ 1.5σ

By design this mode should be **identical** to `VsyncCaptureMode` on these metrics — they
have the same effective scheduling source. Any discrepancy would indicate a subtle
implementation bug (e.g., Present being called too late after wake-up). Treat
`VsyncCaptureMode`'s numbers as ground truth.

## Trade-offs vs the other two options

vs **DWM phase-lock** / **DXGI phase-lock**:
- Simpler conceptually — no resync logic, no drift dynamics to reason about.
- Cannot serve arbitrary capture rates. Limited to display refresh and integer divisors.
- For NVOFA: gives a guaranteed natural insertion point (the wait-to-present gap) where
  work can happen on the current frame's budget. The phase-locked-timer modes also expose
  this gap, but their schedule is CPU-driven rather than display-driven, so any disagreement
  between timer and vblank ends up as schedule jitter NVOFA has to absorb. This mode
  removes that disagreement by design.
- More invasive structurally than the phase-locked-timer modes. Different loop shape,
  different blocking primitive, different setup requirements.

Recommend implementing this option if:
- The project's target rate equals (or is a clean divisor of) the target display refresh,
  AND
- NVOFA interpolation is the primary downstream goal and you want the cleanest possible
  scheduling for it.

Do **not** implement this option if the project needs arbitrary capture rates — pick one
of the phase-locked-timer specs instead.

## Out of scope

Same as the other two options: NVOFA integration (this mode prepares the ground but does
not implement NVOFA), source VRR, removing existing modes, multi-target output.
