# Multithreaded FRUC: Capture Thread + VSync Present Thread

## Motivation

The single-threaded FRUC loop has a fundamental tension:
- **Capture is event-driven**: new source frames arrive at a variable rate (90fps, 144fps, etc.)
- **Presentation is time-driven**: VBlank fires at the output display's fixed refresh rate (60Hz)

In a single thread, one blocks the other. With VSync present, `PresentEx(INTERVAL_ONE)`
blocks ~14ms per frame. During that block, no captures happen — source frames are missed.
For a 90fps source on a 60Hz output, we'd miss ~1 out of every 3 source frames. The ring
buffer pair would span ~33ms instead of ~11ms, producing coarser flow vectors.

With two threads, capture runs continuously at source rate while presentation runs at VBlank
rate. The ring buffer always has the two most recent source frames with the tightest possible
time gap, producing the best flow vectors and interpolation quality.

## Architecture

```
┌─────────────────────────────────┐     ┌──────────────────────────────────────┐
│       Capture Thread            │     │         Present Thread (main)        │
│                                 │     │                                      │
│  loop:                          │     │  loop:                               │
│    NvFBC grab (BLOCKING mode)   │     │    PresentEx(INTERVAL_ONE)           │
│    Set flag: newFrameReady      │     │      ← blocks until VBlank           │
│    Signal event                 │     │                                      │
│                                 │     │    If newFrameReady:                 │
│  (Only NvFBC API calls here.    │     │      StretchRect capture→interop     │
│   No D3D9. No CUDA.)           │     │      CUDA map → ring buffer → unmap  │
│                                 │     │      Advance ring buffer             │
│                                 │     │      If new pair: ComputeOpticalFlow │
│                                 │     │                                      │
│                                 │     │    Calculate weight from VBlank time │
│                                 │     │    InterpolateFrame(weight)          │
│                                 │     │    Copy output → D3D9 → backbuffer   │
│                                 │     │    (PresentEx at top of next iter)   │
└─────────────────────────────────┘     └──────────────────────────────────────┘
         │                                           │
         │  Shared state:                            │
         │  - m_captureTarget (D3D9 surface)         │
         │  - newFrameReady flag (atomic or event)   │
         └───────────────────────────────────────────┘
```

### Key Design Decisions

**1. Capture thread only calls NvFBC — no D3D9, no CUDA.**

D3D9 is not thread-safe. By keeping all D3D9 calls (StretchRect, PresentEx) and all CUDA
calls (interop map/unmap, NvOF, kernel) on the present thread, we avoid all D3D9 threading
issues. The capture thread's only job is calling `NvFBCToDx9VidGrabFrame` in blocking mode
and signaling that a new frame is ready.

**2. NvFBC grab uses NOFLAGS (fully blocking) mode.**

With a dedicated capture thread, there's no reason for timeouts. `NVFBC_TODX9VID_NOFLAGS`
blocks until a genuinely new source frame arrives. Zero wasted CPU cycles. The thread
sleeps in the driver until the source updates.

**3. Synchronization via Windows Event + atomic flag.**

```cpp
HANDLE m_newFrameEvent;          // Auto-reset event, signaled by capture thread
std::atomic<bool> m_newFrameReady;  // Flag checked by present thread
std::atomic<bool> m_captureRunning; // Shutdown signal
```

Capture thread:
```cpp
while (m_captureRunning.load()) {
    NVFBCRESULT res = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);
    if (res == NVFBC_SUCCESS) {
        m_newFrameReady.store(true);
        SetEvent(m_newFrameEvent);  // Wake present thread if it's waiting
    }
    if (res == NVFBC_ERROR_INVALIDATED_SESSION) break;
}
```

Present thread checks `m_newFrameReady` after each VBlank return. The event is only needed
if we want the present thread to wake early (before VBlank) when a frame arrives — in practice,
VBlank already provides the cadence so the flag is sufficient.

**4. m_captureTarget is the shared surface.**

NvFBC writes to `m_captureTarget` on the capture thread. The present thread reads it via
`StretchRect`. These never overlap because:
- NvFBC grab blocks until complete (writes are done when it returns)
- Present thread only reads after seeing `newFrameReady = true`
- Present thread clears the flag after StretchRect
- Capture thread won't signal again until the NEXT NvFBC grab completes

Race window: if NvFBC starts writing the NEXT frame while StretchRect is reading the
current frame. This is possible if source frames arrive very fast. Mitigation: the present
thread should StretchRect immediately after checking the flag, before doing any flow/interp
work. At 1280x720, StretchRect takes ~5μs — the window is tiny. For extra safety, could
add a mutex around the capture target access, but likely unnecessary in practice.

## Implementation Plan

### New members (FrucCaptureMode.h)
```cpp
// Threading
HANDLE m_captureThread;
HANDLE m_newFrameEvent;
std::atomic<bool> m_newFrameReady;
std::atomic<bool> m_captureRunning;

// NvFBC refs needed by capture thread
NvFBCToDx9Vid* m_nvfbcDx9;
NVFBC_TODX9VID_GRAB_FRAME_PARAMS* m_grabParams;
```

### Capture thread function
```cpp
static DWORD WINAPI CaptureThreadProc(LPVOID param) {
    FrucCaptureMode* self = (FrucCaptureMode*)param;

    while (self->m_captureRunning.load()) {
        NVFBCRESULT res = self->m_nvfbcDx9->NvFBCToDx9VidGrabFrame(self->m_grabParams);

        if (res == NVFBC_SUCCESS) {
            self->m_newFrameReady.store(true);
            SetEvent(self->m_newFrameEvent);
        }
        else if (res == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated on capture thread");
            break;
        }
        // Non-SUCCESS, non-error: no new frame, loop again
    }
    return 0;
}
```

### Present thread (main loop in Run())
```cpp
// Set grab to blocking mode for capture thread
grabParams->dwFlags = NVFBC_TODX9VID_NOFLAGS;

// Start capture thread
m_captureRunning.store(true);
m_newFrameReady.store(false);
m_newFrameEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
m_captureThread = CreateThread(NULL, 0, CaptureThreadProc, this, 0, NULL);

while (TRUE) {
    // === PRESENT (VSync-driven) ===
    // PresentEx blocks until VBlank — this IS the timing mechanism
    if (hasOutputFrame) {
        PresentFromGPU(device);  // Uses D3DPRESENT_INTERVAL_ONE internally
    }

    // === CAPTURE LATCH ===
    // Check if capture thread has a new frame
    if (m_newFrameReady.load()) {
        m_newFrameReady.store(false);

        // All D3D9 + CUDA work happens here on the present thread
        StretchRect(m_captureTarget → m_interopCaptureSurface);
        CUDA map → cuMemcpy2D → ring buffer → unmap;
        AdvanceRingBuffer();

        if (haveTwoValidFrames) {
            flowComputedForCurrentPair = false;
        }
    }

    // === FLOW + INTERPOLATION ===
    if (haveTwoValidFrames) {
        if (!flowComputedForCurrentPair) {
            ComputeOpticalFlow();
            flowComputedForCurrentPair = true;
        }

        // Weight: where does "now" fall between the two source frame timestamps?
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float weight = (now - t0) / (t1 - t0);
        weight = clamp(weight, 0.0f, 1.0f);

        InterpolateFrame(weight, prevFrame, currFrame);
        hasOutputFrame = true;
    }

    // Window messages
    ProcessMessages();
}

// Shutdown
m_captureRunning.store(false);
WaitForSingleObject(m_captureThread, 5000);
CloseHandle(m_captureThread);
CloseHandle(m_newFrameEvent);
```

## Timing Behavior

### 90fps source on 60Hz output
```
Source:    S0    S1    S2    S3    S4    S5    S6    S7    S8
          |--11ms--|--11ms--|--11ms--|--11ms--|--11ms--|--11ms--|

VBlank:        V0              V1              V2              V3
               |----16.67ms----|----16.67ms----|----16.67ms----|

Ring buf:  [S0,S1]→[S1,S2]→[S2,S3]→[S3,S4]→[S4,S5]→...
                      ↑                ↑                ↑
                   V0 reads         V1 reads         V2 reads
                   pair [S1,S2]     pair [S3,S4]     pair [S5,S6]
                   weight≈0.5       weight≈0.2       weight≈0.9
```

The capture thread updates the ring buffer at source rate (90fps). The present thread
reads the latest pair at VBlank rate (60fps). Every output frame interpolates between
the two most recent source frames with the tightest possible time gap (~11ms at 90fps).

### Compared to single-threaded VSync
With single-threaded VSync, capture only runs once per VBlank (60 times/sec). At 90fps
source, ~30 source frames are missed per second. The ring buffer pair spans ~16.67ms
(VBlank interval) instead of ~11ms (source interval). Flow vectors are ~50% larger,
interpolation quality is worse on fast motion.

## Risks

### NvFBC thread safety
`NvFBCToDx9VidGrabFrame` internally writes to a D3D9 surface (`m_captureTarget`). Is this
thread-safe with respect to D3D9 calls on the present thread? NvFBC is a proprietary
NVIDIA API — its internal threading model isn't documented. The StretchRect on the present
thread reads `m_captureTarget` while NvFBC might be writing the next frame on the capture
thread. In practice, NvFBC likely uses a different GPU engine (copy engine vs 3D engine)
and the operations don't overlap at the GPU level, but this is unverified.

**Mitigation:** If we see corruption, add a mutex:
```cpp
// Capture thread:
WaitForSingleObject(m_captureMutex, INFINITE);
NvFBCToDx9VidGrabFrame(grabParams);
ReleaseMutex(m_captureMutex);
m_newFrameReady.store(true);

// Present thread:
if (m_newFrameReady.load()) {
    WaitForSingleObject(m_captureMutex, INFINITE);
    StretchRect(m_captureTarget → m_interopCaptureSurface);
    ReleaseMutex(m_captureMutex);
    m_newFrameReady.store(false);
    // ... rest of capture latch
}
```

### Interpolation weight > 1.0
If VBlank fires AFTER the latest source frame pair (i.e., `now > t1`), the raw weight
exceeds 1.0. This means we're extrapolating, not interpolating. Clamping to 1.0 just
shows frame1 directly — no interpolation artifact, but also no motion compensation.
This happens when the source framerate drops below the output refresh rate (e.g., source
dips to 50fps on a 60Hz output). Acceptable behavior.

### Shutdown
The capture thread may be blocked in `NvFBCToDx9VidGrabFrame` when we want to exit.
Need to either:
- Use WAIT_WITH_TIMEOUT (e.g., 100ms) instead of NOFLAGS, so the thread wakes periodically
  to check `m_captureRunning`
- Or call NvFBC release/destroy from the main thread, which should cause the grab to
  return with an error

Using WAIT_WITH_TIMEOUT(100ms) is safer — it guarantees the thread exits within 100ms
of shutdown request, with no dependency on NvFBC's error behavior.

## Complexity Assessment

- **New code:** ~50 lines (thread proc, thread start/stop, flag checks in main loop)
- **Removed code:** Sleep-based timing, manual target time tracking
- **Risk:** Medium — NvFBC thread safety is unverified, but the shared surface access
  pattern (write-then-signal, read-then-clear) minimizes overlap
- **Testable:** Run with 90fps source, check that captures match source rate (~90/sec)
  while presents match VBlank rate (~60/sec)