# D3DCREATE_MULTITHREADED Removal — Experiment Spec

Branch: `claude/no-mt-lock` (stacked on `direct-write-capture-ring`). Status: implemented,
awaiting validation. This is the deferred experiment the baseline spec filed as "possibly
unnecessary — but every validated result was measured with it on. Removing it is a separate,
low-priority experiment requiring the full validation suite, not a cleanup."

## What it does

Removes `D3DCREATE_MULTITHREADED` from all three device creations:

| device | created | used | concurrent access? |
|---|---|---|---|
| present device (`InitD3D9`) | main thread | main thread only (present loop, StretchRect/quad, PresentEx) | none |
| capture device (`CaptureRing::Start`) | main thread | capture thread only after `std::thread` start | none — thread creation is the sync point; `Stop()` joins before the destructor releases resources |
| diag raster device (`DiagCaptureMode`) | main thread | main thread only | none |

The flag makes the D3D9 runtime take a critical section around every device call. Under
single-thread-per-device ownership that lock is pure overhead — small (uncontended lock
acquire per call, a handful of calls per frame), but nonzero, and its absence is also a
*correctness statement*: this diff asserts the threading discipline the architecture claims.

## Why this is an experiment, not a cleanup

Strictly, D3D9 documentation frames non-MT devices as single-thread; sequenced cross-thread
use (create on A, use on B, never concurrent) is safe in practice given proper happens-before
edges (thread start/join), but it is the kind of thing a driver could be pedantic about. The
debug runtime may warn. Every validated number to date was measured with the flag ON. Hence:
full validation, not a wave-through.

## Interaction with the stack

Stacked on direct-write deliberately: both branches touch `CaptureRing.cpp`'s device path, and
the perf story is cumulative (no copy + no lock). Testing this branch = testing both; if this
branch regresses where direct-write alone passed, the lock removal is the cause — attribution
stays clean because direct-write validates first, one layer down the stack.

## Validation

Full suite class, because the flag change touches the shared `InitD3D9` path that plain modes
use too:

| run | pass |
|---|---|
| `60_x1_vsync_ufo` (V1-class) | gold standard unchanged |
| `240_x1_t_60_nomt_ufo` (V3-class) | reverse 0, 0 dupes, stride windows 0, flush/dt profile ≈ direct-write branch |
| `120cap_x2_t_vsync_nomt_kcd` (V4-class) | jit ≈3 µs, pdt ≈16667 µs, collapse healthy, crisp |

Also watch startup: any `CreateDeviceEx` failure or debug-runtime complaint is a loud finding.
Rollback: revert this one commit; the flag returns everywhere.
