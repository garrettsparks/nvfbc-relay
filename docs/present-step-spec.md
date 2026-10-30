# One present step, shared by the relay, the flow planner and the replay

## 1. Why

What the relay decides for one present is computed by three copies of the same sequence:

1. **The relay's loop**, `TemporalCaptureMode::Run` (`src/relay/TemporalCaptureMode.cpp`, from
   the target computation to `ConsumeGuardExemption`), plus the decision inside the present path
   (`D3D11Present::Compose` calls `DecideComposite` on the path's own `CompositeState`).
2. **The flow planner**, `D3D11PresentBackend::PlanFlows` (`src/relay/D3D11Present.cpp`), which
   guesses the next targets as `target + (j + 1) * period` and re-runs only `DecideComposite` on a
   copy of the composite state.
3. **The replay**, `Simulate` (`tests/PolicyTests.cpp`), which re-implements the loop for the
   corpus.

Each copy drifted from the others, and each drift cost something measurable:

- **The planner ignores the lock's wrap ease.** About half of the presents that synthesize with
  no flow planned (`fl=own`, drawn as the blend) sit inside a wrap ease: 20 of 34 on 10-27 0,
  38 of 76 on 10-27 1, 34 of 74 on 10-26 1. The other half are not yet explained.
- **The replay hid a batch's first member from the lookahead**, which production counts until
  the wake that retracts it, and **published frames at arrival**, where production publishes after
  the copy's flush. Found 2026-10-28 on the 10-27 1 barn opening; fixed in the replay, with flush
  times added to the fixtures.

The corpus only tests the copy in `Simulate`, so a drift between the relay and the replay is
invisible to it until a stream shows it. This spec puts the sequence in one pure function in
`src/core`, called by all three. The planner then predicts the next presents with the rule that
will decide them, and the corpus runs the code the relay runs.

## 2. What the step is

One call per present, between the pacing wait and the present itself:

```
deadline -> target (lag, pull, ease) -> lookahead move lands -> stage-6 walk (dejitter) ->
bracket read -> stall run, lock update, resume guard -> recent frames, lookahead update ->
guard exemption -> composite decision
```

Everything in it is policy that `src/core` already holds (`ApplyLookahead`, `UpdateStallRun`,
`UpdatePhaseLock`, `UpdateLookahead`, `DecideComposite`, `MeasureLateness`, the `StampOverlay`).
The step only fixes their order and their wiring, which today each caller does for itself.

**Not in the step:** the pacing wait and the present, the compositor's drawing, the marker, and
logging. The step returns what happened (the lookahead event and its size, each dejitter
verdict, a resume, a wrap) and the caller logs it, so every log line keeps its format; the
analysis tools parse them.

## 3. Design

### 3.1 State

```cpp
struct PresentStepState {
    PhaseLockState lock;
    PhaseLookahead lookahead;
    CompositeState comp;          // moves here from the present paths
    StampOverlay overlay;
    AnchorChain chain;
    long long nextBatch;
    int64_t maxTarget;            // the dejitter fence
    PresentHistory presentHist;
};
```

Plain values, copyable: the planner copies it and steps the copy. `CompositeState` moves out of
the present paths into this state, so the decision the present path executes and the one the
planner predicts come from the same state. The paths receive the decision instead of making it.
(Today each path owns one because `DecideComposite` mutates it; with the state in one place that
reason goes away.)

### 3.2 What the step reads

Two small interfaces, each with a relay implementation and a replay implementation:

- **`IRingView`**: `FindBracket(target, overlay)`, `ReadRecentFrames(target, overlay)`,
  `BatchOpens()`, `BatchStartAt(i)`. The relay's is `CaptureRing` as it stands. The replay's
  wraps its arrays (stamps, valid, publish and copy times) and keeps the ring model the replay
  already has.
- **`ILatenessSource`**: `MeasureLateness(batchStart, chain, window)` with its `dataPending`. The
  relay's is the ETW consumer, the replay's the `FlipHistory` it feeds from the fixture.

The ring model itself stays two implementations: `CaptureRing` is Windows and GPU code. The
corpus's field-fidelity check (replay against the log, per fixture) keeps the replay's ring model
honest, as it does today. Pulling the ring's slot bookkeeping into `src/core` is a possible later
step, not part of this one.

### 3.3 Calls

```cpp
struct PresentStepResult {
    int64_t target;
    FrameBracket bracket;          // as the ring returned it
    CompositeDecision decision;
    LookaheadEvent lookaheadEvent; int64_t lookaheadSize, lookaheadFirstTs;
    int64_t rephase;               // pull change that landed this present
    bool resumedFromStall;
    // dejitter verdicts this present: count plus a short list, for the log
};

PresentStepResult StepPresent(PresentStepState& s, const PolicyConfig& cfg,
                              const IRingView& ring, ILatenessSource* lateness,
                              int64_t deadline, int64_t bracketingLag);
```

- **The relay** calls it once per present with `lateness` set, then hands `decision` and
  `bracket` to the present path and logs from the result.
- **The planner** copies the state and calls it for the next N presents, with deadlines one
  present period apart and `lateness` null (no stage-6 walk: a correction can only land on frames
  the planner reads one present later at most, and the walk's chain must not advance on a copy).
  Each result's `decision` says whether that present synthesizes and its `bracket` names the
  pair, so the planner flows exactly those pairs. The second `DecideComposite` in `PlanFlows`
  goes away.
- **The replay** calls it once per present, as the relay does.

### 3.4 What the planner still cannot know

- **A frame that has not arrived** by the present the plan is made at. With the lag at 95.8 ms,
  a present two periods ahead targets about 60 ms in the past, so its frames are almost always in
  the ring. A late frame changes the bracket, the prediction misses, and that present draws the
  blend with no flow (the rule since 56e78f8).
- **A dejitter correction** inserted between the plan and the present. Rare (about 0.3% of
  batches are corrected at all), and a miss again costs one blend.
- **The deadline itself**: the next vsync is predicted as one period on. Its jitter is tens of
  microseconds against brackets of milliseconds.

### 3.5 Every mode runs the same step

The step is the same for every temporal mode; only its inputs and what fills a synthesized
present differ.

| mode | present path | deadline from | decision | a synthesized present is |
|---|---|---|---|---|
| `b`, `b:vsync` | D3D11 | the card's vblank | `DecideComposite` | the blend |
| `o`, `o:vsync` | D3D11 | the card's vblank | `DecideComposite` | the flow warp, or the blend without a flow |
| `b:dwm` | D3D9 | DWM's compose tick | `DecideComposite` | the blend |
| `b:<fps>` | D3D9 | a QPC timer | `DecideComposite` | the blend |
| `t`, `t:dwm`, `t:<fps>` | D3D9 | compose tick or timer | `SelectFrame` | never: the nearest real frame |

- **Pacing is outside the step.** The caller waits (vblank, compose tick or timer) and passes the
  deadline in, so vsync, DWM and timer modes run identical policy. The planner's future deadlines
  come from the same source: one present period on under vsync, exactly one timer period on a
  timer.
- **The decision is the one place the compositor kind enters:** `DecideComposite` for `b` and
  `o`, `SelectFrame` for `t`. The selection state moves into the step state beside the composite
  state. The passthrough gate and the lookahead stay on for every synthesizing mode and off for
  `t`, as today (`TemporalCaptureMode.cpp`, the compositor setup).
- **`b:vsync` and `o:vsync` already run identical policy**; the step keeps them so. They differ
  only in the compositor and in `o` having a planner.
- **Modes that do not exist yet fit without changing the step.** `o:<fps>` is refused today (only
  `t` and `b` take a rate, and timer modes present through D3D9): it needs the D3D11 path to
  present on a timer, and then it is the step with a timer deadline. A nearest mode on the D3D11
  path (a `t:vsync` again) is `SelectFrame` with a vblank deadline.

**Corpus coverage by mode** (descriptions, 2026-10-28): 57 fixtures `b:vsync`, 2 `o:vsync`, 1
`b:dwm`, 8 older ones that do not say. No `t` mode and no timer mode. The replay feeds the step the
logged deadlines, so a timer capture replays like any other, but there is none to replay. Before
stage 2 (the relay's loop), a short marked local recording each of `t:dwm`, `b:60` and `b:dwm`
(5 minutes of gameplay) joins the corpus, so the byte-identical gate covers every decision path
and every pacing source the relay ships. The replay already runs both decisions every present
(`SelectFrame` and `DecideComposite`), but it always arms the passthrough gate and the lookahead,
so `mktrace.py` records the compositor (`compositor nearest|blend|interp` from the log's
compositor line) and the replay configures the step as the relay did: gate and lookahead off for
`t`.

## 4. Order, each step with its gate

1. **Extract `StepPresent` and the two interfaces in `src/core`; the replay calls it.** Gate: the
   suite's output byte-identical to the baseline (every line, every fixture), C++20 and C++17.
2. **The relay's loop calls it**, the composite state moving out of the present paths. Gate: Zig
   compile of `src/relay` here, CI, then one capture whose log lines read as before (same fields,
   same formats; the analysis tools run on it unchanged).
3. **The planner calls it on a copied state.** This is the one behaviour change: which flows are
   planned. Gate: a new corpus check (4) and a stream as the field check.
4. **Planner accuracy in the suite.** For every fixture, the share of synthesizing presents whose
   pair a plan made one and two presents earlier named, printed per fixture and bounded like the
   other bounds, so a later change that hurts planning fails the suite. Expected: near 100%
   outside stalls and late frames, against today's planner on the same fixtures as the control.

Field check for 3: the share of `fl=own` presents among synthesized ones, 34 of 1,697 (2.0%) on
10-27 0 and 76 of 3,069 (2.5%) on 10-27 1, whole logs, should fall toward the late-frame floor.

## 5. Not in this spec

- The half-comb direction question and the wrap-aware lookahead median (parked as
  `patches/2026-10-28-wrap-aware-lookahead-and-replay-fixes.patch` in the analysis folder).
- Detecting unsteady pacing and holding the lock through it.
- Running the replay in QPC ticks (tried 2026-10-28 and backed out: no gain on the case it was
  for, mixed across the corpus).
- Moving the D3D9 modes (`b:dwm`, `b:<fps>`, `t:dwm`, `t:<fps>`, `vsync`, `60`) onto the D3D11
  present path. The user's order (2026-10-28): after this refactor, with its own spec, and before
  `t:vsync` returns. Once the policy is one step that takes a deadline, a mode's present path no
  longer reaches the policy, so that move changes only how a mode presents and the corpus shows
  the decisions stay put.

## 6. Open questions

- **The D3D9 paths** (`b:dwm`, the nearest compositor) decide through `FrameCompositors`. Moving
  their composite state into the step as well keeps one rule; they plan no flows, so nothing
  else changes for them. To confirm while doing 2.
- **The present period for the planner's deadlines**: the scheduler's period, or the mean of
  recent presents (`PresentHistory`), which follows DWM's clock on a composed desktop.
