# ETW Frame-Timing Spec (reading the driver's real scanout times)

Status: FEASIBILITY ESTABLISHED, no relay code consuming it yet. Measured on hardware: the events
arrive, the wire layout is recovered and validated against a known refresh rate, the scanout grid
is readable, delivery latency is 8 ms p50 once the session is configured for it, and flips pair to
capture wakes within ~70 us. What remains unbuilt is the relay-side consumer and the pairing rule
itself.
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

**There is a second payoff, and it is already costing frames in the daily-driver config.** The
above is about 90 fps. Measured on two 60 fps captures (~30 KCD map cycles each), a large share of
the blends the relay produces TODAY are compensating for a stamping error rather than for real
motion:

| gaps of 1.3 - 2.5 source periods | capture A | capture B |
|---|---|---|
| followed by catch-up, i.e. the pair sums to 2 periods | 36% | 28% |
| consistent with a genuinely dropped source frame (~3 periods) | 2% | 3% |

A dropped frame leaves a gap of ~N periods and the frames after it stay on the shifted grid. A
frame that WAS rendered on time but handed over late leaves a long gap followed immediately by a
short one, because the next frame arrives near its own correct time. Real drops are ~2%. When a
gap does catch up, the delivery was late by a median of 7650 us, which is 0.46 of a source period:
almost exactly the amount that pushes the target past the srcPeriod/4 passthrough threshold and
forces a blend. So the ring records motion that did not happen, and the relay smoothly interpolates
across it.

**Wake stamping cannot even classify these.** A real frame delivered ~8 ms late and a GENERATED
frame whose delivery slipped past the 3 ms batch-collapse window produce identical wake timelines:
one entry, roughly half a source period off the grid, with no way to tell which it was. Note that
0.46 of a source period is also, to within measurement error, the flip separation between a real
frame and its Smooth Motion twin. Both readings fit the same data. A flip token and a true scanout
time separate them immediately, which is something no amount of arrival-time analysis can do.

This does not change the design below. It changes the priority: the payoff is not confined to a
source rate nobody runs yet, and the first probe trace can settle the ambiguity directly.

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

**Correct timestamps are NOT a licence to treat generated frames as interchangeable with real
ones.** This is the constraint the whole design has to respect, and it is easy to get backwards.

Flip-stamping makes the generated frames usable, but selection must still PREFER the real one.
Measured at 60x2: if both members were kept, flip-stamped, and selection simply took
nearest-to-target, it would land on a generated frame **93.2% of the time**. The targets sit almost
exactly half a source period from every real flip (p50 7937 us, and 0.0% of them within the 4166 us
passthrough threshold), because at a 16.67 ms output period against an 8.33 ms flip grid the lock
phase picks a parity and holds it. Nearest-selection would therefore invert the output from
essentially all-real to almost all-generated, which the keep-first vs keep-real A/B (two full
videos, Round 10) established is the visibly worse picture.

This also explains something that looks like a bug and is not. The ring stamps the surviving real
member at BATCH-START, which is the GENERATED frame's display time, roughly 8.3 ms "early". The
comb lock has settled with targets landing on that same instant. The stamp offset and the lock
phase cancel, so selection reliably picks the real frame. **That 8.3 ms offset is load-bearing**;
removing it without re-phasing the lock by half a comb inverts the output.

So the shape of the win is narrow and specific: a generated frame becomes usable when NO real frame
is near the target. That is rate mismatch (90 -> 60), stall resumes, and ragged delivery. It is not
a general upgrade at 60x2, where the existing behaviour is already correct.

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

| capture rate | frames alive for a 40 ms floor + ~2 source periods | RING_SIZE |
|---|---|---|
| 120/s (60x2) | ~73 ms | >= 9, use 16 |
| 180/s (90x2) | ~73 ms | >= 14, use 16 |

16 slots is ~127 MiB at 1920x1080x4B. The 90 ms floor this table originally assumed needed 32;
measuring delivery lag at 8 ms p50 rather than the ~30 ms guessed made the smaller floor viable.

**The lag floor is a runtime knob, not a design commitment.** Because degradation is graceful, the
relay can ship at today's 20833 us and raise the floor per-profile. Relay output latency does not
touch the player's input loop (gameplay happens on the source display; the relay feeds the XR1 ->
OBS PC -> Twitch), so tens of milliseconds cost stream viewers a rounding error on top of seconds
they already have. The lag must stay CONSTANT for audio-sync compensation, which the existing
launch-time-constant design already guarantees.

Measured, the floor needed is **40-50 ms**, not the 90-120 ms guessed here originally. At today's
20833 us the p50 upgrade lands and the tail does not, which is exactly the graceful path this
section describes rather than a failure.

## The provider

```
NVIDIA DisplayDriver {AE4F8626-8265-40D1-A70B-11B64240E8E9}
FlipRequest (Id 1, level 0x04, keyword 0x1000000000000000)
fields: alloc (u64), vidPnSourceId (u32), ts (u64), token (u32)
```

`ts` is the PROPOSED FLIP TIME in QPC ticks. Consecutive `ts` values on one head are the true
scanout cadence. GUID and descriptor come from the manifest PresentMon embeds, cross-checked
against its consumer.

**The wire layout is now recovered from hardware, not taken from that field list.** TDH cannot
resolve field names here: NVIDIA's provider does not register a manifest (10800 of 10800 events
failed on a current driver), which is why PresentMon ships a reverse-engineered copy of the schema
rather than asking the system. Decoding is positional, from a layout read off real payloads:

```
+0  u64   per-head counter, +1 per event
+8  u64   second counter, +1 per event
+16 u64   PROPOSED FLIP TIME (QPC)
+24 u32   vidPnSourceId (head)
+28 u32   token
+32       12 bytes, zero on every payload observed
44 bytes total, descriptor ver=0 opcode=10
```

`+16` is identified positively rather than by elimination: its per-head deltas land on the 8.33 ms
half-period a 60 fps source with 2x frame generation produces, and on a 240 Hz desktop they land on
4.167 ms with 95.7% of gaps in a single bucket - a rate known independently of the measurement.
Decoding is guarded three ways (payload size, descriptor version, and the flip time landing within
a second of the event that announced it) so a driver that moves the layout stops decoding rather
than reporting confident nonsense.

**Two heads appear, not one.** The gaming display at 120 flips/s under 2x and the relay's own XR1
output at 60. Filter by head; never aggregate them.

**`ahead` is ~0.** The driver logs the event at the instant it assigns the flip time (measured
0-880 us depending on workload). There is no advance notice, so the earliest a flip can be known
is its own scanout time plus delivery lag. That makes delivery lag the whole budget rather than
one term in it.

## EtwProbe

Standalone exe, in the solution so CI builds it. Requires elevation. Reports:

- **dts histogram** - gaps between consecutive proposed flip times on a head. A tight spike near
  8.33 ms at 60x2 means the grid is even and readable. Smeared or multi-modal kills the arithmetic
  model outright.
- **ahead** - `ts` minus the event stamp. Measured at ~0, see above.
- **lag** - callback time minus event stamp: raw ETW delivery latency. This sizes the floor.
- **session losses** - events, real-time buffers and log buffers dropped, read from the session at
  stop. Mandatory next to any latency figure: latency bought by discarding events looks identical
  to latency earned, and this project's recurring failure is the silently wrong measurement.
- **hexdump of the first 8 payloads**, always, decode success or not. It is the only route to
  re-deriving the layout when a driver update moves it.
- **`--dxgk` control** - also enables DxgKrnl and counts it. If NVIDIA events are zero but DxgKrnl
  is not, the session works and the provider is the problem. Without the control a silent probe
  has five possible causes.

## Required session configuration

Any consumer, probe or relay, needs all of these. Each was established by measurement and three of
them are the difference between an 8 ms pipeline and a 1.5 second one.

```
StartTrace:
  Wnode.ClientContext = 1                              // QPC, so flip times and NvFBC arr= share a clock
  LogFileMode = EVENT_TRACE_REAL_TIME_MODE
              | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING // removes ~14 ms of pipeline delay
  BufferSize = 64 KB, MinimumBuffers = 256, MaximumBuffers = 1024   // PresentMon's sizing

OpenTrace:
  ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME
                   | PROCESS_TRACE_MODE_EVENT_RECORD
                   | PROCESS_TRACE_MODE_RAW_TIMESTAMP  // without this EventHeader.TimeStamp is
                                                       // system time, not the QPC asked for above

Consumer thread:
  ControlTrace(EVENT_TRACE_CONTROL_FLUSH) every ~10 ms // delivery is otherwise on a ~1 s cadence
```

`RAW_TIMESTAMP` is the subtle one: `Wnode.ClientContext` alone does NOT make the event header
arrive in QPC, and mixing a QPC payload against a system-time header silently rejects or corrupts
every comparison. Both PresentMon and this probe set it.

Read the session's loss counters at stop and log them. Every configuration above was verified to
lose zero events across seven runs, but that is a property of a measured workload, not a guarantee.

## Open questions

1. **Raw ETW delivery lag. MEASURED - and the session must be configured for it.** Left at
   defaults, real-time ETW delivers on a ~1 s cadence and hands over the PREVIOUS period's buffer,
   so even the freshest event in a delivery is ~1 s old (p50 1487 ms, max 2025 ms). That is 70x
   past the relay's bracketing lag and would have killed the design. Two settings fix it, and both
   are needed:

   | configuration | lag p50 | p95 | max |
   |---|---|---|---|
   | defaults | 1487 ms | 1961 ms | 2025 ms |
   | forced flush every 10 ms | 23.7 ms | 30.7 ms | 35.9 ms |
   | `EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING` | 20.2 ms | 36.8 ms | 37.5 ms |
   | **both** | **8.0 ms** | **15.0 ms** | **30.4 ms** |

   The two are not the same mechanism. A forced flush (`ControlTrace` with
   `EVENT_TRACE_CONTROL_FLUSH` on a timer) delivers OFTEN but each delivery is stale; consolidating
   the per-processor buffers makes each delivery FRESH by removing ~14 ms of pipeline delay.
   Combining them gives frequent delivery of fresh data. The relationship is simply
   `p50 ~= flush interval / 2`.

   Two practical notes. `Sleep()` is quantised by the 15.6 ms system timer, so a 10 ms request
   produces a 16 ms interval and 20 ms and 30 ms requests are indistinguishable (both measured
   15.7 ms p50); a high-resolution waitable timer would take p50 to ~2.5 ms if anything ever needs
   it. And zero events were lost in any configuration, including the consolidated one that
   Microsoft documents as able to lose them - verified across seven runs, not assumed.

   PresentMon independently confirms the approach: it uses `PROCESS_TRACE_MODE_RAW_TIMESTAMP` and
   exposes a `SetEtwFlushPeriod` API for exactly this, defaulting to off. It does NOT consolidate
   per-processor buffers, and it sizes its pool at 64 KB x 256-1024 buffers, which this probe now
   matches.

   Consequence for the lag floor: with p95 at 15 ms and the flip 8.33 ms after the wake, an upgrade
   lands ~23 ms after capture. At today's 20833 us floor most upgrades land and the tail does not,
   which is the graceful degradation the design already assumes. A **40-50 ms** floor lands
   everything including the max. The 90-120 ms figure below was a guess made before this
   measurement and is not needed; RING_SIZE goes 8 -> ~16 rather than 32.

   Not yet measured in situ: these are desktop numbers with a TestUFO tab, not the game plus relay
   under load. That measurement belongs to the first relay-side consumer.
2. **Pairing. Much easier than this section assumed, but the RULE is still unvalidated.** The
   pessimism here predated knowing both streams would share a clock. They do: the probe requests
   QPC (`Wnode.ClientContext = 1` plus `PROCESS_TRACE_MODE_RAW_TIMESTAMP`) and the relay logs its
   QPC origin at startup, so the two logs join by arithmetic with no fingerprinting. Against a real
   capture, every other head-0 flip coincided with an NvFBC wake to within **+73, +62 and -40 us**.
   That is identification, not structural matching.

   What that establishes and what it does not:

   | claim | status |
   |---|---|
   | the first wake of a batch coincides with a flip | measured, 3 of 3 |
   | the second wake belongs to the NEXT flip, 8.33 ms later | inferred from how frame generation must work, NOT measured |

   The second is the load-bearing half of any pairing rule and is currently reasoning. It needs
   measuring across thousands of batches, including the ragged cases (a pair that missed the 3 ms
   collapse window, a stall resume), before a `capture_seq` field exists anywhere. An unpairable
   wake must come out explicitly unpairable rather than silently matched to a neighbouring flip.

   One consequence already visible: batch-collapse stamps the surviving REAL frame at batch-start,
   but the flips say that frame does not reach the screen until ~8.33 ms later. A constant offset
   is harmless; it stops being constant when the batch structure varies.
3. **Is the grid readable at x3?** x3 never paced correctly and the reason is unknown. Where those
   frames actually land is a direct measurement, and it may be a different explanation from the
   batch-grouping one.
4. **Does DLSS-FG look structurally different** from driver-level Smooth Motion? It is an in-game
   SDK path rather than a driver one, so it may not emit FlipRequest with the same shape, or at
   all. Everything below assumes it does until measured.

4b. **Can the multiplier be DERIVED rather than configured?** A "-fg x2 / x3 / x4" flag would be
   one more thing to get wrong, and would be silently wrong the moment the user changes a driver
   setting without changing the flag. Both quantities needed appear to be measurable at runtime:

   | quantity | how it is read |
   |---|---|
   | multiplier | flips per capture batch, or source period / flip period |
   | which member is real | position in the batch |

   At 60x2 the batch holds 2 wakes and the interval carries exactly 2 flips in 99.96% of cases, so
   the multiplier falls straight out of counting. The real member is the SECOND of two, which the
   two-video A/B established independently of any of this.

   The rule that would generalise without configuration is **"the real frame is the LAST member of
   the batch"**, which follows from how interpolation must work: the driver cannot produce frames
   between N-1 and N until it has N, so the generated ones are submitted first. If that holds, x3
   is [gen, gen, real] and x4 is [gen, gen, gen, real], and nothing needs declaring.

   UNVERIFIED beyond x2. Profiling x3, x4 and DLSS-FG is what settles it, and the failure mode if
   the rule does not generalise is severe rather than subtle: picking the wrong member outputs
   almost entirely generated frames. The offset from the nearest flip is a weaker independent
   signal (74 us for the generated member, 411 us for the real one at x2, separating 99% / 76%),
   and it is the only signal available for the ~17% of batches that arrive with a single wake,
   where position cannot say anything.
5. **Lost events fail silently.** An estimator fed incomplete data is the silent-wrong failure this
   project keeps hitting. Whatever consumes ETW must log lost-event counters.
6. **Two behaviour modes is two test surfaces.** The more the policy exploits ETW when present, the
   more the modes diverge, including at the boundary where upgrades land intermittently. The
   trace-replay harness is what makes that testable without a capture cycle.
7. **Late real frame, or a generated frame that missed the batch window?** The wide brackets that
   force blends at 60 fps sit a median of 0.46 source periods off the grid, which fits BOTH a real
   frame handed over late and a Smooth Motion twin whose delivery slipped past the 3 ms collapse
   threshold. The wake timelines are identical. The x2 baseline trace answers it: count the flips
   per source period and compare their scanout spacing against the wake spacing over the same
   interval. If the answer is "generated frame", the 3 ms threshold is the thing to fix and it is
   cheaper than consuming ETW at runtime. Still open: the traces so far are desktop-only, and this
   question needs the game running with the relay capturing.

## Settled, do not re-litigate

- **PresentMon as the consumer.** Rejected: it collapses per-flip data, and its `FrameType` enum
  has no NVIDIA member (`NotSet, Unspecified, Application, Repeated, Intel_XEFG, AMD_AFMF`), so it
  cannot label Smooth Motion frames anyway. `--track_frame_type` requires instrumentation via the
  Intel-PresentMon provider, which the NVIDIA driver does not emit.
- **Real-vs-generated labelling from any ETW source.** Not available for Smooth Motion. The label
  comes from batch position, which the keep-first vs keep-real A/B (Round 10) established visually
  on real output - stronger evidence than a driver label would be.

  What has changed since that entry was written: the label is now REQUIRED rather than merely
  available. Correct timestamps make generated frames usable, and a selection that cannot tell the
  two apart lands on generated frames 93.2% of the time at 60x2. See the constraint under
  Decisions. So "not needed either" was true only while generated frames were being discarded.
- **ETW as offline-only calibration.** Too weak. It discards the data's main value, which is
  aligning to patterns no built-in heuristic covers.
- **"Per-frame lookup is impossible."** It is viable given a raised lag floor and unambiguous
  pairing. Both are measurements, not assumptions.
- **Elevation.** Admin or the Performance Log Users group. Moot here: the relay already runs as
  admin.
