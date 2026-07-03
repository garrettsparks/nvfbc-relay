# Configurable Ring Capacity — Feature Spec

Branch: `claude/ring-capacity-config` (off dev @ v0.0.11). Independent. Status: implemented.

## What it does

`CaptureRing`'s slot count becomes a startup configuration: `-ring N` command-line argument,
default 8 (the validated baseline), clamped to `[3, MAX_RING_SIZE=32]` at construction. The
backing array is statically `MAX_RING_SIZE`; only `m_capacity` slots are created and cycled.
`FindBracket` scans `[p−(capacity−1), p−1]`.

## Why startup-config, not runtime-adaptive

"Adaptive ring size" was considered and deliberately rejected in its literal form:

- Slots are shared GPU textures whose present-device aliases were opened from shared handles
  at `Start()`. Growing/shrinking mid-session means creating/destroying shared resources
  while the present thread holds borrowed aliases — a direct violation of
  publish-then-never-touch and an invitation to the exact cross-device races T4 exists to
  prevent.
- The quantity that varies at runtime (FG multiplier k, source rate) changes *slowly and
  regime-wise*; the ring does not need to resize within a session, only to be *sized right
  for the session*. That is a launch decision.
- VRAM cost is the real constraint (each slot ≈ width×height×4 bytes; 32 slots at 1440p
  ≈ 470 MB), so an unconditional "always allocate huge" default is wrong too.

## Sizing guidance (why you'd change it)

Keep-real batch collapse retracts k−1 of every k wakes at FG multiplier k, leaving
~capacity/k *valid* frames spanning the ring:

| regime | valid frames at capacity 8 | guidance |
|---|---|---|
| no FG | 8 | default fine |
| ×2 (validated baseline) | 4 | default fine (measured d ≤ 5) |
| ×3 | ~2.7 | thin — `-ring 12` recommended |
| ×k | ~8/k | `-ring 4k` keeps ~4 valid frames |

The logged bracket depth `d<n>` and the ring-miss `LOGERR` are the observability: if `d`
approaches capacity or misses appear, the ring is undersized for the regime.

## Future (Path B interaction)

Gen-frame pass-through (publish all batch members re-stamped) multiplies the *publish* rate by
k instead of retracting — same sizing arithmetic from the other direction. A future auto-size
could set capacity from the measured batch-size mode k̂ at session start; that wants the
predicted-k̂ machinery Path B builds anyway, so it is deferred to that work rather than
pre-built here.

## Validation

Defaults unchanged → behavior-identical to v0.0.11 when `-ring` is absent (the only diff on
that path is `capacity` living in a member instead of a constant). Spot check: one
`t:60` @240 UFO run with `-ring 8` (expect identical numbers) and one with `-ring 4`
(expect functional behavior with visibly smaller `d` headroom; misses acceptable only under
deliberately hostile lag). Init log line reports the configured slot count.
