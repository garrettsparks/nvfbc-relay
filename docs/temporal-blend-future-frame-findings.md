# Temporal vs Blend: future-frame handling (findings)

Findings from reviewing whether the two non-vsync resampling modes correctly use a
"future" frame. Code reviewed at dev `fb0e6e3`.

> **Update:** Temporal's vestigial selection (described below) has since been **fixed** as part
> of the temporal-mode rebuild — it now captures with a blocking grab (true source timestamps),
> brackets the target on a shared absolute-QPC schedule, and picks the genuinely *nearest*
> frame (before or after). See **`temporal-capture-mode-spec.md`**. Blend still awaits its own
> rebuild on the same foundation; the analysis below remains the rationale.

## The principle

To resample *for* a target present time `T` — whether by selecting the nearest captured
frame or by interpolating across `T` — you need a frame captured **after** `T` to bracket
it. You cannot know which captured frame is closest to `T`, or blend across `T`, until you
have captured past it. So any honest temporal resampling presents **~1 output frame behind**
real time (bounded by how soon a post-`T` capture arrives). That latency is inherent, not a
bug.

This is the opposite of `TimerCaptureMode`, which presents the newest frame available *at*
the deadline (no look-ahead, minimal latency) and needs no future frame.

## Blend — correct

`FrameBlendCaptureMode` honors the principle explicitly. It only presents once a frame past
the target time exists (`FrameBlendCaptureMode.cpp`, ~line 227):

```cpp
bool hasAfterFrame = false;            // a frame with timestamp > nextPresentTime
... scan history ...
// Only present if we have frames bracketing the present time
// This adds latency but gives true temporal interpolation
if (hasAfterFrame) { BlendFramesToBackbuffer(...); present; nextPresentTime += ticksPerFrame; }
```

So blend genuinely runs ~1 frame behind and interpolates between the bracketing frames.
Working as intended; the latency is acknowledged in its own comment. It also grabs at ~3×
output rate, so the post-`T` frame arrives quickly and the added latency stays under one
full output period.

## Temporal — future-frame handling is vestigial

`FrameTemporalCaptureMode` does **not** use a future frame, in two places:

1. **Present trigger has no future-frame gate** (`FrameTemporalCaptureMode.cpp:157`):
   ```cpp
   if (currentTime.QuadPart >= nextPresentTime.QuadPart) { SelectFrameToBackbuffer(...); present; }
   ```
   It fires the instant the deadline is reached — when, by definition, nothing past `T`
   has been captured yet. (Unlike blend, which gates on `hasAfterFrame`.)

2. **The selector ignores the future frame even when one exists.** `SelectFrameToBackbuffer`
   (~line 188) computes both the nearest-before (`bestBefore`) and nearest-after
   (`bestAfter`) frames, but when both are present it deliberately uses the before frame
   (~line 213):
   ```cpp
   // Prefer frames from the past (before target time)
   if (bestBefore >= 0 && bestAfter >= 0) { /* use bestBefore */ }
   ```
   `bestAfter` is only ever used as a fallback when there is no before frame at all.

Net effect: temporal **always presents the newest frame at or before the deadline**. That
makes it functionally equivalent to `TimerCaptureMode`, just with a frame-history buffer and
a selection routine whose `bestAfter` machinery never changes the output. It is not doing
temporal selection in any meaningful sense.

## Options for temporal

- **A — make it truly bracket.** Gate the present on a future frame (as blend does) *and*
  pick the genuinely closest of `bestBefore`/`bestAfter` (compare `smallestBeforeDiff` vs
  `smallestAfterDiff`) instead of always preferring before. This gives real nearest-frame
  selection — reduced judder on rate conversions — at the cost of ~1 frame of latency, like
  blend. This is the version that justifies the mode's existence.
- **B — remove it as redundant.** With the timer mode now correct (absolute QPC schedule),
  temporal as written adds nothing over it but overhead. If the ~1-frame latency of option A
  is unwanted, temporal has no distinct reason to exist.

## Relationship to scheduling unification

This is also why the temporal/blend modes cannot simply adopt `TimerCaptureMode`'s loop
wholesale: their **present trigger** is mode-specific.

- Timer: "deadline reached → present newest."
- Blend (and temporal-under-option-A): "deadline reached **and** a post-`T` frame exists →
  present the bracketed result."

Only the deadline-*scheduling* math (period rounding, `+= period` advance, the >1-period
catch-up clamp) is shareable across all three; the trigger condition and the grab cadence
(temporal/blend grab faster than they present, to build history) are not.

## Validation reminder

Any change to either mode must be re-validated with a `t:60` / `b:60` capture through
`detect.py` (MAD dupes + pacing distribution) against a same-session `vsync` baseline — per
`frame-pacing-drift-analysis.md`. "It looks basically the same" is not sufficient; we have
been wrong about that before.
