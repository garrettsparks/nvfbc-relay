# Multithreaded FRUC: Capture Thread + VSync Present Thread

## Motivation

The single-threaded FRUC loop has a fundamental tension:
- **Capture is event-driven**: new source frames arrive at a variable rate (90fps, 144fps, etc.)
- **Presentation is time-driven**: VBlank fires at the output display's fixed refresh rate (60Hz)

In a single thread, one blocks the other. With VSync present, `PresentEx(INTERVAL_ONE)`
blocks ~14ms per frame. During that block, no captures happen — source frames are missed.

### Measured evidence (single-threaded)

| Source fps | pair_delta avg | flow (ms) | weight avg | outliers/300 | Quality |
|-----------|---------------|-----------|------------|--------------|---------|
| 60        | 5.6ms         | 1.2       | 0.75       | 0            | Good    |
| 90        | 5.6-16.7ms*   | 1.3-6.6   | 0.75-0.95  | 0-47         | OK→Bad  |
| 120       | ~16.7ms       | 6-16      | 0.95-0.98  | 6-45         | Bad     |

*90fps: pair_delta=5.6ms on menus (loop is fast), 16.7ms during gameplay (loop slows down).

**The problem**: When flow takes >3ms, the total loop time approaches 16.7ms and pair_delta
locks to one VBlank interval. At that point weight≈0.95 and we're just frame-selecting,
not interpolating. NvOF is doing expensive work for no benefit.

**With multithreaded capture**: The ring buffer stays at source rate regardless of flow cost.
At 90fps → pair_delta=11ms. At 120fps → pair_delta=8.3ms. NvOF sees half the motion per
pair → faster execution → more headroom. Weight spreads meaningfully → real interpolation.

## Architecture

```
┌─────────────────────────────────┐     ┌──────────────────────────────────────┐
│       Capture Thread            │     │         Present Thread (main)        │
│                                 │     │                                      │
│  loop:                          │     │  loop:                               │
│    NvFBC grab (WAIT_WITH_TIMEOUT│     │    PresentEx(INTERVAL_ONE)           │
│                       100ms)    │     │      ← blocks until VBlank           │
│    if SUCCESS:                  │     │                                      │
│      Set flag: newFrameReady    │     │    LatchCapturedFrame():             │
│      Signal event               │     │      if newFrameReady:               │
│    Check m_captureRunning       │     │        StretchRect capture→interop   │
│                                 │     │        CUDA map → ring buffer → unmap│
│  (Only NvFBC API calls here.    │     │        Advance ring buffer           │
│   No D3D9. No CUDA.)           │     │        flowComputedForCurrentPair=F  │
│                                 │     │                                      │
│                                 │     │    If pair valid && !flowComputed:   │
│                                 │     │      ComputeOpticalFlow()            │
│                                 │     │                                      │
│                                 │     │    Calculate weight from VBlank time │
│                                 │     │    InterpolateFrame(weight)          │
│                                 │     │    Copy output → D3D9 → backbuffer   │
│                                 │     │    (PresentEx at top of next iter)   │
└─────────────────────────────────┘     └──────────────────────────────────────┘
         │                                           │
         │  Shared state:                            │
         │  - m_captureTarget (D3D9 surface)         │
         │  - newFrameReady flag (atomic)            │
         │  - newFrameEvent (Windows auto-reset)     │
         └───────────────────────────────────────────┘
```

## Key Design Decisions

**1. Capture thread only calls NvFBC — no D3D9, no CUDA.**

D3D9 is not thread-safe. By keeping all D3D9 calls (StretchRect, PresentEx) and all CUDA
calls (interop map/unmap, NvOF, kernel) on the present thread, we avoid all D3D9 threading
issues. The capture thread's only job is calling `NvFBCToDx9VidGrabFrame` and signaling.

**2. NvFBC grab uses WAIT_WITH_TIMEOUT(100ms) for clean shutdown.**

Originally considered NOFLAGS (fully blocking), but a blocked grab can't check the shutdown
flag. WAIT_WITH_TIMEOUT(100ms) guarantees the thread exits within 100ms of shutdown request.
At source rates >10fps, the timeout rarely fires — NvFBC returns SUCCESS before it expires.

**3. Synchronization: atomic flag + Windows event.**

```cpp
std::atomic<bool> m_newFrameReady;   // Set by capture thread, cleared by present thread
std::atomic<bool> m_captureRunning;  // Shutdown signal
std::atomic<bool> m_sessionInvalidated;  // Fatal error from capture thread
HANDLE m_newFrameEvent;              // Auto-reset event
```

The event isn't required for correctness — after PresentEx returns from VBlank, the present
thread polls `m_newFrameReady`. But we include it so we could later use `WaitForSingleObject`
if we want the present thread to wake between VBlanks.

**4. m_captureTarget is the shared surface — signal-then-read protocol.**

NvFBC writes to `m_captureTarget` on the capture thread. The present thread reads it via
`StretchRect` only after seeing `newFrameReady = true`. Race window exists if NvFBC starts
the NEXT grab while StretchRect reads. At 1280x720, StretchRect takes ~5μs — window is tiny.

**Mitigation if corruption observed**: Add mutex around capture target access (see Risks).

**5. Capture thread gets its own copy of grab params.**

The global grab params in NvFBCR.cpp use WAIT_WITH_TIMEOUT(2ms). The capture thread needs
WAIT_WITH_TIMEOUT(100ms). Rather than mutating the caller's struct, copy it:
```cpp
m_captureGrabParams = *grabParams;
m_captureGrabParams.dwFlags = NVFBC_TODX9VID_WAIT_WITH_TIMEOUT;
m_captureGrabParams.dwWaitTime = 100;
```

**6. Weight calculation stays VBlank-relative.**

Same approach as single-threaded: `weight = (lastVBlankTime - t0) / (t1 - t0)`. The
difference is that t0 and t1 are now ~11ms apart (at 90fps source) instead of ~16.7ms,
so the VBlank time falls at a more meaningful position within the pair.

**7. Present at top of loop, not bottom.**

```
Present (blocks ~14ms) → Latch → Flow → Interpolate → [loop]
```

This means the VBlank block happens WHILE the capture thread is grabbing frames. By the
time PresentEx returns, the capture thread has likely grabbed 1+ new source frames. We
latch the latest one immediately, minimizing staleness.

## Implementation Plan

### Step 1: New members in FrucCaptureMode.h

```cpp
#include <atomic>

// Add to private section:

// ===== Threading =====
HANDLE m_captureThread;
HANDLE m_newFrameEvent;
std::atomic<bool> m_newFrameReady;
std::atomic<bool> m_captureRunning;
std::atomic<bool> m_sessionInvalidated;
std::atomic<int> m_captureGrabCount;          // For capture rate metric

// NvFBC refs for capture thread
NvFBCToDx9Vid* m_nvfbcDx9;
NVFBC_TODX9VID_GRAB_FRAME_PARAMS m_captureGrabParams;  // Thread-local copy

// Static thread proc
static DWORD WINAPI CaptureThreadProc(LPVOID param);

// New method: present-thread-only frame latch
bool LatchCapturedFrame();
```

### Step 2: Split CaptureFrame into grab + latch

The current `CaptureFrame()` does everything in sequence:
1. `NvFBCToDx9VidGrabFrame` → **moves to capture thread**
2. `StretchRect` capture→interop → stays on present thread
3. CUDA interop map/copy/unmap → stays on present thread
4. Ring buffer timestamp + advance → stays on present thread

**New `LatchCapturedFrame()`**: Contains steps 2-4 from the existing `CaptureFrame()`.
Called on the present thread after seeing `newFrameReady`. This is a straightforward
extract — the code already exists, just moved into a new method.

The old `CaptureFrame()` method can be removed or kept for reference.

### Step 3: Capture thread proc

```cpp
DWORD WINAPI FrucCaptureMode::CaptureThreadProc(LPVOID param) {
    FrucCaptureMode* self = (FrucCaptureMode*)param;
    LOG("Capture thread started");

    while (self->m_captureRunning.load()) {
        NVFBCRESULT res = self->m_nvfbcDx9->NvFBCToDx9VidGrabFrame(&self->m_captureGrabParams);

        if (res == NVFBC_SUCCESS) {
            self->m_newFrameReady.store(true);
            self->m_captureGrabCount.fetch_add(1);
            SetEvent(self->m_newFrameEvent);
        }
        else if (res == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated on capture thread");
            self->m_sessionInvalidated.store(true);
            SetEvent(self->m_newFrameEvent);
            break;
        }
        // Non-SUCCESS, non-error: timeout expired, loop again
    }

    LOG("Capture thread exiting");
    return 0;
}
```

### Step 4: Restructured Run() main loop

Key change: **Present moves to top of loop** so VBlank blocking overlaps with capture thread.

```cpp
// Copy grab params for capture thread
m_captureGrabParams = *grabParams;
m_captureGrabParams.dwFlags = NVFBC_TODX9VID_WAIT_WITH_TIMEOUT;
m_captureGrabParams.dwWaitTime = 100;

// Store NvFBC ref for capture thread
m_nvfbcDx9 = nvfbcDx9;

// Launch capture thread
m_captureRunning.store(true);
m_newFrameReady.store(false);
m_sessionInvalidated.store(false);
m_captureGrabCount.store(0);
m_newFrameEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
m_captureThread = CreateThread(NULL, 0, CaptureThreadProc, this, 0, NULL);

bool hasOutputFrame = false;

while (TRUE) {
    // === PRESENT (VSync-driven) ===
    if (hasOutputFrame) {
        PresentFromGPU(device);
        QueryPerformanceCounter(&lastVBlankTime);
        hasVBlankTime = true;
        // ... timing stats for present ...
    }

    // === LATCH ===
    if (m_newFrameReady.load()) {
        m_newFrameReady.store(false);
        // ... timing around latch ...
        if (!LatchCapturedFrame()) {
            LOGERR("Fatal error during frame latch");
            break;
        }
        flowComputedForCurrentPair = false;
    }

    // Check for fatal error from capture thread
    if (m_sessionInvalidated.load()) {
        LOGERR("NvFBC session invalidated");
        break;
    }

    // === FLOW + INTERPOLATION ===
    FrameHistoryEntry* frame0 = GetFrame(1);
    FrameHistoryEntry* frame1 = GetFrame(0);

    if (frame0 && frame1 && frame0->valid && frame1->valid) {
        LONGLONG t0 = frame0->timestamp.QuadPart;
        LONGLONG t1 = frame1->timestamp.QuadPart;

        if (!flowComputedForCurrentPair) {
            ComputeOpticalFlow();
            flowComputedForCurrentPair = true;
        }

        float weight = 0.5f;
        if (hasVBlankTime && t1 > t0) {
            double rawWeight = (double)(lastVBlankTime.QuadPart - t0) / (double)(t1 - t0);
            weight = clamp(rawWeight, 0.0, 1.0);
        }

        InterpolateFrame(weight, GetFrameCudaPtr(1), GetFrameCudaPtr(0));
        hasOutputFrame = true;
    }

    // Window messages
    ProcessMessages();
}

// === SHUTDOWN ===
m_captureRunning.store(false);
if (m_captureThread) {
    WaitForSingleObject(m_captureThread, 5000);
    CloseHandle(m_captureThread);
    m_captureThread = NULL;
}
if (m_newFrameEvent) {
    CloseHandle(m_newFrameEvent);
    m_newFrameEvent = NULL;
}
```

### Step 5: Timing instrumentation

Keep all existing metrics. Add:

- **`capture_rate`**: Grabs/sec on the capture thread. Read `m_captureGrabCount.exchange(0)`
  in the TIMING log, divide by elapsed time. Should match source fps.
- **`latch` timing**: Replace `capture` timing with `latch` timing (StretchRect+CUDA, no
  NvFBC wait). Should be ~0.5-1ms, much shorter than the old capture metric which included
  NvFBC blocking time.

## Files to Modify

1. **`FrucCaptureMode.h`** — Add threading members, `LatchCapturedFrame()`, `CaptureThreadProc`
2. **`FrucCaptureMode.cpp`** — Constructor init, thread proc, `LatchCapturedFrame()`,
   restructured `Run()` loop, shutdown, cleanup

No other files need changes.

## Risks

### NvFBC thread safety (m_captureTarget race)
NvFBC writes to `m_captureTarget` on the capture thread. StretchRect reads it on the present
thread. The signal-then-read protocol should prevent overlap, but at very high source rates
NvFBC could start writing the next frame while StretchRect reads.

**Mitigation (only if corruption observed)**:
```cpp
HANDLE m_captureMutex;

// Capture thread:
WaitForSingleObject(m_captureMutex, INFINITE);
NvFBCToDx9VidGrabFrame(&m_captureGrabParams);
ReleaseMutex(m_captureMutex);
m_newFrameReady.store(true);

// Present thread (in LatchCapturedFrame):
WaitForSingleObject(m_captureMutex, INFINITE);
StretchRect(m_captureTarget → m_interopCaptureSurface);
ReleaseMutex(m_captureMutex);
```

### Capture thread accumulating ahead
If the source runs at 240fps, the capture thread fires ~4x per VBlank. Only the LATEST
capture matters — previous ones are overwritten in `m_captureTarget`. The present thread
latches whatever is there when it checks `newFrameReady`. We always get the freshest frame.

### Weight > 1.0 (source fps < output Hz)
If source drops below 60fps, weight clamps to 1.0 → shows frame1. No extrapolation
artifacts. Same as plain VSync capture.

## Verification

1. **60fps source**: Should match current single-threaded. pair_delta similar, 0 outliers.
2. **90fps source**: pair_delta should drop from ~16.7ms to ~11ms during gameplay.
   Weight should spread (avg ~0.5-0.8). Flow times should decrease. Fewer outliers.
3. **120fps source**: pair_delta ~8.3ms. Flow should be much lower than single-threaded 10-16ms.
4. **capture_rate**: Should match source fps independent of present rate (~60/s).
5. **Visual corruption check**: If seen, add mutex mitigation.
6. **Shutdown**: Close window, should exit cleanly within seconds.

---

## V2: Fully Decoupled Capture/Present Architecture

### Problem with V1

V1's back-pressure mechanism (`m_newFrameReady` flag) serializes capture to present rate.
Capture can only grab once per present-thread iteration. Under GPU load (game running),
present takes 10-15ms → capture_rate locks to 60/s regardless of source framerate. The
multithreading provides no benefit for frame pair diversity.

### V2 Architecture

Capture thread runs the **full pipeline** (grab + StretchRect + CUDA copy to ring buffer)
independently. Present thread **pauses capture briefly** only when it needs to snapshot
the ring buffer for flow computation.

```
Capture Thread (free-running)            Present Thread (VSync-driven)
────────────────────────────             ───────────────────────────
loop:                                    loop:
  if (paused) → idle, spin                 PresentEx(INTERVAL_ONE) ← VBlank block
  cuCtxSetCurrent(ctx)
  NvFBC grab (WAIT_WITH_TIMEOUT)           if (new frames available):
  if SUCCESS:                                Pause capture, wait for idle
    StretchRect target → interop             cuMemcpy ring → staged buffers
    CUDA map interop → ring slot             Cache timestamps
    cuStreamSync(captureStream)              Resume capture
    Record timestamp
    Advance writeIndex                       Copy staged → NvOF inputs
    capturedFrameCount++                     NvOF Execute (flow)
                                             InterpolateFrame(weight, staged)
  (No waiting for present thread.
   Runs at source framerate.)              else (no new frame):
                                             Re-interpolate with new weight
                                             (uses same staged frames)

                                           Copy output → D3D9 → backbuffer
```

### Key Design Changes from V1

**1. Ring buffer size 3 (was 2).**

```cpp
static const int RING_SIZE = 3;
CUdeviceptr m_cudaFrames[RING_SIZE];
FrameHistoryEntry m_frameHistory[RING_SIZE];
std::atomic<int> m_ringWriteIndex;   // capture thread advances
```

Capture writes to `writeIndex`, present reads `writeIndex-1` and `writeIndex-2`.
Third slot ensures capture never overwrites what present is reading.

**2. Capture thread does full D3D9+CUDA pipeline.**

Previously capture only called NvFBC. Now it also does StretchRect and CUDA interop
copy. Requires:
- `D3DCREATE_MULTITHREADED` (already added) for concurrent D3D9 calls
- Separate `m_captureStream` for capture-thread CUDA ops
- `cuCtxSetCurrent(m_cuContext)` at top of capture thread (shared context, separate streams)

**3. Pause/resume replaces back-pressure.**

```cpp
std::atomic<bool> m_capturePaused;  // present requests pause
std::atomic<bool> m_captureIdle;    // capture acknowledges

// Present thread:
m_capturePaused.store(true);
while (!m_captureIdle.load() && m_captureRunning.load()) Sleep(0);
// ... read ring buffer ...
m_capturePaused.store(false);

// Capture thread (top of loop):
if (m_capturePaused.load()) {
    m_captureIdle.store(true);
    while (m_capturePaused.load() && m_captureRunning.load()) Sleep(0);
    m_captureIdle.store(false);
}
```

Pause duration: 2x cuMemcpy (3.7MB each) + sync ≈ 0.03ms. Negligible.
Worst-case wait for idle: one grab period (11ms at 90fps, 100ms on static desktop).

**4. Staged frame buffers for flow/interpolation.**

```cpp
CUdeviceptr m_stagedPrev;  // tight-stride copy of ring[prev]
CUdeviceptr m_stagedCurr;  // tight-stride copy of ring[curr]
```

During pause: ring → staged (fast GPU memcpy, tight stride).
After resume: staged → NvOF inputs (2D copy with stride conversion).
InterpolateFrame reads from staged buffers, not ring buffer.

This fully isolates the ring buffer from flow/interp work. Capture can resume
immediately after the staged copy.

**5. Removed members (from V1):**
- `m_newFrameReady` (replaced by pause/resume)
- `m_newFrameEvent` (not needed)
- `m_captureTimestamp` (timestamp set directly in capture thread)
- `LatchCapturedFrame()` (logic moved to capture thread + staging)
- `m_cudaFrame0, m_cudaFrame1` (replaced by `m_cudaFrames[RING_SIZE]`)

### Instrumentation

TIMING log adds:
- **`stage`**: total time for pause + ring→staged copy + resume (replaces `latch`)
- **`pause_wait`**: how long present waited for capture to go idle (subset of stage)

Diagnostic logging for first N operations:
- Capture thread: log slot index, timestamp for first 5 frames
- Stage: log ring writeIndex, slot indices, timestamps for first 5 stages

### Actual Results (tested with Avatar Frontiers of Pandora @ 90fps)

**What worked:**
- capture_rate=80-86/s (close to 90fps, some D3D9 lock contention)
- pause_wait typically <2ms
- flow=0.4-0.8ms, interp=0.01ms — fast
- Ring buffer cycling correctly

**What didn't work: weight=1.000 always.**

Both staged frames are captured DURING the PresentEx block (while it blocks for VBlank).
Both timestamps are before `lastVBlankTime`:

```
prevVBlank ... capture_t0 ... capture_t1 ... lastVBlank
                                               ↑ target
weight = (lastVBlank - t0) / (t1 - t0) ≥ 1.0 → clamped to 1.0
```

No actual interpolation happens. Just frame selection (always showing latest).

**Additional issue: D3D9 lock contention.**
PresentEx appears to hold the D3DCREATE_MULTITHREADED critical section for the
entire VSync block (~14ms), starving the capture thread's StretchRect. Shows as
capture_rate=80/s instead of expected 90/s, and present=17-22ms (should be ~16.7ms).

### Weight Problem Analysis

For interpolation to work, the target time must fall BETWEEN t0 and t1.
With the current "stage two newest frames" approach, both are always before
the current VBlank → weight ≥ 1.0.

**The "1 frame behind" approach was correct.** Targeting `prevVBlankTime`
places the target between older captures:

```
C0(T-25ms) < prevVBlank(T-16.7ms) < C1(T-14ms) < C2(T-3ms) < currVBlank(T)
                   ↑ target here
     ↑ use this pair ↑
weight = (16.7 - 25 + T) / (14 - 25 + T) = 8.3/11 = 0.75 ✓
```

But this requires:
1. Ring size 4 (3 completed frames + 1 write slot) to have enough history
2. Smart pair selection: find the two frames that bracket prevVBlankTime
3. Accepting ~16.7ms additional display latency

### Open Design Questions (resolved in V3)

See `samples/NvFBC/NvFBCR/.claude/handoff.md` for full analysis of options:
- **Option A**: Target prevVBlankTime, ring size 4, smart pair selection ← **CHOSEN (V3)**
- **Option B**: Don't advance "prev" frame until after present, only advance "next"
- **Option C**: Rethink staging model entirely (triple buffer, capture-selected pairs, etc.)

Option B was rejected: all captures happen during PresentEx VSync block, so all timestamps
are before the current VBlank. There's no way to get a capture "after" the VBlank without
adding a post-VBlank wait that would halve output framerate.

---

## V3: Bracket Pair Selection + No Staged Buffers

### Problem with V2

V2 successfully decoupled capture from present (capture_rate=80-86/s at 90fps source),
but weight was always 1.0. Root cause: the staging step copies the two most recent ring
frames — both captured DURING the PresentEx block — so both timestamps are before
lastVBlankTime. Weight = (lastVBlank - t0) / (t1 - t0) >= 1.0. Clamped to 1.0. No
actual interpolation occurs.

### V3 Solution: Target prevVBlankTime

Instead of targeting lastVBlankTime (current VBlank), target prevVBlankTime (the VBlank
before that). At any given VBlank, captures exist on both sides of the PREVIOUS VBlank:

```
C0(T-25ms) < prevVBlank(T-16.7ms) < C1(T-14ms) < C2(T-3ms) < currVBlank(T)
                  ↑ target                                      ↑ PresentEx returns
  ↑ bracket pair: C0 ──────────────── C1 ↑
  weight = (prevVBlank - C0) / (C1 - C0) = 8.3/11 = 0.75 ✓
```

This adds ~16.7ms display latency (1 output frame behind). This is the standard cost
for frame interpolation — same approach used by DLSS/FSR frame generation.

### Key Design Changes from V2

**1. Ring buffer size 6 (was 3).**

5 completed slots + 1 write slot. At 240fps worst case: 5 × 4.2ms = 20.8ms of history,
comfortably spanning the 16.7ms VBlank interval.

**2. Staged buffers eliminated.**

V2 copied ring → staged → NvOF inputs (4 copies per pair). V3 reads ring slots directly:
- ring[slot] → NvOF inputs (stride conversion, 2 copies — the unavoidable minimum)
- InterpolateFrame reads ring slots directly (tight stride, no copy)

Memory: ring size 6 - 2 staged = net +1 frame allocation vs V2 (~3.7MB at 1280x720).

**3. Bracket pair selection replaces "two newest" staging.**

After pausing capture (metadata read only, microseconds), walk completed ring slots
from newest to oldest. Find first slot with timestamp <= prevVBlankTime → "before" frame.
The next-newer slot → "after" frame. Fallback to two newest during startup or when
source rate <= output rate.

```cpp
LONGLONG target = prevVBlankTime.QuadPart;
for (int i = 1; i <= completedCount; i++) {
    int slot = (wi + RING_SIZE - i) % RING_SIZE;
    if (m_frameHistory[slot].timestamp.QuadPart <= target) {
        bracketPrevSlot = slot;
        if (i > 1) bracketNextSlot = (wi + RING_SIZE - (i-1)) % RING_SIZE;
        break;
    }
}
```

**4. Weight uses prevVBlankTime.**

```cpp
weight = (prevVBlankTime - cachedT0) / (cachedT1 - cachedT0)
```

Naturally falls in (0, 1) when bracket pair straddles prevVBlankTime.

**5. Same-pair detection skips redundant flow.**

If the bracket search finds the same pair as last iteration (same timestamps), the
flow computation is skipped. Only the weight changes (different prevVBlankTime each
VBlank). This handles source <= output gracefully: same pair re-interpolated at
different weights across consecutive VBlanks.

**6. Pause is metadata-only (microseconds).**

```
Pause capture → read writeIndex + timestamps → resume immediately
↓ (capture running again)
Copy ring[slot] → NvOF inputs (stride conversion, ~0.03ms)
NvOF Execute (~0.5ms)
InterpolateFrame reads ring[slot] directly (~0.01ms)
```

Ring slots are safe to read after resume: capture must write 4+ slots to overwrite the
bracket pair. At 240fps: 4 × 4.2ms = 16.7ms >> 1ms flow+interp. 16x safety margin.

### V3 Architecture

```
Capture Thread (free-running)            Present Thread (VSync-driven)
────────────────────────────             ───────────────────────────
loop:                                    loop:
  if (paused) → idle, spin                 PresentEx(INTERVAL_ONE) ← VBlank block
  cuCtxSetCurrent(ctx)                     prevVBlankTime = lastVBlankTime
  NvFBC grab (WAIT_WITH_TIMEOUT)           lastVBlankTime = QPC()
  if SUCCESS:
    StretchRect target → interop           if (new frames available):
    CUDA map interop → ring slot             Pause capture (metadata only)
    cuStreamSync(captureStream)              Read writeIndex + timestamps
    Record timestamp                         Resume capture immediately
    Advance writeIndex
    capturedFrameCount++                     Walk ring: find bracket pair
                                             straddling prevVBlankTime
  (No waiting for present thread.
   Runs at source framerate.)              if (new pair):
                                             ring → NvOF inputs (stride conv)
                                             NvOF Execute (flow)

                                           weight = (prevVBlank-t0)/(t1-t0)
                                           InterpolateFrame(weight,
                                             ring[prevSlot], ring[nextSlot])
                                           Copy output → D3D9 → backbuffer
```

### Shared State

```cpp
// Ring buffer (capture writes, present reads after pause)
CUdeviceptr m_cudaFrames[6];           // CUDA ring slots (GPU-resident)
FrameHistoryEntry m_frameHistory[6];   // Timestamps per slot
std::atomic<int> m_ringWriteIndex;     // Next slot to write

// Synchronization
std::atomic<bool> m_capturePaused;     // Present requests pause
std::atomic<bool> m_captureIdle;       // Capture acknowledges
std::atomic<bool> m_captureRunning;    // Shutdown signal
std::atomic<bool> m_sessionInvalidated; // Fatal error
std::atomic<int> m_capturedFrameCount; // Total captures (for new-frame detection)
std::atomic<int> m_captureGrabCount;   // For capture rate metric
```

### Instrumentation

TIMING log fields:
- **`select`**: total time for pause + metadata read + pair selection (replaces `stage`)
- **`pause_wait`**: how long present waited for capture to go idle
- **`bracket_hit`** / **`fallback`**: how many selections found a bracket vs fell back to newest

### Source Rate Behavior

| Source fps | Behavior |
|-----------|---------|
| 24-30     | Bracket found most VBlanks. Weight alternates ~0.5 / ~1.0 (correct pulldown) |
| 60        | ~1 capture per VBlank interval. Bracket found ~50%. Weight ~0.5 on hit, 1.0 on fallback |
| 90        | ~1.5 captures per interval. Bracket found >90%. Weight ~0.75 avg |
| 120+      | Multiple captures per interval. Bracket always found. Weight spreads 0.3-0.8 |
| ≤ output  | Fallback to two newest. Weight ≥ 1.0. Just frame selection (correct) |

### V3 Test Results

**Desktop (~240fps source):**
- weight avg=0.55-0.79, full spread min=0.012 max=0.995
- bracket_hit=300/300, fallback=0 in stable periods
- capture_rate=239-305/s
- flow=0.56-0.78ms, interp=0.01ms, 0 stalls

**Avatar benchmark (~90fps source):**
- weight avg=0.13-0.44 — bracket working, but biased low
- bracket_hit=288-300, fallback=0-5
- capture_rate=39-86/s (should be ~90, limited by D3D9 lock contention)
- present=19.7-26.7ms (should be ~16.7ms)
- 271-299 stalls per 300 presents (~43fps output instead of 60fps)
- pause_wait=1-12ms (waiting for capture thread to finish StretchRect)

**Root cause of gameplay performance issues:** `D3DCREATE_MULTITHREADED` global critical
section. PresentEx holds the D3D9 lock during VSync wait. The capture thread's StretchRect
and CUDA interop map/unmap also need this lock. Under GPU load, cuStreamSynchronize in
PresentFromGPU blocks longer (GPU busy with game), extending the lock hold time. Total
loop time exceeds 16.7ms → missed VBlanks → ~43fps output.

Weight calculation is correct (V3 fix works). Performance under GPU load requires
eliminating D3D9 from the capture thread → V4.

---

## V4: NvFBCCuda Mode — Eliminate D3D9 from Capture (IMPLEMENTED, UNTESTED)

### Problem with V3

V3 fixed the weight=1.0 issue (bracket pair selection works correctly). But under GPU
load, D3D9 lock contention between the capture thread (StretchRect + CUDA interop) and
the present thread (PresentFromGPU + PresentEx) causes:
- capture_rate drops to 39-86/s instead of 90/s
- present time inflates to 20-27ms instead of 16.7ms
- ~43fps output instead of 60fps
- pause_wait of 1-12ms (waiting for capture to finish D3D9 work)

The root cause is `D3DCREATE_MULTITHREADED`, which adds a global critical section around
ALL D3D9 API calls. Both threads need D3D9, so they serialize.

### Solution: NvFBCCuda

The NvFBC SDK has a CUDA output mode (`NVFBC_SHARED_CUDA` = 0x1007, defined in
`nvFBCCuda.h`). Instead of capturing to a D3D9 surface, NvFBC writes directly to a
caller-provided `CUdeviceptr`.

```c
// nvFBCCuda.h
typedef struct {
    NvU32 dwVersion;
    NvU32 dwFlags;                    // NVFBC_TOCUDA_WAIT_WITH_TIMEOUT etc.
    void *pCUDADeviceBuffer;          // [in]: Output buffer — caller provides CUdeviceptr
    NvFBCFrameGrabInfo *pNvFBCFrameGrabInfo;  // [in/out]: Grab info (includes dwBufferWidth)
    NvU32 dwWaitTime;                 // [in]: Timeout in ms
} NVFBC_CUDA_GRAB_FRAME_PARAMS;
```

This eliminates ALL D3D9 from the capture thread. Only the present thread uses D3D9
(output StretchRect + PresentEx). `D3DCREATE_MULTITHREADED` can be removed.

### Creation Requirements

From `nvFBC.h` (`NvFBCCreateParams`):
```c
void* cudaCtx;  // CUDA context created using cuD3D9CtxCreate with the D3D9 device
                 // passed as pDevice. Only used for NvFBCCuda interface.
                 // It is mandatory to pass a valid D3D9 device if cudaCtx is passed.
```

We already create our CUDA context with `cuD3D9CtxCreate(ctx, device, 0, d3dDevice)`.
The D3D9 device is needed for NvFBC creation but NOT for runtime capture. After setup,
all grabs go directly to CUDA memory.

### V4 Architecture

```
Capture Thread (free-running)            Present Thread (VSync-driven)
────────────────────────────             ───────────────────────────
loop:                                    loop:
  cuCtxSetCurrent(ctx)                     PresentEx(INTERVAL_ONE) ← VBlank block
  NvFBCCudaGrabFrame(ring[wi])             prevVBlankTime = lastVBlankTime
  if SUCCESS:                              lastVBlankTime = QPC()
    Record timestamp
    Advance writeIndex                     Read writeIndex + timestamps (atomic)
    capturedFrameCount++                   Walk ring: find bracket pair
                                           straddling prevVBlankTime
  (Pure CUDA. No D3D9. No interop.
   No pause needed. No lock contention.    if (new pair):
   Runs at true source framerate.)           ring → NvOF inputs (stride conv)
                                             NvOF Execute (flow)

                                           weight = (prevVBlank-t0)/(t1-t0)
                                           InterpolateFrame(weight,
                                             ring[prevSlot], ring[nextSlot])
                                           CUDA output → D3D9 → backbuffer
```

### Key Design Changes from V3

**1. NvFBCCuda replaces NvFBCToDx9Vid.**

Creation: `pNVFBCLib->create(NVFBC_SHARED_CUDA, ...)` with `cudaCtx` set in
NvFBCCreateParams. Setup: `NvFBCCudaSetup()` with ARGB format. Grab:
`NvFBCCudaGrabFrame()` with ring buffer slot as `pCUDADeviceBuffer`.

**2. Capture thread does NvFBC grab only — no D3D9, no CUDA interop.**

The entire capture thread proc becomes:
```cpp
while (m_captureRunning.load()) {
    grabParams.pCUDADeviceBuffer = (void*)m_cudaFrames[writeSlot];
    NVFBCRESULT res = nvfbcCuda->NvFBCCudaGrabFrame(&grabParams);
    if (res == NVFBC_SUCCESS) {
        QueryPerformanceCounter(&timestamp);
        m_frameHistory[writeSlot] = { timestamp, true };
        m_ringWriteIndex.store((writeSlot + 1) % RING_SIZE);
        m_capturedFrameCount.fetch_add(1);
        m_captureGrabCount.fetch_add(1);
    }
}
```

**3. Pause mechanism removed.**

No D3D9 calls on capture thread → no lock contention → no need to pause. The present
thread reads ring metadata via atomics (writeIndex is seq_cst, timestamps are written
before writeIndex advances). Ring size 6 provides timing safety for direct slot reads
during flow/interp (same analysis as V3).

**4. D3DCREATE_MULTITHREADED removed.**

Only the present thread calls D3D9 (StretchRect to backbuffer, PresentEx). No concurrent
D3D9 usage → no global critical section needed. PresentEx no longer blocks the capture
thread.

**5. Eliminated members:**
- `m_interopCaptureSurface` (no D3D9 interop for capture)
- `m_cudaCaptureResource` (no CUDA-D3D9 registration for capture)
- `m_captureStream` (no separate CUDA stream for capture)
- `m_capturePaused`, `m_captureIdle` (no pause mechanism)
- `m_captureTarget` (no D3D9 capture surface)

**6. NvFBC stride padding.**

`NvFBCFrameGrabInfo.dwBufferWidth` may differ from `dwWidth` (padded rows). Options:
a. If dwBufferWidth == dwWidth: direct grab to ring slot (zero-copy)
b. If dwBufferWidth != dwWidth: grab to temp buffer, cuMemcpy2D to tight-stride ring slot
c. Or: let ring slots use padded stride, handle stride in NvOF copy + interp kernel

Option (a) is ideal. Option (b) adds one GPU copy per grab (still faster than V3's
StretchRect + interop chain). Option (c) avoids the copy but complicates downstream.
Determine at runtime after first grab.

### Expected Performance Improvement

| Metric | V3 (gameplay) | V4 (expected) |
|--------|--------------|---------------|
| capture_rate | 39-86/s | ~90/s (true source rate) |
| present | 20-27ms | ~16.7ms (no lock contention) |
| pause_wait | 1-12ms | N/A (no pause) |
| output fps | ~43fps | ~60fps |
| stalls | 271-299/300 | ~0 |

### Files Modified (Implementation)

1. **`IFrameCaptureMode.h`** — Added `virtual bool ManagesOwnCapture() const { return false; }`
2. **`NvFBCR.cpp`** — Removed `D3DCREATE_MULTITHREADED`. Added `ManagesOwnCapture()` branch:
   creates canary NvFBCToDx9Vid to verify NvFBC is enabled, releases it, then calls
   `Setup()` + `Run(nullptr, nullptr, device, hwnd)` for self-managing modes.
3. **`FrucCaptureMode.h`** — `#include <NvFBC/nvFBCCuda.h>`, `extern NvFBCLibrary* pNVFBCLib`,
   `ManagesOwnCapture() override { return true }`. Replaced V3 D3D9/interop/pause members
   with: `NvFBCCuda* m_nvfbcCuda`, `NVFBC_CUDA_GRAB_FRAME_PARAMS`, `NvFBCFrameGrabInfo`,
   stride detection members (`m_strideChecked`, `m_needsStrideCopy`, `m_grabTempBuffer`).
   Added `CreateNvFBCCuda()` method. Removed `m_captureStream`.
4. **`FrucCaptureMode.cpp`** — Full V4 rewrite:
   - `Setup()`: calls `InitCuda()` + `CreateNvFBCCuda()` (creates instance via `createEx`)
   - `CreateNvFBCCuda()`: builds `NvFBCCreateParams` with `NVFBC_SHARED_CUDA` + `cudaCtx`,
     calls `NvFBCCudaSetup(ARGB)`.
   - `CaptureThreadProc()`: trivial — `NvFBCCudaGrabFrame(ring[writeSlot])`, stride check
     on first grab (temp buffer + cuMemcpy2D if padded), timestamp + advance. No D3D9.
   - `Run()`: no pause mechanism, atomics-only bracket selection. Same bracket/weight/flow/interp.
   - `Cleanup()`: `NvFBCCudaRelease()`, no interop cleanup, no pause reset.
5. **`NvFBCLibrary.h`** — No changes needed (used existing `createEx()` method).

### Implementation Notes

- NvFBCR.cpp creates a canary NvFBCToDx9Vid to test if NvFBC is enabled (reuses existing
  enable/retry logic). For FRUC mode, the canary is released and FrucCaptureMode creates
  its own NvFBCCuda in Setup(). This avoids having two NvFBC sessions active simultaneously.
- Stride padding: runtime detection on first grab. If `dwBufferWidth != dwWidth`, allocates
  temp buffer and does `cuMemcpy2D` per grab. If equal, zero-copy directly to ring slot.
- TIMING log: removed `pause_wait` metric (no pause). `select` metric remains.

### Open Questions (Answer During Testing)

1. Does `dwBufferWidth` match `dwWidth` on RTX 5080 at 1280x720?
2. Does NvFBCCuda require `cuCtxSetCurrent` on the capture thread? (Code does it.)
3. Does NvFBCCuda grab block the CUDA context (preventing present-thread CUDA work)?
4. Any visual corruption from lock-free ring reads?

### V4 Desktop Test Results (ANSWERED)

1. **dwBufferWidth == dwWidth** on RTX 5080 at 2560x1440 (native capture res). Zero-copy confirmed.
2. Yes, `cuCtxSetCurrent` called on capture thread — works.
3. NvFBCCuda **internally uses D3D9** (discovered via crashes). `D3DCREATE_MULTITHREADED` is
   required. But lock contention is minimal with 1ms grab timeout.
4. No visual corruption observed with lock-free ring reads.

See handoff doc (`samples/NvFBC/NvFBCR/.claude/handoff.md`) for full V4 desktop test data:
0 stalls, work=2-3ms, vsync=2-3ms, flow=1-1.8ms, capture_rate=53-89/s.

### V4 Known Issues (Desktop)

1. **Dedup heuristic too aggressive at high source rates.** At 240Hz desktop, capture_rate
   is 53-89/s instead of ~240/s. Grab itself takes 300-800µs (14.7MB copy), close to the
   500µs dedup threshold. Self-resolved at 90fps gameplay (11ms >> 500µs threshold).

2. **pair_delta outliers (300-900ms max on desktop).** Occasional large gaps between unique
   captures. Also present under gameplay (max pair_delta up to 452ms).

### V4 Gameplay Test Results (Avatar Frontiers of Pandora @ 90fps)

**Hardware:** RTX 5080, 2560x1440 @ 240Hz source → 60Hz output.
**Capture resolution:** 2560x1440 native (NvFBCCuda, no SOURCEMODE_SCALE).

| Phase | flow (ms) | work (ms) | vsync (ms) | stalls/300 | capture_rate | bracket_hit |
|-------|-----------|-----------|------------|------------|-------------|-------------|
| Desktop (startup) | 2-5 | 2-5 | 0.5-2.7 | 0 | 42-85/s | ~150 |
| **Heavy gameplay** | **8-18** | **8-17** | 3-6 | **35-235** | **17-31/s** | 92-187 |
| Desktop (shutdown) | 3-4 | 4-5 | 0.8-0.9 | 0 | 78-80/s | 151-159 |

**Total: 1578 stalls across ~11,500 presents. 5,832 captured frames. 90,138 duplicates skipped.**

#### Stall Pattern Analysis

Two alternating stall patterns visible in the per-stall breakdown:

- **Pattern A — NvOF bottleneck:** `flow=15-18ms, work=5-7ms`
  Flow alone exceeds the 16.7ms frame budget.
- **Pattern B — GPU work bottleneck:** `flow=0-7ms, work=14-20ms`
  cuGraphicsMap + cuMemcpy2D + cuStreamSync + StretchRect consumes the budget.

These alternate frame-to-frame, indicating GPU time-slicing between NvOF, the present
pipeline, and the game. The GPU simply cannot service all three workloads within 16.7ms.

#### Root Cause: 2560x1440 Pipeline Under GPU Contention

The total per-frame GPU work (flow + work) ranges 16-35ms under game load — exceeding
the 16.7ms VBlank budget. This is a fundamental resource problem, not a synchronization
or architecture issue.

Breakdown of GPU costs at 2560x1440 under game load:
- NvOF Execute: 8-18ms (vs 1-2ms on desktop) — GPU-starved
- Ring → NvOF input copy: 2x cuMemcpy2D of 14.7MB with stride conversion
- cuGraphicsMapResources + cuMemcpy2D output + cuStreamSync: 5-17ms
- StretchRect (2560x1440 → 1280x720): included in work
- Game rendering: competing for the same GPU

#### V3 vs V4 Comparison Under Gameplay

| Metric | V3 (gameplay) | V4 (gameplay) |
|--------|--------------|--------------|
| stalls/300 | 271-299 | 35-235 |
| Bottleneck | D3D9 lock contention | GPU contention |
| capture_rate | 39-86/s | 17-31/s |
| output fps | ~43fps | ~50fps (variable) |

V4 eliminated D3D9 lock contention as the primary bottleneck. The remaining problem is
raw GPU budget: flow + work exceeds 16.7ms when the GPU is loaded by a game.

---

## V5 Planning: Fitting in the 60Hz Budget Under GPU Load

### Hard Requirement

**60Hz locked output.** No dropped frames. flow + interp + work must fit in 16.7ms
even while a game is running.

### The Budget Problem

At 2560x1440 under game load:
- flow = 8-18ms
- work = 8-17ms
- flow + work = 16-35ms → **exceeds 16.7ms budget by up to 2x**

Two independent strategies to solve this:
**A) Reduce GPU work** (make each operation cheaper)
**B) Overlap GPU work** (pipeline operations so they run concurrently)

These can be combined.

### Strategy A: Reduce GPU Work — Downscale After Capture

NvFBCCuda captures at source native resolution (2560x1440, no SOURCEMODE_SCALE).
All downstream GPU work scales with pixel count. Downscaling early reduces everything.

**Approach:** After NvFBCCuda grabs to ring slot at 2560x1440, a CUDA downscale kernel
produces a 1280x720 (or other target res) version. All subsequent operations — NvOF,
interpolation, output copy — work at the reduced resolution.

**Impact estimate:**
- NvOF input: 14.7MB → 3.7MB (4x reduction at 720p)
- Ring → NvOF copy: 4x faster
- Interpolation: 4x faster (already negligible)
- Output cuMemcpy2D: 4x faster
- StretchRect: trivial (720p → 720p, no scaling)
- Downscale kernel cost: ~0.1-0.5ms (simple bilinear, memory-bound)

**NvOF caveat:** We tested 640x360 vs 1280x720 on desktop and saw only 4% improvement
(8-9ms fixed cost). However, under GPU load the memory bandwidth component matters more
because the GPU's memory bus is shared with the game. 4x less data through the bus could
meaningfully reduce NvOF wall time under contention. Needs testing.

**Implementation:**
- Capture thread grabs to full-res ring slots (unchanged)
- Add downscaled ring buffer (RING_SIZE × 1280×720×4 = 22MB vs current 88MB)
- Capture thread runs downscale kernel after each grab (or present thread does it)
- NvOF, interpolation, output all use downscaled buffers
- Full-res ring slots could be eliminated (only keep downscaled), saving 66MB GPU memory

**Alternative: NvFBCToSys with SOURCEMODE_SCALE** — captures directly at target res but
routes through system memory. The GPU→CPU→GPU roundtrip likely negates the scaling benefit.
Not recommended.

### Strategy B: Overlap GPU Work — Pipeline Flow + Present

The current present loop is sequential:

```
Current V4 (sequential):
  VBlank → Select → Flow(~10ms) → Interp(~0.1ms) → Work(~10ms) → PresentEx → VBlank
                     |<------------ 20ms+ total, exceeds 16.7ms ----------->|
```

Flow and Work are independent GPU operations that could run on separate CUDA streams
concurrently. The key insight: we can compute flow for the NEXT frame while presenting
the CURRENT frame.

```
Pipelined V5:
  Frame N:
    Start Flow(N+1) on stream A (async)       ← for NEXT present
    Work(N) on stream B (uses previous flow)   ← for CURRENT present
    PresentEx(INTERVAL_ONE) → VBlank
    Wait for Flow(N+1) completion
    Interp(N+1) → output buffer ready for next iteration

  Timeline:
    |---Flow(N+1)----->|
    |---Work(N)--------|--PresentEx--VBlank--|
                                             |--Interp(N+1)--|

  Budget: max(flow, work) + interp ≈ 10ms + 0.1ms = 10.1ms ✓
  (vs sequential: flow + work ≈ 20ms ✗)
```

**Requirements:**
- Two CUDA streams (flow stream + present/work stream)
- Double-buffered output (interpolated frame N and N+1)
- Predictive bracket selection (must know the target for N+1 before computing flow)
- RTX 5080 must actually schedule both streams concurrently (likely yes, modern GPUs
  support concurrent kernel execution on separate streams)

**Risk:** Under heavy GPU load, the GPU scheduler may serialize the streams anyway,
giving no benefit. But even partial overlap helps.

### Strategy C: Predictive Timing — Compute Before VBlank

All V1-V4 versions react to VBlank: `PresentEx` blocks, then we compute. This wastes
the VSync wait time (3-6ms under load, up to 12ms on desktop). With prediction, we
compute BEFORE the VBlank block:

```
Predictive:
  1. Predict next VBlank: targetVBlank = lastVBlankTime + measuredPeriod
  2. Target interpolation time: targetVBlank - measuredPeriod (one frame behind)
  3. Select bracket pair, compute flow + interp
  4. PresentEx(INTERVAL_ONE) → hardware sync
  5. Update measuredPeriod from actual VBlank time (EMA filter)
```

This transforms the loop from **wait → compute** to **compute → wait**, reclaiming
the VSync wait time as computation headroom.

**Combined with pipelining:** Start flow for frame N+1, do work+present for frame N,
then interp N+1 during/after VSync. Maximizes GPU utilization across the full 16.7ms.

#### Why "One Frame Behind" Is Fundamental

Interpolation requires a "future" frame — you need source frames on BOTH sides of the
output time. You can't interpolate until the second frame is captured. This means the
output must always represent a time in the past relative to the latest capture.

The "one frame behind" approach (`targetTime = nextVBlank - vblankPeriod`) is the same
strategy used by DLSS/FSR frame generation. The cost is ~16.7ms of display latency.

#### VBlank Prediction Accuracy

VBlank is a hardware signal from the display's crystal oscillator. A software prediction
uses the CPU's crystal (QPC). Crystal oscillators have ~20-100 ppm tolerance:
- Worst case drift: ~0.012Hz at 60Hz → 1 frame drift per ~83 seconds
- Typical: 1 frame drift per 5-10 minutes

**Mitigation:** Don't free-run. Use actual VBlank measurements to continuously correct:
```cpp
double actualPeriod = (actualVBlank - prevActualVBlank) / perfFreq;
measuredVBlankPeriod = 0.95 * measuredVBlankPeriod + 0.05 * actualPeriod;  // EMA
```

This locks prediction to hardware VBlank without drift. Prediction error is bounded to
one VBlank period's jitter (~100µs), which is fine for bracket selection.

### NvFBC Timing Utilities

#### NvFBCCudaGPUBasedCPUSleep

```c
/// A high precision implementation of Sleep().
/// Can provide sub quantum (usually 16ms) sleep that does not burn CPU cycles.
/// @param qwMicroSeconds The number of microseconds that the thread should sleep for.
virtual NVFBCRESULT NvFBCCudaGPUBasedCPUSleep(__int64 qwMicroSeconds) = 0;
```

**Properties:**
- Microsecond resolution (vs. Windows Sleep() at ~15.6ms granularity)
- Does not busy-wait / burn CPU cycles (unlike QPC spin loops)
- Uses GPU timing hardware (likely GPU interrupt/timer)
- Available on `m_nvfbcCuda` object directly

**Unknown:** Behavior under GPU load needs testing.

**Potential uses:**
1. **Capture thread pacing**: Sleep until next expected source frame instead of
   tight-looping with 1ms timeout grabs. Reduces D3D9 lock acquisitions.
2. **Present thread timing**: With predictive model, sleep precisely until target
   computation start time (reclaims VSync wait time for useful work scheduling).
3. **Replace dedup heuristic**: Use `NVFBC_TOCUDA_NOFLAGS` (blocks until new frame)
   for guaranteed new-frame detection. No timing heuristic needed.

#### NVFBC_TOCUDA_CPU_SYNC Flag

```c
NVFBC_TOCUDA_CPU_SYNC = 0x2  // Does a cpu event signal when grab is complete
```

Signals a CPU event on grab completion. Could enable efficient capture thread blocking
via `WaitForSingleObject` instead of polling. Event mechanism undocumented.

### V5 Implementation Options

All options assume 60Hz locked output as a hard requirement.

**Option 1: Downscale only (simplest)**
- Add CUDA downscale kernel after capture (2560x1440 → 1280x720)
- All downstream work at 720p
- Keeps V4 architecture otherwise (reactive VBlank, sequential flow+work)
- Expected improvement: work drops ~4x. Flow may or may not improve (8ms fixed cost).
- If flow stays 8ms + work drops to 3ms = 11ms total → **fits in 16.7ms budget**
- Risk: NvOF fixed cost doesn't decrease → still 8ms + 3ms + vsync = might be tight

**Option 2: Downscale + predictive timing**
- Option 1 + predict next VBlank → compute before VSync block
- Reclaims VSync wait time (~3-6ms) as headroom
- Eliminates VBlank state tracking complexity (prevVBlankTime etc.)
- Capture thread switches to `NOFLAGS` (no dedup heuristic)
- Moderate complexity increase over Option 1

**Option 3: Downscale + pipelined flow/present**
- Option 1 + overlap flow(N+1) with work+present(N) on separate streams
- Budget becomes max(flow, work) instead of flow + work
- If flow=8ms and work=3ms after downscale → max = 8ms → fits easily
- Requires: double-buffered output, predictive bracket selection, 2 CUDA streams
- Highest complexity but best GPU utilization

**Option 4: Pipelining without downscale**
- Overlap flow(N+1) with work+present(N) at full 2560x1440
- Budget: max(flow=10ms, work=10ms) ≈ 10ms → might fit
- No downscale kernel needed, preserves full resolution through pipeline
- Risk: under heavy GPU load, streams may not overlap well → falls back to ~20ms
- Higher resolution = higher quality optical flow vectors

**Recommendation:** Start with **Option 1** (downscale only). It's the smallest change
and may be sufficient on its own. If NvOF's 8ms fixed cost still causes issues, add
predictive timing (Option 2) to reclaim VSync headroom. Pipelining (Option 3/4) is the
nuclear option if the simpler approaches fail.

## V5: Downscale After Capture (IMPLEMENTED, TESTED)

### Implementation
V5 = Option 1 from planning above. Added CUDA downscale kernel in capture thread:
- NvFBCCuda grab → `m_fullResGrabBuffer` (2560×1440)
- `launchDownscaleKernel` → ring slot (1920×1080, target display resolution)
- All downstream work (NvOF, interp, present) at 1920×1080

### V5 Gameplay Results (Avatar @ 90fps)

| Phase | flow (ms) | work (ms) | Stalls/300 |
|-------|-----------|-----------|------------|
| Desktop | 0.3-0.5 | 1.4-1.9 | 0 |
| Medium gameplay | 4.7-5.5 | 4.3-5.7 | 0 |
| Heavy gameplay | 7.8-10.1 | 8.4-11.7 | 12-108 |

**Total: ~424 stalls** (73% reduction from V4's 1578).

### Analysis
NvOF is still 8-10ms under heavy GPU load. flow+work = 16-22ms exceeds 16.7ms budget.
Downscale helped but wasn't sufficient alone. Need Option 3 (pipelining).

## V6: Pipelined Flow + Present (IMPLEMENTED, TESTED)

### Implementation
V6 = Option 1 + Option 3. Dedicated flow worker thread:
- Flow thread: waits for signal → copies ring→NvOF inputs → NvOF Execute → signals done
- Present thread: waits for flow done → interp → present → dispatches next flow
- Double-buffered NvOF outputs prevent read/write conflict
- Windows auto-reset events for synchronization
- Budget = max(flow, present) instead of flow + present

### V6 Gameplay Results (Avatar @ 90fps)

| Phase | flow (ms) | fwait (ms) | work (ms) | Stalls/300 |
|-------|-----------|------------|-----------|------------|
| Desktop | 4.1-4.8 | 0.03-0.10 | 1.7-1.8 | 0 |
| Medium gameplay | 13.0-14.3 | 0.06-0.20 | 3.2-7.6 | 0-2 |
| Heavy gameplay | 14.3-16.0 | 0.10-0.17 | 7.6-13.8 | 10-74 |

**Total: 311 stalls** (27% reduction from V5's 424).

### Analysis
Pipelining works — fwait≈0 in most stalls (flow finishes before present needs it).
But `work` alone reaches 20-52ms during heavy GPU contention. The problem shifted from
flow+work budget to work-only GPU contention. Need to reduce work volume.

## V7: Direct-to-Backbuffer Resolution (IMPLEMENTED, TESTED — CURRENT)

### Implementation
Two changes combined:
1. **Working resolution → backbuffer (1280×720)** instead of target display (1920×1080)
   - Ring buffers, NvOF inputs, interp, memcpy all 2.25× smaller
   - NvOF cost unchanged (8-9ms fixed regardless of resolution)
2. **Backbuffer-native output** — interop surface at 1280×720, StretchRect is 1:1 copy
   - Attempted to register backbuffer directly for CUDA interop → CUDA_ERROR_INVALID_VALUE
   - D3D9 swap chain surfaces can't be CUDA-registered
   - Kept intermediate surface but at backbuffer resolution (1:1 copy instead of scaling)

### V7 Gameplay Results (Avatar @ 90fps)

| Phase | flow (ms) | work (ms) | Stalls/300 |
|-------|-----------|-----------|------------|
| Desktop | 3.9-4.4 | 1.1-1.3 | 0 |
| Medium gameplay | 12.7-13.5 | 4.5-6.0 | 0 |
| Heavy gameplay | 13.3-16.1 | 3.7-7.2 | 0-6 |

**Total: 22 stalls / 12,900 presents (0.17%).**
**98.6% reduction from V4 baseline (1578 → 22).**

### Remaining Stall Analysis
All 22 stalls are isolated GPU contention spikes (work=15-30ms) during peak game load.
The actual data copies total ~0.3ms — stalls are from cuGraphicsMapResources and
cuStreamSynchronize waiting for GPU availability behind the game's rendering.

## Future Optimization Notes

Pipeline is near-optimal for single-GPU. Remaining 22 stalls are fundamental GPU
scheduling contention. Marginal optimizations (~0.3ms total):

1. **NvOF RegisterPreAllocBuffers** — register ring slots as NvOF inputs, skip
   ring→NvOF memcpy (~0.1ms/flow). Complex: 6 rotating slots.
2. **CUDA surface objects** — interp kernel writes directly to D3D9 CUarray via
   surf2Dwrite, eliminating cudaOutputFrame + memcpy (~0.05ms). Requires .cu changes.
3. **CUDA stream priorities** — cuStreamCreateWithPriority for present/flow streams.
4. **D3D9Ex swap chain tuning** — BackBufferCount, swap effect experiments.

None expected to significantly reduce the 22 stalls.

## Pending Testing

1. **Performance impact** — measure game FPS delta with/without NvFBCR running
2. **Visual quality** — interpolation artifacts, motion smoothness on relay display