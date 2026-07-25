# ETW Frame-Timing Spec (reading the driver's real scanout times)

Status: DESIGN. Probe built and CI-green; no measurements taken yet, no relay code consuming it.
Companions: `dxgi-native-pipeline-spec.md`, `nvfbc-capture-pacing.md`, `frame-marker-spec.md`.

## The problem

The relay stamps every captured frame with the time NvFBC woke up. That is submission time, and
under frame generation it is wrong in a way that matters. Measured on this hardware:

| | measured |
|---|---|
| gap between real and generated frame ARRIVING at NvFBC | 0.35 - 0.50 ms |
| gap between those frames APPEARING on screen | 8.33 ms |

The driver submits the pair together and scans them out half a source period apart. So the ring
believes two frames representing motion 8.33 ms apart are 0.4 ms apart. Neither NvFBC nor DXGI
exposes the scanout time, and neither labels a frame real or generated.

Today the relay sidesteps this: batch-collapse keep-real discards the generated frame and stamps
the survivor at batch-start. Correct at 60 -> 60, where only real frames are needed.

## Why it is worth fixing

At 90 fps source -> 60 output, 90 does not divide into 60, so roughly half the output frames are
synthesized by the relay (a lerp of two real frames). But Smooth Motion has already produced 180
frames per second, and 180 / 60 = 3 exactly. Every output frame could land on a real captured
frame with no blending at all, IF the generated frames are stamped at the times they actually
appeared.

The comparison is not gen-vs-real (Round 10 settled that: real wins). It is the relay's linear
cross-fade versus the driver's motion interpolation, for the ~50% of output frames that cannot be
a real frame. The driver already paid for those frames; re-synthesizing worse ones is waste.

## Decisions

**Raw ETW, not PresentMon.** The NVIDIA timing data is a single event with four fields, so the
"large fragile surface" argument does not apply. More decisively, PresentMon COLLAPSES exactly the
data we need: `NVTraceConsumer` dedupes by token, keeps only the latest flip time per head, and
folds the result into one `MsFlipDelay` column on the application's present row. The individual
generated-frame flip times never reach its CSV. Reading the provider directly is the only way to
see them.

**ETW is retained and drives alignment continuously.** Not a one-time calibration that bakes
constants into a hardcoded pattern. A model that assumes "2x, inserted at the midpoint" fails
SILENTLY on x3, on MFG, and on any driver change: it produces confident wrong stamps. Reading
actual scanout times derives the cadence instead of assuming it, including for patterns nobody has
characterized yet.

**Enrichment, not dependency.** The existing stamping rule (arrival time, batch-start for
intra-batch members, keep-real) stays as the baseline and never goes away. ETW UPGRADES a slot's
timestamp from estimated to measured when the data arrives in time. Nothing branches on "is ETW
available"; the timestamp is either better or it is not. Dropping the lag floor back to 20 ms must
leave the relay working as it does today.

## Architecture

```
 NvFBC capture ─► ring (pixels + arrival-stamped timestamp)   <- works standalone, as today
 NVIDIA DisplayDriver ETW ─► flip history (capture_seq -> true scanout time)
                               │  upgrade slots not yet bracketed
                               ▼
                     policy sees a measured timeline when available,
                     the estimated one when not
```

**The coherence rule: only upgrade slots that have not been bracketed yet.** If a slot's timestamp
changed after the policy bracketed against it, a frame could move from "before" to "after" under
the policy's feet. Comparing the slot's sequence number against the newest sequence the policy has
consumed closes that.

That one rule is also the entire degradation path. At a 90 ms floor an upgrade landing ~30 ms
after capture arrives long before the target (90 ms back) reaches the slot, so nearly everything
upgrades. At 20 ms the upgrade almost always arrives after the slot was used and is declined. No
separate fallback code exists; the ordering rule produces both behaviours.

For generated frames specifically: with ETW they get a measured stamp and are kept; without, no
stamp arrives in time and keep-real retracts them as today. 120 usable frames/s with, 60 without.

**Sizing.** ETW history is metadata, so size it by TIME, not by ring slots:

```
{capture_seq u64, display_qpc u64, token u32, head u32} = 24 bytes
5 seconds at 180 events/s = ~900 records = ~22 KB
```

Keyed by the monotonic capture counter, never by slot index, so recycling cannot corrupt it.

The ring itself must grow only if the lag floor is raised, because the frames have to survive
until the target reaches them:

| capture rate | frames alive for a 90 ms floor + ~2 source periods | RING_SIZE |
|---|---|---|
| 120/s (60x2) | ~123 ms | >= 16 |
| 180/s (90x2) | ~123 ms | >= 22, use 32 |

32 slots is ~253 MiB at 1920x1080x4B. Negligible on this card.

**The lag floor is a runtime knob, not a design commitment.** Because degradation is graceful, the
relay can ship at today's 20833 us and raise the floor per-profile. Relay output latency does not
touch the player's input loop (gameplay happens on the source display; the relay feeds the XR1 ->
OBS PC -> Twitch), so 90-120 ms costs stream viewers a rounding error on top of seconds they
already have. The lag must stay CONSTANT for audio-sync compensation, which the existing
launch-time-constant design already guarantees.

## The provider

```
NVIDIA DisplayDriver {AE4F8626-8265-40D1-A70B-11B64240E8E9}
FlipRequest (Id 1, level 0x04, keyword 0x1000000000000000)
fields: alloc (u64), vidPnSourceId (u32), ts (u64), token (u32)
```

`ts` is the PROPOSED FLIP TIME in QPC ticks. Consecutive `ts` values on one head are the true
scanout cadence. GUID and descriptor come from the manifest PresentMon embeds, cross-checked
against its consumer; field order above is READ order from that consumer, which is not necessarily
wire order.

## EtwProbe

Standalone exe, in the solution so CI builds it. Requires elevation. Reports:

- **dts histogram** - gaps between consecutive proposed flip times on a head. A tight spike near
  8.33 ms at 60x2 means the grid is even and readable. Smeared or multi-modal kills the arithmetic
  model outright.
- **ahead** - `ts` minus the event stamp: how far in advance the driver schedules a flip.
- **lag** - callback time minus event stamp: raw ETW delivery latency. This sizes the floor.
- **hexdump of the first 8 payloads**, always. `TdhGetProperty` resolves field names only if the
  manifest is registered, and PresentMon embedding NVIDIA's manifest is evidence it may not be. If
  TDH fails the hexdump is how the wire layout gets recovered from hardware instead of guessed.
- **`--dxgk` control** - also enables DxgKrnl and counts it. If NVIDIA events are zero but DxgKrnl
  is not, the session works and the provider is the problem. Without the control a silent probe
  has five possible causes.

## Open questions

1. **Raw ETW delivery lag distribution. UNMEASURED.** The ~30 ms figure is PresentMon's own
   pipeline, not raw ETW, which delivers on buffer-fill or flush timer. The p99 and tail size the
   lag floor. The probe's `lag` line answers it.
2. **Pairing.** ETW gives a flip with a token and a scanout time; NvFBC gives a wake with no
   identity. Nothing in either stream names the other, so matching is structural (N flips per
   source period against M captures per batch, in order). This does NOT get easier with more
   latency, and it is now the hard part.
3. **Is the grid readable at x3?** x3 never paced correctly and the reason is unknown. Where those
   frames actually land is a direct measurement, and it may be a different explanation from the
   batch-grouping one.
4. **Does DLSS-FG look structurally different** from driver-level Smooth Motion?
5. **Lost events fail silently.** An estimator fed incomplete data is the silent-wrong failure this
   project keeps hitting. Whatever consumes ETW must log lost-event counters.
6. **Two behaviour modes is two test surfaces.** The more the policy exploits ETW when present, the
   more the modes diverge, including at the boundary where upgrades land intermittently. The
   trace-replay harness is what makes that testable without a capture cycle.

## Settled, do not re-litigate

- **PresentMon as the consumer.** Rejected: it collapses per-flip data, and its `FrameType` enum
  has no NVIDIA member (`NotSet, Unspecified, Application, Repeated, Intel_XEFG, AMD_AFMF`), so it
  cannot label Smooth Motion frames anyway. `--track_frame_type` requires instrumentation via the
  Intel-PresentMon provider, which the NVIDIA driver does not emit.
- **Real-vs-generated labelling from any ETW source.** Not available for Smooth Motion. Not needed
  either: keep-first vs keep-real (Round 10) already answered which batch member is real, visually,
  on real output. That is stronger evidence than a driver label.
- **ETW as offline-only calibration.** Too weak. It discards the data's main value, which is
  aligning to patterns no built-in heuristic covers.
- **"Per-frame lookup is impossible."** It is viable given a raised lag floor and unambiguous
  pairing. Both are measurements, not assumptions.
- **Elevation.** Admin or the Performance Log Users group. Moot here: the relay already runs as
  admin.
