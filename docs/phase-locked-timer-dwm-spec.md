# Phase-Locked Timer Capture Mode (DWM resync)

Read `frame-pacing-drift-analysis.md` first. This spec assumes its diagnosis and validation
method.

This is option 1 of three sibling proposals for fixing drift in clock-based capture modes.
The other two (`phase-locked-timer-dxgi-spec.md`, `vblank-driven-scheduler-spec.md`) propose
different phase references / architectures. Pick one to implement — they are mutually
exclusive choices for the same problem, not stages of one plan.

## Approach

Keep `TimerCaptureMode`'s structure (free-running high-resolution CPU timer, immediate
non-blocking present), but **periodically resync the timer's next-fire QPC to the target
display's vblank** so cumulative drift can never exceed the resync interval. Between
resyncs the timer free-runs at exactly the target framerate; at each resync the schedule
is snapped to align with vblank.

Phase reference: `DwmGetCompositionTimingInfo`. This function returns the QPC of the next
DWM composition vblank and the refresh period in QPC ticks.

```c
typedef struct _DWM_TIMING_INFO {
    UINT32 cbSize;
    UNSIGNED_RATIO rateRefresh;     // monitor refresh rate
    QPC_TIME qpcRefreshPeriod;      // monitor refresh period in QPC ticks
    UNSIGNED_RATIO rateCompose;     // compose rate
    QPC_TIME qpcVBlank;             // QPC of last vblank
    DWM_FRAME_COUNT cRefresh;       // running refresh counter
    // ... many more fields, see dwmapi.h
} DWM_TIMING_INFO;

HRESULT DwmGetCompositionTimingInfo(HWND hwnd, DWM_TIMING_INFO *info);
```

DWM cadence in Windows 10/11 desktop composition is generally the highest-refresh-rate
attached display, but when an exclusive-fullscreen swapchain is the target window, DWM
reports timing for that swapchain. For NvFBCR's windowed pseudo-fullscreen output to the
target display, DWM's reported vblank corresponds to the target's vblank in practice.
Edge cases are handled below.

## Design

### New class: `PhaseLockedTimerCaptureMode`

Add a new mode alongside the existing ones. Do not modify `TimerCaptureMode` — it's needed
as an A/B comparison baseline.

`samples/NvFBC/NvFBCR/PhaseLockedTimerCaptureMode.h`:

```cpp
#pragma once
#include "IFrameCaptureMode.h"

class PhaseLockedTimerCaptureMode : public IFrameCaptureMode {
private:
    HANDLE m_timer;
    LARGE_INTEGER m_perfFreq;
    LONGLONG m_perfFreqQuad;       // cached for arithmetic
    LONGLONG m_targetPeriodQpc;    // target frame interval in QPC ticks
    LARGE_INTEGER m_nextPresentQpc; // absolute QPC time for next scheduled present
    HWND m_hwnd;                   // for DwmGetCompositionTimingInfo
    float m_framerate;
    int m_framesSinceResync;
    static constexpr int kResyncIntervalFrames = 60;  // resync ~once per second at 60 fps
    static constexpr LONGLONG kPreVblankMarginUs = 1000;  // present 1ms before vblank target

public:
    PhaseLockedTimerCaptureMode(float framerate);
    virtual ~PhaseLockedTimerCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(NvFBCToDx9Vid* nvfbcDx9,
                     NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
                     IDirect3DDevice9Ex* device,
                     HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
```

`GetPresentationInterval()` returns `D3DPRESENT_INTERVAL_IMMEDIATE` — the mode owns its
own scheduling and must not have Present block on vblank.

### Setup

1. Create high-res waitable timer:
   ```cpp
   m_timer = CreateWaitableTimerEx(NULL, NULL,
       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
       TIMER_ALL_ACCESS);
   ```
2. `QueryPerformanceFrequency(&m_perfFreq)`, cache `m_perfFreqQuad = m_perfFreq.QuadPart`.
3. Compute `m_targetPeriodQpc = m_perfFreqQuad / m_framerate` (use exact float division, then
   round to nearest LONGLONG to minimize accumulated rounding error).
4. Stash `m_hwnd` (passed to `Run`, store before main loop).
5. Initial phase: query DWM timing once, set `m_nextPresentQpc = qpcVBlank +
   (m_perfFreqQuad * kPreVblankMarginUs) / 1000000` clamped forward to first vblank after
   "now."

### Run loop

Per iteration:

```cpp
// 1. capture
NvFBCToDx9VidGrabFrame(grabParams);

// 2. wait until scheduled present time
LARGE_INTEGER now;
QueryPerformanceCounter(&now);
LONGLONG ticksUntilPresent = m_nextPresentQpc.QuadPart - now.QuadPart;
if (ticksUntilPresent > 0) {
    LARGE_INTEGER due;
    due.QuadPart = -(ticksUntilPresent * 10000000 / m_perfFreqQuad);  // 100ns units, negative = relative
    SetWaitableTimer(m_timer, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(m_timer, INFINITE);
}

// 3. present immediately
device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

// 4. advance schedule
m_nextPresentQpc.QuadPart += m_targetPeriodQpc;
m_framesSinceResync++;

// 5. periodic resync
if (m_framesSinceResync >= kResyncIntervalFrames) {
    DWM_TIMING_INFO timing = { sizeof(timing) };
    if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &timing))) {
        // find the vblank closest to (but not before) our current m_nextPresentQpc,
        // and snap m_nextPresentQpc to (that vblank - preVblankMargin)
        LONGLONG period = timing.qpcRefreshPeriod;
        LONGLONG ref = timing.qpcVBlank;
        LONGLONG margin = (m_perfFreqQuad * kPreVblankMarginUs) / 1000000;
        // round to nearest vblank at or after current schedule
        LONGLONG delta = m_nextPresentQpc.QuadPart + margin - ref;
        LONGLONG nVblanks = (delta + period / 2) / period;  // nearest, not floor
        m_nextPresentQpc.QuadPart = ref + nVblanks * period - margin;
    }
    m_framesSinceResync = 0;
}

// 6. message pump
while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { ... }
if (msg.message == WM_QUIT) break;
```

The resync step does NOT reset the schedule to "the next vblank" — that would create a
visible hitch every resync interval. Instead it finds the vblank *closest* to where the
schedule says we should be, and aligns to that. Net effect: ±half-vblank correction
applied gradually, no visible step.

### Drift behavior

- Free-running: timer drifts ~0.01–0.1% relative to display PLL → over 60 frames at 60 fps
  (~1 second) cumulative drift is ~10–100 μs, well under the 16.667 ms frame period.
- At resync: schedule snaps to nearest vblank. If accumulated drift was 50 μs, the snap
  shifts present time by 50 μs; imperceptible.
- Long-term: bounded forever. Drift never has a chance to slip a full frame.

`kResyncIntervalFrames = 60` is a starting point. If drift is faster than expected, lower
it. If `DwmGetCompositionTimingInfo` has its own jitter (which is plausible since DWM
quantizes to its compose tick), raise it — too-frequent resync amplifies DWM-side noise.

### Edge cases

- **`DwmGetCompositionTimingInfo` fails.** Log and skip the resync; rely on free-running
  timer until next attempt. Don't crash, don't reinitialize phase.
- **DWM composition disabled** (legacy, very rare on Win10+). The call returns failure;
  handle as above. Degenerate behavior matches old `TimerCaptureMode`.
- **Target display refresh ≠ DWM compose rate.** DWM may compose at a different rate than
  the target's true vblank when multiple monitors with different refreshes are connected.
  Mitigation: detect mismatch (compare `qpcRefreshPeriod` to expected `m_targetPeriodQpc`)
  and warn / disable resync if discrepancy is large. For NvFBCR's common single-target-
  display setup this won't trigger.
- **Frame work takes longer than `m_targetPeriodQpc`.** `ticksUntilPresent` goes negative;
  skip the wait, present immediately, advance schedule by one period anyway (we're already
  late, advancing only one period rather than catching up entirely prevents runaway).
- **WM_QUIT during wait.** `WaitForSingleObject(INFINITE)` doesn't process messages. Either
  use `MsgWaitForMultipleObjects` instead, or split the wait into smaller chunks. The
  existing `TimerCaptureMode` has the same issue; matching its behavior is acceptable for
  this fix.

### Wiring

`NvFBCR.cpp::ParseCaptureMode` already pattern-matches mode strings (see `NvFBCR.cpp:77`).
Add a new branch for a chosen identifier (suggest: `p:60`, `p:vsync`, mirroring existing
`t:` and `b:` prefixes — "p" for "phase-locked"). Update the help text in both `NvFBCR.cpp`
and `ConsoleUserInput`.

The mode is constructed the same way `TimerCaptureMode` is, taking a float framerate.

## Validation

Per the procedure in `frame-pacing-drift-analysis.md`, with target metrics:

- `std(Δ) ≤ 0.7`
- `min(Δ) ≥ 10`
- `count(Δ < 2.0) ≤ 1` over a 10s clip
- All P2..P10 sigma ≤ 1.5σ

If the implementation passes these on UFO test, it should also pass on real gameplay,
since UFO test is the more sensitive substrate.

## Trade-offs vs the other two options

vs **DXGI WaitForVBlank phase-lock** (`phase-locked-timer-dxgi-spec.md`):
- DWM API is simpler — single non-blocking call, no separate thread or polling required.
- DWM's vblank timing is filtered through DWM's compose engine and may be less precise
  than a direct IDXGIOutput query. For 60 fps capture with periodic resync this precision
  difference is below the threshold of visibility, but for higher rates (120/240) DXGI
  may be measurably better.
- DWM API needs the window's HWND; DXGI needs an IDXGIOutput. Either is straightforward
  but they have different setup paths.

vs **vblank-driven scheduler** (`vblank-driven-scheduler-spec.md`):
- This approach preserves the ability to capture/present at rates ≠ target refresh
  (e.g., 60 CFR output from a 120 Hz target). The vblank-driven approach cannot do this
  without explicit Nth-vblank logic.
- This approach gives you an explicit schedule that downstream NVOFA interpolation can
  use to plan work in advance. Vblank-driven only signals "now" — work scheduling is
  reactive.
- This approach is closer to the existing `TimerCaptureMode` structure; smaller diff,
  lower risk.

Recommend implementing this option first if you want the lowest-risk path that addresses
the primary use case. The other two are valuable if 60 fps proves insufficient or if NVOFA
turns out to need a different scheduling model.

## Out of scope

- NVOFA integration. This mode produces a clean schedule that NVOFA can hook into later.
- Source display VRR. Phase reference is the target display only.
- Removing `TimerCaptureMode` or `GPUSleepCaptureMode`. Both retained for A/B testing.
- Multi-target output. Single target, single phase reference.
