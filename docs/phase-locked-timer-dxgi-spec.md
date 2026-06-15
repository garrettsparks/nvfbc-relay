# Phase-Locked Timer Capture Mode (DXGI vblank resync)

Read `frame-pacing-drift-analysis.md` first. This spec assumes its diagnosis and validation
method.

This is option 2 of three sibling proposals. The other two
(`phase-locked-timer-dwm-spec.md`, `vblank-driven-scheduler-spec.md`) propose different
phase references / architectures. Pick one to implement — they are mutually exclusive
choices for the same problem.

## Approach

Same algorithm as the DWM option: free-running high-resolution CPU timer drives the
present schedule, periodically resync the schedule to target display vblank to bound
cumulative drift. The difference is the **phase reference**: instead of
`DwmGetCompositionTimingInfo`, this option uses `IDXGIOutput::WaitForVBlank` against the
target display's IDXGIOutput.

DXGI vblank events are reported by the display engine itself, with no DWM-side filtering
or composition quantization. For 60 fps capture the difference vs DWM is likely below
the visibility threshold; for 120/240 fps it may be measurably better. Choosing DXGI also
removes a dependency on DWM cadence semantics in multi-monitor / mixed-refresh setups.

The complication: `WaitForVBlank` blocks. It cannot be polled like
`DwmGetCompositionTimingInfo`. The resync must therefore either (a) run in the main loop
and accept a brief block, or (b) run in a dedicated thread that records vblank QPC
timestamps which the main loop reads. This spec proposes (b) — cleaner architecture, no
loop perturbation, and the thread infrastructure is reusable for downstream NVOFA work
that may also need to know vblank timestamps.

## Design

### New class: `DxgiPhaseLockedCaptureMode`

Add a new mode alongside the existing ones. Do not modify `TimerCaptureMode`.

`samples/NvFBC/NvFBCR/DxgiPhaseLockedCaptureMode.h`:

```cpp
#pragma once
#include "IFrameCaptureMode.h"
#include <dxgi1_2.h>
#include <atomic>
#include <thread>

class DxgiPhaseLockedCaptureMode : public IFrameCaptureMode {
private:
    HANDLE m_timer;
    LARGE_INTEGER m_perfFreq;
    LONGLONG m_perfFreqQuad;
    LONGLONG m_targetPeriodQpc;
    LARGE_INTEGER m_nextPresentQpc;
    float m_framerate;
    int m_framesSinceResync;
    static constexpr int kResyncIntervalFrames = 60;
    static constexpr LONGLONG kPreVblankMarginUs = 1000;

    // DXGI vblank-watcher thread
    IDXGIOutput* m_targetOutput;       // ref-counted COM, target display's output
    std::thread m_vblankThread;
    std::atomic<LONGLONG> m_lastVblankQpc;     // QPC of most recently observed vblank
    std::atomic<LONGLONG> m_observedPeriodQpc; // running-average vblank-to-vblank period
    std::atomic<bool> m_vblankThreadStop;

    void VblankWatcherLoop();
    HRESULT FindTargetOutput(IDirect3D9Ex* d3dEx);

public:
    DxgiPhaseLockedCaptureMode(float framerate);
    virtual ~DxgiPhaseLockedCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(NvFBCToDx9Vid* nvfbcDx9,
                     NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
                     IDirect3DDevice9Ex* device,
                     HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
```

### Locating the target IDXGIOutput

The challenge: NvFBCR uses D3D9, not DXGI. There is no direct `IDirect3DDevice9 ->
IDXGIOutput` path. Two options:

**Option A** (recommended): Create a separate `IDXGIFactory1` just for vblank polling,
enumerate adapters/outputs, and match by HMONITOR.

```cpp
HRESULT FindTargetOutput(HMONITOR targetMonitor) {
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    for (UINT adapterIdx = 0; ; adapterIdx++) {
        IDXGIAdapter1* adapter;
        if (factory->EnumAdapters1(adapterIdx, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT outputIdx = 0; ; outputIdx++) {
            IDXGIOutput* output;
            if (adapter->EnumOutputs(outputIdx, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            if (desc.Monitor == targetMonitor) {
                m_targetOutput = output;  // retain
                adapter->Release();
                factory->Release();
                return S_OK;
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return E_FAIL;
}
```

The HMONITOR of the target display is available via
`g_pD3DEx->GetAdapterMonitor(target.dxAdapterIndex)` — see `NvFBCR.cpp:InitDisplays` for
existing usage. Pass it into `Setup` either via a constructor argument or via the existing
`target` global.

**Option B**: Re-use the D3D9 adapter ordinal directly with
`IDXGIFactory1::EnumAdapters` and select adapter ordinal-N. This is fragile (D3D9 and
DXGI ordinals are not guaranteed to match) and not recommended.

### Vblank watcher thread

```cpp
void DxgiPhaseLockedCaptureMode::VblankWatcherLoop() {
    LONGLONG prev = 0;
    LONGLONG period = m_targetPeriodQpc;  // seeded with target
    while (!m_vblankThreadStop.load()) {
        HRESULT hr = m_targetOutput->WaitForVBlank();
        if (FAILED(hr)) {
            // adapter lost or output torn down — exit
            break;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        m_lastVblankQpc.store(qpc.QuadPart);
        if (prev != 0) {
            LONGLONG delta = qpc.QuadPart - prev;
            // exponentially smoothed period estimate (alpha = 1/16)
            period = period + (delta - period) / 16;
            m_observedPeriodQpc.store(period);
        }
        prev = qpc.QuadPart;
    }
}
```

The thread blocks on `WaitForVBlank`, records a timestamp every vblank, and maintains a
smoothed period estimate. Both values are exposed to the main loop via atomics.

### Main run loop

Identical structure to the DWM option, but the resync reads from atomics:

```cpp
// per iteration:
NvFBCToDx9VidGrabFrame(grabParams);

LARGE_INTEGER now;
QueryPerformanceCounter(&now);
LONGLONG ticksUntilPresent = m_nextPresentQpc.QuadPart - now.QuadPart;
if (ticksUntilPresent > 0) {
    LARGE_INTEGER due;
    due.QuadPart = -(ticksUntilPresent * 10000000 / m_perfFreqQuad);
    SetWaitableTimer(m_timer, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(m_timer, INFINITE);
}

device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

m_nextPresentQpc.QuadPart += m_targetPeriodQpc;
m_framesSinceResync++;

if (m_framesSinceResync >= kResyncIntervalFrames) {
    LONGLONG ref = m_lastVblankQpc.load();
    LONGLONG period = m_observedPeriodQpc.load();
    if (ref > 0 && period > 0) {
        LONGLONG margin = (m_perfFreqQuad * kPreVblankMarginUs) / 1000000;
        LONGLONG delta = m_nextPresentQpc.QuadPart + margin - ref;
        LONGLONG nVblanks = (delta + period / 2) / period;  // round to nearest
        m_nextPresentQpc.QuadPart = ref + nVblanks * period - margin;
    }
    m_framesSinceResync = 0;
}

// message pump as before
```

Compared to the DWM option:

- Uses `m_lastVblankQpc` and `m_observedPeriodQpc` atomics instead of
  `DwmGetCompositionTimingInfo`. Conceptually identical math.
- The smoothed observed period (rather than the nominal `m_targetPeriodQpc`) is what
  drives the snap. This corrects for any small discrepancy between the configured target
  framerate and the actual display refresh — for instance, "60 fps" against a 59.94 Hz
  output naturally lands on 59.94 Hz cadence over time.

### Setup

1. Create timer (same as DWM option).
2. `QueryPerformanceFrequency`, compute `m_targetPeriodQpc`.
3. Get target HMONITOR (via D3D9 adapter, see "Locating the target IDXGIOutput").
4. `FindTargetOutput(hmonitor)` → set `m_targetOutput`.
5. Spawn vblank thread: `m_vblankThread = std::thread(&DxgiPhaseLockedCaptureMode::VblankWatcherLoop, this);`
6. Wait briefly (e.g., up to 100ms) for first vblank observation so the thread has
   primed `m_lastVblankQpc`. Bail with an error if no vblank observed in that window.
7. Seed `m_nextPresentQpc` from current observed vblank + first frame offset.

### Teardown

1. `m_vblankThreadStop.store(true)`.
2. The thread may still be inside `WaitForVBlank`. It will wake on the next vblank
   (within ~16 ms) and exit. `m_vblankThread.join()` blocks until then. Acceptable.
3. Release `m_targetOutput`.
4. Close timer handle.

If a faster shutdown is needed, `IDXGIOutput` doesn't have a cancel mechanism — best
alternative is to call `Release()` on the output from the main thread, which causes the
next `WaitForVBlank` to fail and exit. Not strictly required for v1.

### Edge cases

- **Adapter / output lost during runtime.** `WaitForVBlank` returns failure; thread exits.
  Main loop loses its phase reference and free-runs. Detect by checking
  `m_lastVblankQpc` staleness (e.g., > 100 ms since last update) and either disable
  resync or terminate the mode with an error.
- **Target output moved to a different adapter** (rare, e.g., during graphics driver
  restart). Handle as adapter-lost above.
- **High GPU load delaying vblank observation.** `WaitForVBlank` is normally accurate to
  within microseconds even under heavy GPU load (it's a kernel-side wait on the display
  engine), but on very contested systems it can quantize. The exponentially-smoothed
  period estimate absorbs single-vblank noise; the snap-to-nearest rounding absorbs
  small per-resync errors.
- **Frame work overruns period.** Same handling as DWM option: skip wait, present
  immediately, advance schedule by one period only.

### Wiring

Same as DWM option. Add a branch in `NvFBCR.cpp::ParseCaptureMode` with a chosen
identifier (suggest `d:60`, `d:vsync` for "DXGI" phase-lock; or use `p:` and treat DXGI
vs DWM as a build-time choice — discuss with project owner). Update help text.

## Validation

Per `frame-pacing-drift-analysis.md`. Same target metrics:

- `std(Δ) ≤ 0.7`
- `min(Δ) ≥ 10`
- `count(Δ < 2.0) ≤ 1`
- All P2..P10 sigma ≤ 1.5σ

## Trade-offs vs the other two options

vs **DWM phase-lock** (`phase-locked-timer-dwm-spec.md`):
- More accurate phase reference (direct from display engine vs filtered through DWM).
- Adds a dedicated thread and atomics — more moving parts.
- Adds a DXGI dependency to the codebase (currently D3D9-only). Small dependency,
  available since Windows 7, but a new include / lib.
- More resilient in multi-monitor mixed-refresh setups where DWM's compose cadence may
  not match the target's true vblank.

vs **vblank-driven scheduler** (`vblank-driven-scheduler-spec.md`):
- Same advantages as the DWM option: preserves rate-independence, gives downstream NVOFA
  an explicit advance schedule, smaller diff vs existing TimerCaptureMode structure.
- The vblank-watcher thread infrastructure is reusable for the vblank-driven option, so
  this can be a stepping stone if the vblank-driven approach is later preferred.

Recommend implementing this option over DWM if the project anticipates needing higher
frame rates (120+) or expects to run in multi-monitor configurations where DWM cadence
may not reflect the target display. The DWM option is slightly simpler if neither of
those applies.

## Out of scope

Same as DWM option: NVOFA integration, source VRR, removing existing modes, multi-target
output.
