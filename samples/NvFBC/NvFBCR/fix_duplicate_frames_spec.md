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

## Phase A: VSync-Driven Presentation (next step)

Switch from `D3DPRESENT_INTERVAL_IMMEDIATE` + Sleep-based timing to
`D3DPRESENT_INTERVAL_ONE` (VSync). PresentEx blocks until VBlank, which is
hardware-timed with zero drift.

**File:** `FrucCaptureMode.cpp` `Run()` main loop

The loop structure changes from timer-driven to VSync-driven:
```
// BEFORE (timer-driven):
while (TRUE) {
    CaptureFrame(WAIT_WITH_TIMEOUT=2ms)
    PresentAtTargetTimes()      // manual timer + Sleep
    Sleep(untilNextTarget)
}

// AFTER (VSync-driven):
while (TRUE) {
    CaptureFrame(WAIT_WITH_TIMEOUT=2ms)
    if (newPair) ComputeOpticalFlow()
    InterpolateFrame(weight)    // weight from current time vs frame pair timestamps
    PresentFromGPU()            // PresentEx(D3DPRESENT_INTERVAL_ONE) — blocks until VBlank
}
```

VSync present replaces Sleep as the timing mechanism. No manual timer, no drift.

**Concern:** With VSync blocking ~14ms per frame, CaptureFrame only runs once per
VBlank (~60 times/sec on 60Hz output). For source rates >> 60fps, we only see the
latest source frame each VBlank — we miss intermediate source frames. This is fine for
the interpolation (we always get the freshest pair), but the time gap between ring buffer
frames equals the VBlank interval rather than the source frame interval.

**Bigger concern for non-60fps sources:** If source is 90fps on a 60Hz output, the
timing relationship between source frames and VBlanks drifts. Some VBlanks will see 1
new source frame, others 2. The ring buffer pair timestamps will be uneven. This should
still work — the interpolation weight adjusts based on actual timestamps — but it's not
ideal. The multithreaded approach (Phase B) handles this properly.

## Phase B: Multithreaded Capture + VSync Present (future)

See: `multithreaded_fruc_spec.md`

Separates capture (event-driven, source frame rate) from presentation (VSync-driven,
output display rate). Each runs on its own schedule without blocking the other.

## Notes

### HW Cursor
`bWithHWCursor = 1` in setup (NvFBCR.cpp:720). WAIT modes trigger on cursor moves too.
Since cursor is composited, this causes captures + flow recomputation on mouse movement
even with static scenes. For FRUC mode, `bWithHWCursor = 0` would be better — cursor
is an OS overlay that doesn't need interpolation. Separate decision.

### Impact on other capture modes
The WAIT_WITH_TIMEOUT change in NvFBCR.cpp affects all capture modes. If it breaks other
modes, make it FRUC-specific by overriding `grabParams->dwFlags` in `FrucCaptureMode::Run()`.
