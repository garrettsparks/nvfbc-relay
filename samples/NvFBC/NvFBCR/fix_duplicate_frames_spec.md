# Fix Duplicate Frames in FRUC Pipeline

## Problem

At 60fps source → 60fps output (Big Buck Bunny test), the output has visible stutter from
duplicate frames despite 0 dropped frames and ~5.7ms headroom. The pipeline has plenty of
budget — the problem is that NvFBC captures the same source frame ~10 times before the
source updates, and every capture overwrites the ring buffer and advances it. Both ring
buffer slots end up with identical pixel data, so NvOF computes zero motion vectors and
interpolation just copies the same frame repeatedly.

**Root cause:** No source frame change detection. The 2-slot ring buffer blindly advances
on every NvFBC grab, even when the captured content is identical to the previous grab.

## Key Discovery: NvFBC Has Built-In New-Frame Detection

The grab flags in `nvFBCToDx9Vid.h` (line 106-108):
```
NVFBC_TODX9VID_NOFLAGS           = 0x0   // Waits for a NEW frame or HW cursor move
NVFBC_TODX9VID_NOWAIT            = 0x1   // Grabs immediately, even if same frame
NVFBC_TODX9VID_WAIT_WITH_TIMEOUT = 0x10  // Waits for new frame, with timeout
```

**Previously:** `dwFlags = NVFBC_TODX9VID_NOWAIT` (NvFBCR.cpp:740). NvFBC returned SUCCESS
on every grab even when the source frame hadn't changed, producing ~10 identical captures
per source frame.

**Fix (DONE):** Switched to `NVFBC_TODX9VID_WAIT_WITH_TIMEOUT` with `dwWaitTime = 2`.
NvFBC now only returns SUCCESS when a genuinely new frame is available (or HW cursor moves).
The existing code path at FrucCaptureMode.cpp:335-338 already handled non-SUCCESS as
"no new frame" and skips ring buffer advancement.

## Results: WAIT_WITH_TIMEOUT

| Metric                | Before (NOWAIT) | After (WAIT_WITH_TIMEOUT=2ms) |
|-----------------------|-----------------|-------------------------------|
| captures/300 presents | ~3100           | ~350-650                      |
| flow (ms/present)     | ~9.5            | ~1.3                          |
| work (ms/present)     | ~15.5           | ~2.2                          |
| headroom (ms)         | ~1.2            | ~14.4                         |
| dropped               | 0               | 0                             |

Massive improvement. Duplicate frames largely eliminated. Output is mostly smooth.

**Remaining issue:** ~1 second periodic hitch. Same issue previously seen with
FrameTemporalCaptureMode, which was fixed by switching to VSync-driven presentation.
Cause: `Sleep()` on Windows has ~1-2ms jitter that accumulates into a periodic stutter.

## Phase A: VSync-Driven Presentation — DONE, WEIGHT FIX NEEDED

Switch from `D3DPRESENT_INTERVAL_IMMEDIATE` + Sleep-based timing to
`D3DPRESENT_INTERVAL_ONE` (VSync). PresentEx blocks until VBlank, which is
hardware-timed with zero drift.

### Critical: VBlank Time as Interpolation Target

**The key invariant:** We always have two source frames in the ring buffer. When VBlank
fires, we interpolate between them at the point corresponding to the VBlank moment. This
produces smooth output regardless of source framerate.

**The bug in the initial VSync implementation:** Weight was calculated using "now" (after
capture), which is always past frame1's timestamp → weight ≈ 1.0 → no actual interpolation,
just showing the latest frame. Confirmed by logs: every frame had `weight=1.000`.

**The fix:** Use the VBlank time as the interpolation target, not "now". The VBlank time
naturally falls between the two ring buffer frames:

```
Iteration N:
  1. Capture frame1 → ring buffer has [frame0, frame1]
  2. Compute flow (frame0 → frame1)
  3. Interpolate at weight = (lastVBlankTime - t0) / (t1 - t0)
  4. PresentEx(INTERVAL_ONE) — blocks until VBlank
  5. Record VBlank time: QueryPerformanceCounter(&lastVBlankTime)
  6. → Go to step 1 (next iteration)
```

**Why this works:** frame0 was captured right after the PREVIOUS VBlank. frame1 was
captured right after THIS VBlank (step 1). The VBlank moment (step 5 of the previous
iteration) sits between frame0's timestamp and frame1's timestamp:

```
prev VBlank          this VBlank
    |                    |
    V                    V
----+--------------------+--------------------
    |  frame0            |  frame1
    |  captured          |  captured
    t0                   t1

    lastVBlankTime is HERE ↑
    (recorded at prev iteration's step 5)

    weight = (lastVBlankTime - t0) / (t1 - t0)
           ≈ 0.95 for 60fps/60Hz (VBlank slightly before t1)
           varies for non-matching rates
```

**For non-matching rates (e.g., 90fps source on 60Hz output):**
The source frames arrive at ~11ms intervals. We capture once per VBlank (~16.67ms).
Some VBlanks we get 1 new source frame, some we get 2. The ring buffer pair timestamps
vary, and the VBlank time falls at different positions within each pair. This is correct
behavior — the weight adapts to wherever the VBlank actually falls between the two most
recent source frames.

**For the first frame:** No lastVBlankTime exists yet. Use weight=0.5 as default until
the first PresentEx returns and we have a VBlank timestamp.

### Loop Structure

```cpp
LARGE_INTEGER lastVBlankTime = {};
bool hasVBlankTime = false;

while (TRUE) {
    // 1. Capture new source frame
    CaptureFrame()

    // 2. If we have a valid frame pair:
    if (frame0 && frame1) {
        // Compute flow once per new pair
        if (newPair) ComputeOpticalFlow()

        // Weight from VBlank time (falls between frame0 and frame1)
        float weight = 0.5f;
        if (hasVBlankTime && t1 > t0) {
            weight = clamp((lastVBlankTime - t0) / (t1 - t0), 0, 1);
        }

        InterpolateFrame(weight)

        // VSync present — blocks until VBlank (this IS the timing mechanism)
        PresentFromGPU()  // PresentEx(D3DPRESENT_INTERVAL_ONE)

        // Record VBlank time for next iteration's weight calculation
        QueryPerformanceCounter(&lastVBlankTime)
        hasVBlankTime = true;
    }

    ProcessMessages()
}
```

### Results: VSync Mode (before weight fix)

```
300 presents, 300 captures (perfect 1:1)
flow=1.2ms, work=1.2ms, present=3.5ms (includes VSync wait)
weight avg=1.000 min=1.000 max=1.000  ← BUG: no actual interpolation
```

No 1-second hitch (VSync provides hardware-timed cadence). But weight=1.0 means we're
just frame-selecting, not interpolating. The VBlank weight fix addresses this.

## Phase B: Multithreaded Capture + VSync Present (future)

See: `multithreaded_fruc_spec.md`

Separates capture (event-driven, source frame rate) from presentation (VSync-driven,
output display rate). Each runs on its own schedule without blocking the other.

**When this becomes necessary:** Single-threaded VSync captures once per VBlank, so the
ring buffer pair always spans roughly one VBlank interval. At 90fps source, we miss
intermediate source frames — the pair spans ~16.67ms instead of ~11ms (one source frame).
Multithreaded capture keeps the ring buffer at source rate for tighter frame pairs and
better flow vectors. But single-threaded with the VBlank weight fix should produce correct
interpolation for any source rate — just with coarser frame pairs for high source rates.

## Notes

### HW Cursor
`bWithHWCursor = 1` in setup (NvFBCR.cpp:720). WAIT modes trigger on cursor moves too.
Since cursor is composited, this causes captures + flow recomputation on mouse movement
even with static scenes. For FRUC mode, `bWithHWCursor = 0` would be better — cursor
is an OS overlay that doesn't need interpolation. Separate decision.

### Impact on other capture modes
The WAIT_WITH_TIMEOUT change in NvFBCR.cpp affects all capture modes. If it breaks other
modes, make it FRUC-specific by overriding `grabParams->dwFlags` in `FrucCaptureMode::Run()`.