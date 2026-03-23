# GPUSleepCaptureMode Spec

## Problem

VsyncCaptureMode synchronizes output by blocking on `D3DPRESENT_INTERVAL_ONE`, which ties
frame pacing to the capture card's display refresh. When the card's 60Hz vsync drifts
relative to the game's frame delivery, FCAT analysis shows bimodal frame gaps (alternating
short/long) instead of consistent spacing. Gap mode consistency drops from ~95% to ~40%
during drift episodes.

TimerCaptureMode avoids vsync but uses Windows waitable timers, which are limited to ~1ms
granularity (the OS timer quantum). This is better than vsync-locked but still coarse for
precise frame pacing at 60fps (16.667ms intervals).

## Solution

A new capture mode that uses `NvFBCToDx9VidGPUBasedCPUSleep` for frame timing. This API
provides sub-quantum microsecond-precision sleep without burning CPU cycles, giving the
precision benefits of a spin-wait with the efficiency of a kernel sleep.

### About NvFBCToDx9VidGPUBasedCPUSleep

This is an undocumented NVIDIA API with no public documentation or search results. Everything
known comes from the NvFBC SDK header (`nvFBCToDx9Vid.h`):

```
A high precision implementation of Sleep().
Can provide sub quantum (usually 16ms) sleep that does not burn CPU cycles.
param [in] qwMicroSeconds The number of microseconds that the thread should sleep for.
return An applicable NVFBCRESULT value.
```

The "GPU-based" name suggests it uses GPU timer hardware to achieve sub-quantum precision,
which would explain how it avoids both CPU spin-waiting and the OS timer quantum limitation.
The same API exists on all three NvFBC interfaces (ToDx9Vid, ToCuda, ToSys), indicating it's
a deliberate cross-interface feature likely intended for frame-pacing use cases.

**Precision claims are unverified.** The actual jitter characteristics and whether it truly
delivers microsecond-level accuracy need to be validated with FCAT analysis after
implementation. The existing TimerCaptureMode (using `SetWaitableTimer` + `WaitForSingleObject`
without `timeBeginPeriod`) is limited by the default Windows timer quantum of ~15.6ms, so
even modest improvement from this API would be significant for 60fps (16.667ms) frame pacing.

## Design

### Class: GPUSleepCaptureMode

Implements `IFrameCaptureMode`. Structurally identical to `TimerCaptureMode` with the
timing mechanism replaced.

**Header** (`GPUSleepCaptureMode.h`):
```cpp
class GPUSleepCaptureMode : public IFrameCaptureMode {
private:
    __int64 m_targetIntervalUs;       // target frame interval in microseconds
    float m_framerate;
    LARGE_INTEGER m_perfFreq;         // QPC frequency for elapsed time measurement

public:
    GPUSleepCaptureMode(float framerate);
    virtual ~GPUSleepCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
```

**Key differences from TimerCaptureMode:**

| Aspect | TimerCaptureMode | GPUSleepCaptureMode |
|--------|------------------|---------------------|
| Timing primitive | `CreateWaitableTimer` + `SetWaitableTimer` + `WaitForSingleObject` | `NvFBCToDx9VidGPUBasedCPUSleep` |
| Interval storage | `LARGE_INTEGER` (100ns units, negative = relative) | `__int64` (microseconds) |
| Precision | ~1ms (OS timer quantum) | Sub-quantum microseconds |
| Setup resources | Creates timer handle | None (uses NvFBC API directly) |
| Cleanup | `CloseHandle(m_timer)` | None |
| CPU usage | Blocks on kernel wait object | GPU-assisted sleep, does not burn CPU |

### Capture Loop

```
loop:
    1. Record frame start time (QueryPerformanceCounter)
    2. Grab frame (NOWAIT flag already set in grabParams by caller)
    3. Present with D3DPRESENT_INTERVAL_IMMEDIATE (no vsync blocking)
    4. Process Windows messages
    5. Measure elapsed work time since frame start
    6. Sleep for max(0, m_targetIntervalUs - elapsedUs) via NvFBCToDx9VidGPUBasedCPUSleep
```

**Work-time compensation:** TimerCaptureMode gets this for free — `SetWaitableTimer` starts
a countdown at the top of the loop that runs concurrently with grab+present+messages, so
`WaitForSingleObject` at the bottom only blocks for `target_interval - work_time`.

`NvFBCToDx9VidGPUBasedCPUSleep` is a blocking sleep, not a timer you can start early and
wait on later. To achieve the same consistent frame-to-frame intervals, we measure elapsed
work time with `QueryPerformanceCounter` and subtract it from the target interval before
sleeping. If work took longer than the target interval (unlikely but possible under load),
we skip the sleep entirely and proceed immediately — the frame is already late, sleeping
would only make it worse.

### Interval Calculation

```cpp
m_targetIntervalUs = (__int64)(1000000.0f / framerate);
```

Examples:
- 60 fps -> 16667 us
- 59.94 fps -> 16683 us
- 30 fps -> 33333 us

### CLI Integration

New prefix `g:` in `ParseCaptureMode`:

| Input | Mode |
|-------|------|
| `g:60` | GPUSleepCaptureMode at 60 fps |
| `g:59.94` | GPUSleepCaptureMode at 59.94 fps |

Parsing follows the same pattern as `t:` and `b:` prefixes. Same validation:
`framerate > 0.0f && framerate <= 1000.0f`.

Add to error help text:
```
  g:60             - GPU sleep mode (sub-ms precision timer at specified fps)
```

### File Changes

1. **New: `GPUSleepCaptureMode.h`** - Class declaration
2. **New: `GPUSleepCaptureMode.cpp`** - Implementation
3. **Modified: `NvFBCR.cpp`** - Add `#include "GPUSleepCaptureMode.h"`, add `g:` parsing
   block, update error help text

### Presentation Interval

Returns `D3DPRESENT_INTERVAL_IMMEDIATE` (same as TimerCaptureMode). The GPU sleep controls
pacing, not vsync.

### Error Handling

- `NvFBCToDx9VidGPUBasedCPUSleep` return value: The API returns `NVFBCRESULT`. Log on
  failure but continue the loop (non-fatal — a missed sleep just means one frame arrives
  slightly early).
- `NVFBC_ERROR_INVALIDATED_SESSION` from grab: break out of loop (same as all modes).

## Not In Scope

- Temporal or blend variants using GPU sleep (can be added later as `gt:` and `gb:` if
  needed, following the existing pattern of VsyncTemporalCaptureMode vs
  FrameTemporalCaptureMode).
- Adaptive sleep that further refines timing beyond QPC-based work-time compensation (e.g.,
  rolling average correction for systematic over/undershoot from the sleep API itself).
