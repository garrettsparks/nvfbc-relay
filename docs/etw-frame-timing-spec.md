# ETW Frame-Timing Correlation Spec (honest present time + real/gen labels)

Status: DESIGN / EXPLORATION (nothing committed). The capture APIs (NvFBC, DXGI Desktop
Duplication) fundamentally cannot give the relay two things it wants: the TRUE per-frame display
(scanout) time, and whether a frame is REAL or driver-GENERATED. Both are emitted by the graphics
driver/OS as ETW events, which PresentMon consumes. This spec is how the relay could tap that -
correlated to its own captures - and, critically, a cheap offline Phase 0 that proves the join
before any real-time plumbing is built. Companion: `dxgi-native-pipeline-spec.md`,
`nvfbc-capture-pacing.md`, `frame-marker-spec.md`.

## Why (from the framegen finding, 2026-07-23)

Under Smooth Motion the relay captures the real+gen pair batched (~250us apart on DXGI's
LastPresentTime, ~2-3ms on NvFBC's grab-wake `arr=`), while the driver schedules the two SCANOUTS
~8.33ms apart. Neither capture API exposes that scanout spacing, and neither labels gen vs real.
PresentMon does both:
- `msBetweenDisplayChange` = the actual scanout cadence (vs `msBetweenPresents` = submission).
  NVIDIA's DLSS4 PresentMon build adds "flip metering" for it.
- "Frame Type Differentiation" = a real-vs-AI-generated label per frame.

The relay gives the pixels; ETW gives the honest timeline and the identity. Joined, that is the
data the relay has never had.

### Correction (2026-07-24, from reading the PresentMon source)

Half of the above is wrong for this hardware, and the correction changes what Phase 0 tests.

- **Honest display timing: AVAILABLE, and upstream.** NVIDIA's flip-metering work was merged
  into GameTechDev/PresentMon `main` (PR #440, 2025-06-13), so any release from v2.4.0 on has
  it. The separate `PresentMon-2.3.1-x64-DLSS4.exe` bundled with RTSS is not needed. It adds
  `PresentData/NvidiaTraceConsumer.*`, consuming the NVIDIA DisplayDriver provider
  `{AE4F8626-8265-40D1-A70B-11B64240E8E9}`, single event `FlipRequest` (Id 1, level 0x04,
  keyword 0x1000000000000000; fields `alloc`, `vidPnSourceId`, `ts`, `token`). It surfaces as
  the `MsFlipDelay` column, and makes `MsBetweenDisplayChange` reflect real metered intervals.
- **Real-vs-generated label: NOT AVAILABLE for Smooth Motion.** `FrameType` is an enum with no
  NVIDIA member: `NotSet, Unspecified, Application, Repeated, Intel_XEFG, AMD_AFMF`, printed as
  `Application` / `Repeated` / `Intel XeSS-FG` / `AMD AFMF`. `--track_frame_type` is documented
  as requiring instrumentation via the Intel-PresentMon provider, which the NVIDIA driver does
  not emit. On the 5080 the column will read `Application` for every row.
- **But the label is recoverable structurally.** `NVTraceConsumer::ApplyFlipDelay` attaches the
  metering delay to an EXISTING `PresentEvent`; it never manufactures a row. So PresentMon rows
  are application presents only (~60/s under Smooth Motion), while the NvFBC ring wakes ~120/s.
  Each present therefore draws ~2 captures, and the one sitting at the measured capture latency
  is the real frame; the other is generated. The join becomes the labeller, which is exactly
  what tests the ring's keep-real heuristic. This is what `etwjoin.py` measures.
- **Epoch gap.** Relay log `arr=`/`dl=` are QPC minus `m_baseQpc` in microseconds - relative to
  relay start. PresentMon's QPC is absolute. Closed without touching relay code by having
  PresentMon also capture `NvFBCR.exe`: the relay's own presents are the same events in both
  files, so aligning those two sequences pins the origin.

## The join key: QPC (answers "is the ETW stamp ours?")

No - and it does not need to be. `QueryPerformanceCounter` is a SYSTEM-WIDE monotonic clock:
every process reads the same timebase, and ETW event timestamps are QPC-based. So the relay's
own `arr=` (QPC at NvFBC grab-return) and an ETW present event's QPC are on the SAME clock,
directly comparable - they just mark DIFFERENT events (grab-wake vs true present), separated by a
small, roughly-constant capture latency. That is exactly what makes a nearest-QPC join work:
match each captured frame to the ETW present event whose QPC is nearest its `arr=`; the offset is
the capture latency, which the join measures rather than assumes.

Caveat the relay must respect: ETW delivery LAGS the event by ms to tens of ms (buffered). Fine
for offline correlation; for a LIVE relay it fights the need to decide the present NOW (see
Architecture).

## Phase 0: offline correlation (build/validate this FIRST)

Prove the join and the data before any real-time consumer. No relay code.
1. Capture a Smooth-Motion-ON session with BOTH running: the marked relay (`-mark N`, so the video
   and log are frame-exact) AND PresentMon (Intel's, or NVIDIA's DLSS4 build) writing its per-frame
   CSV of the SAME source game.
2. Offline join: for each relay temporal-line (its `arr=`/capture QPC) find the nearest PresentMon
   present row by QPC. Emit a table: relay op/bw + PresentMon present-time, display-time
   (`msBetweenDisplayChange`), and frame-type (real/gen).
3. Metrics / go-no-go:
   - Join quality: is the nearest-QPC match unambiguous (a clean, roughly-constant offset, one
     PresentMon row per capture)? A bimodal or drifting offset means the join is unreliable.
   - Does the frame-type label line up with the batch-collapse belief (the ring's presumed-gen vs
     real)? This validates OR corrects the "take the second frame" heuristic against ground truth.
   - Do the display-times reveal the even 8.33ms spacing the capture timeline hid?
4. This is a script in frame-drop-analysis (like the blend-fingerprint matcher), not relay code.
   Cheap, and it either proves the holy grail is real or kills it.

## Architecture (if Phase 0 passes)

```
 NvFBC capture ─► ring (pixels + arr= QPC)
 PresentMon/ETW ─► present-event stream (QPC present + display time + real/gen)
                     │  nearest-QPC join
                     ▼
         each ring frame tagged with true display time + real/gen  ─► policy
```

- Consumer: prefer the PresentMon SERVICE + its streaming API (Intel PresentMon SDK) over
  hand-rolling an ETW session - PresentMon already parses the DxgKrnl/Dwm providers, tracks the
  present token through the flip, and does frame-type differentiation. Re-deriving that from raw
  ETW is a large, fragile surface.
- The LIVE latency problem: ETW/PresentMon events arrive AFTER the present. A relay presenting on a
  60Hz deadline cannot wait tens of ms for the label. Options to evaluate: (a) run the relay one
  present BEHIND so the ETW label for present N is available when N is composited (adds a frame of
  latency - acceptable? measure); (b) use ETW only to VALIDATE/retune the heuristic offline and
  keep the real-time path heuristic; (c) predict the label from the timeline and correct when the
  ETW event lands. Phase 0 informs which.

## EtwProbe (this branch's relay-side probe, parallel to DdProbe)

A standalone probe that opens a real-time ETW session for the present/flip providers and logs each
event's QPC + present metadata, so we can see the live event stream and its latency directly
(complements the offline PresentMon-CSV join). First cut; the exact provider GUIDs, event IDs, and
property decoding need on-hardware verification (ETW schemas are not stable to guess blind). If the
raw-ETW surface proves too fragile, fall back to consuming the PresentMon service API instead - the
probe is a means to measure feasibility, not the final architecture.

## Open questions / risks

- Raw ETW vs PresentMon SDK: raw ETW is a big fragile surface; the PresentMon service/SDK is the
  pragmatic path. Decide in Phase 0.
- Live latency budget: can the relay afford one frame of lag to get the label, or must it stay
  predictive? Measure the actual ETW delivery lag first.
- Frame-type coverage: does differentiation cover Smooth Motion specifically (vs only DLSS-FG)?
  Verify on the RTX 5080 + Smooth Motion.
- Admin/session: ETW real-time sessions and PresentMon typically need elevation; the relay would
  inherit that requirement.
- This whole direction is only worth it if the relay ever wants to USE gen frames or needs precise
  gen/real timing; keep-real + the relay's own interpolation may remain the simpler answer.
