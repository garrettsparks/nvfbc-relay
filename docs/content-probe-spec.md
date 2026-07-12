# Content Probe (-probe) and Frame Dump (-dump) — Instrument Spec

Branch: `fg-and-dupe-content-probe` (off dev @ v0.0.13). Status: -probe implemented and
calibrated (2026-07-11 runs closed the SM x2 capture model); -dump implemented. Instruments,
not features: they inform the `-fg` frame-gen handling redesign and give dupe analysis
content ground truth.

## Problem

Two standing blind spots, both content-shaped, both invisible to timing:

1. **Frame-gen structure.** The 2026-07-11 characterization measured: SM ×2 ε-batches
   [gen, real] but ~33% of gen members produce no NvFBC wake (coalescing); SM ×3 ε-batches
   only the first gen and paces the second ~11ms out, timing-indistinguishable from a real
   frame. Timing cannot say which frame is real in a paced regime, and cannot say which
   member a coalesced wake kept.
2. **Dupe attribution.** Source content dupes (decoder resync metronome) vs relay repeats
   are separated today only by same-day A/B discipline; the log has no per-frame content
   signal.

## Design

`-probe` (command line or console prompt, valueless) enables two NvFBC session features on
the capture ring, reduced per grab to two log fields:

- **`blk=<changed>/<total>`** — NvFBC diffmap, 32×32 blocks, changed vs the previous grab.
  `blk=0` = content dupe (the direct dupe-attribution signal).
- **`hf=<sum>`** — classification-map byte sum (high-frequency-content measure, 16×16
  stamps). Candidate gen-frame fingerprint: interpolation loses high-frequency detail, so
  gen frames may read measurably lower than real neighbors. UNVERIFIED hypothesis — the map
  format is not documented; treat `hf=` as a relative signal and calibrate empirically
  before believing it.

Off by default; when off, the session bits are not set and cost is zero. Setup degrades one
rung at a time (full probe → diffmap-only → no probe), each rung logged loudly, so a missing
column always has a stated reason. Buffers are `VirtualAlloc`'d per the API contract and
freed after the capture thread joins.

## Cost model (why instrument-only)

Driver-side diff/classification work per grab plus a 3.6KB + 14.4KB CPU scan on the capture
thread. The scan runs after publish (never delays a frame) but widens the wake-to-regrab
window — the same window whose size sets the ε-coalescing rate the probe is measuring.
Observer effect is inherent: compare probe-on runs against the probe-off 2026-07-11
characterization baselines to quantify it before trusting probe-run batch statistics.

## Calibration / first runs

Same three KCD log-only characterization runs, probe on (`t:60 -probe`, 60s gameplay):

| run | question it answers |
|---|---|
| ×1 (no FG) | `blk=0` rate = source-dupe ground truth; `hf=` variance on known-all-real frames |
| SM ×2 | coalesced singles: `hf=`/`blk=` distribution vs pair members → which member survives coalescing |
| SM ×3 | lone paced frames: `hf=` vs pair members → does the gen fingerprint exist |

Plus the standing sentinel use: probe-on ufo run when the decoder metronome needs direct
confirmation, and a DLSS-G title (paced regime) when available.

## Frame dump (-dump <seconds>)

The probe answered every SM x2 question but left SM x3's structure undecodable from timing
plus hf alone (the paced single is sharp; no label assignment closes the frame accounting).
The dump is the ground-truth instrument: `-dump 60` stages 30 consecutive captures starting
60 s in and writes them as `dump_NN_capXXXX.bmp` beside the exe, with a `dump NN cap= arr=
dt= blk= hf= file=` log line mapping each file to its capture metadata.

Timeline integrity is the design constraint: writing to disk inside the capture loop
(10-50 ms/frame) would stall grab re-entry and decimate the very batch structure being
photographed. So the window stages GPU-to-GPU only (StretchRect into 30 pre-allocated
capture-device render targets, ~0.1 ms, ~250 MB VRAM for the run) and the drain — readback,
A2R10G10B10 to 24-bit unpack, BMP write — runs once the window is full, when the run's
timing no longer matters. A partial window drains at capture shutdown. The drain pauses
capture for its duration (logged); everything after the drain in that log is non-reference.

Run recipe for the x3 question: `t:60 -probe -dump 60`, SM x3, constant yaw pan through the
window. Interpolated frames show double-image ghosting under pan; a repeat is
pixel-identical to its predecessor; a warped/extrapolated frame shows edge smearing without
the double image. One pass over 30 BMPs assigns every batch position its true label.

## Analysis notes

Capture-line regex consumers are unaffected (fields append-only). Probe and dump runs are
instrument runs: never compare their timing statistics against reference numbers from
probe-off runs.
