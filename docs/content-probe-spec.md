# Content Probe (-probe) — Instrument Spec

Branch: `fg-and-dupe-content-probe` (off dev @ v0.0.13). Status: implemented, calibration
pending. Instrument, not a feature: informs the `-fg` frame-gen handling redesign and gives
dupe analysis content ground truth.

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

## Analysis notes

Capture-line regex consumers are unaffected (fields append-only). Probe runs are instrument
runs: never compare their timing statistics against reference numbers from probe-off runs.
