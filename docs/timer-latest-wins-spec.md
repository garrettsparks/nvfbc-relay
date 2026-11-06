# Latest wins for the timer modes

Written 2026-11-05. Not built. The user asked for it the same day ("I want to try that out"), then
chose the simpler of two forms (section 3). Section 7 lists the decisions left to the user, each
with Claude's default.

## 1. Why

The timer modes (`t:<fps>`, `b:<fps>`, `o:<fps>`) present on a QPC timer into the D3D11
composition swapchain, 3 buffers (`docs/d3d11-present-unification-spec.md`, 3.5). The presentation
manager shows presents first in, first out: one present per refresh, in order.

Four marked 5-minute KCD recordings on 2026-11-05 (`t:60`, `b:60`, `o:60`, `t:vsync`, all `-wgc`,
the XR1 at 60.0002 Hz, the CI build of `2044bc4`) showed what that queue does after a hiccup:

- Every launch logged one `display gap:` about 4.3 s after its first present. In `t:60` and `o:60`
  it was 2 refreshes between frames (1 held), and the wait for room after each timer wake stayed at
  about 1 us for the whole run.
- In `b:60` it was 3 refreshes (2 held), at 4.38 s. From 4.41 s every present waited for room: 11.3
  ms at first, shrinking about 5.3 us a second, the drift between the timer (59.99988 Hz) and the
  card. 18,624 of 18,888 presents were held past half a period.
- The cadence did not change (wake interval p99 16,737 us against `t:60`'s 16,740) and the video had
  0 relay freezes, 0 blends, 0 repeats and 0 skips.

The reading, not proven: the extra held refresh left one more picture in the queue. The timer
runs at the card's rate, so nothing removes it until the drift does, after about 35 minutes (11.3
ms at 5.3 us a second). Until then that launch shows everything about one refresh later than a
launch that started cleanly. The delay is unpredictable from launch to launch, which is what the
user cannot correct for (audio sync is set in advance).

## 2. What exists

- `TimerPacing::Wait` (`src/relay/Pacing.cpp`): waits for the timer's deadline, then for a free
  buffer (`IPresentPath::WaitForFrame`). It records the wait as `blk=` on the present's line and in
  the pacing summary. A present held past half a period is decided at the time it is let go
  instead of at the deadline.
- `PresentScheduler` (`src/relay/PresentScheduler.cpp`): a fixed timeline that advances one period
  per present. A schedule more than one period behind is re-anchored to now (the catch-up clamp),
  so a stall never produces a burst of presents.
- `CopyEngineTarget` (`src/relay/CopyEnginePresent.cpp`): the composition swapchain. `Present()`
  sets the next buffer on the surface and calls `IPresentationManager::Present()` with no target
  time. `WaitForBuffer` waits on that buffer's available event. `DrainStatistics` counts present
  statuses (`PresentStatus_Skipped`, `PresentStatus_Canceled`) and reads displayed times
  (`display gap:` lines and the `copy engine displayed:` summary). The manager also offers
  `GetNextPresentId`, `SetTargetTime`, `CancelPresentsFrom` and `GetPresentRetiringFence`; none is
  used today.

What the queue earned, measured 2026-10-30 (unification spec 7.2), and what this change must not
lose:

- **A slip is one repeated refresh.** With the timer slower than the card (`b:59.98`, `t:59.98`,
  `b:59.98 -wgc`, `b:59.998`, Pandora), every slip was one repeat, 0 skipped presents, and no band
  of repeats and skips at the refresh edge. The reading there: a picture that misses one refresh
  moves every later picture by one, so the timer's jitter has nothing left to flip.
- **A timer faster than the card falls onto the card.** `b:60.02`: 5,286 of 7,390 presents held,
  wait for room p50 16.2 ms, one gap, 0 skipped. In effect `b:vsync` with a late decision.

## 3. Design

Two forms were weighed. The user chose the first as the simpler.

**A. Latest wins (chosen).** Before each present, cancel any earlier present not yet on screen:
`CancelPresentsFrom(<the previous present's id>)`, then present. A picture can never wait behind
another, so a startup hiccup costs at most one replaced picture and no lasting delay. No state, no
threshold: the same call on every present.

**B. Restore the depth (dropped, kept as the fallback).** Count presents in flight (the next
present id minus the retiring fence's value); when it stays one above normal for about half a
second, skip one present. Costs one content skip at that moment, keeps the queue's behaviour
otherwise. Dropped as more decision logic than the problem needs; it returns only if A fails its
gate in section 5.

**What A risks losing,** and the tests in section 5 are built around these:

1. **The band at the edge may come back.** Without a queue, two presents landing in one refresh
   means one is canceled (a skip) and the next refresh has none (a repeat). With the tight wait's
   jitter (p99 41 to 74 us on 2026-11-05) and the real drift (5.3 us a second), the edge takes
   about 15 to 30 seconds to cross, once every 52 minutes or so, an estimate. At 10 times the
   drift (`b:59.998`) it would take about 3 s, and at 100 times (`b:59.98`) about 0.3 s.
2. **A timer faster than the card would skip** where today it settles onto the card: every
   time the timer gains a whole period, one present is canceled. `b:60.02` against 60.0002 Hz
   gains one about every 50 s.

**Where the change goes.** One place: `CopyEngineTarget::Present()` cancels from the previous
present's id when the target was set up for latest wins. Only the timer pacing sets it up; the
`:vsync` modes pace on the buffer itself and keep the queue. The policy step does not change:
the timer modes still decide at the deadline, and with nothing queued the wait for room should
stay near zero.

**The switch.** A hidden option, `-xlatest`, off by default while it is tested (the project's way
for an experiment). Each run logs the latest-wins setting on the `Resolved options:` line, and
the copy engine summary already counts canceled presents.

## 4. The probe (first, before the switch is built for real)

Unknowns about the presentation manager, answered on the rig before any design is trusted:

1. Does `CancelPresentsFrom(id)` drop a present that is queued but not yet flipped, and what does
   it do to one already on screen (expected: nothing)?
2. Does the canceled present's buffer come back on its available event at once, so the
   round-robin `m_next` stays valid?
3. What does the summary's canceled count read, and do the canceled presents show up as
   `PresentStatus_Canceled` in the statistics?
4. Does `SetTargetTime` alone give latest wins (several presents due at one refresh, the newest
   shown and the rest `PresentStatus_Skipped`)? If it does, that may be the simpler call.

The probe is a few rows of `b:60 -wgc` on the desktop with the change in, reading the counts and
the `display gap:` lines, plus the forced control below.

## 5. Tests and the gate

Every row marked, on the rig, KCD where a game is needed (the timer modes' fixtures and the 10-28
and 11-05 recordings are KCD); the desktop where the clock alone is tested.

| row | mode and flags | expect with `-xlatest` | expect without (today) |
|---|---|---|---|
| forced extra picture | `b:60 -wgc -xstuck 300` (a test switch: at present 300, present twice in one tick) | wait for room back to about 1 us within a refresh; 1 canceled | wait steps to a refresh's worth and stays (the 11-05 `b:60` case) |
| normal | `t:60 -wgc`, `b:60 -wgc`, `o:60 -wgc`, 5 min each, KCD | as today: 0 relay freezes, wait about 1 us | the 11-05 rows |
| slower timer, 100 times the drift | `b:59.98 -wgc`, desktop, 2 min | each slip one repeat, and no band of repeats and skips (risk 1) | 2 slips, one repeat each (10-30) |
| slower timer, 10 times the drift | `b:59.998 -wgc`, desktop, 18 min | the same; this is the row where a band would last long enough to see | 2 slips, one repeat each (10-30) |
| faster timer | `b:60.02 -wgc`, desktop, 4 min | about one skip every 50 s, so 4 or 5 (risk 2) | falls onto the card, 0 skipped (10-30) |

Controls: the forced row is the positive control (it must clear with the switch and must stay
stuck without it); a run without the switch must reproduce the 10-30 off-rate rows; the canceled
count must be 0 without the switch.

**The gate, written before the data.** Latest wins becomes the default only if: the forced row
clears every time; the normal rows match today's; the slower-timer rows show no band (no more
than one repeat or skip per slip); and the user accepts the faster-timer skips (or they do not
happen). If the slower rows show a band, A is dropped and B (section 3) is built instead.

Launch-to-launch luck: the natural startup hiccup is launch luck (one of four launches on
2026-11-05). The forced row tests the mechanism; counting how often launches get stuck naturally
needs a balanced sitting of several launches a mode with the rule written first, and is not
needed for this decision.

## 6. Cost

No GPU work is added: a cancel is one call on the presenting thread. No cost bench is planned. If
the user wants one, three rows a build, balanced, partnered, uncapped Pandora.

## 7. Decisions for the user

1. **Faster timers skipping (risk 2).** Default: accept it if the slower rows pass. A timer faster
   than the card is a setting mismatch the log already reports; today it hides as `b:vsync` with a
   late decision.
2. **A or B if both pass.** Default: A, the simpler.
3. **Default on after the gate.** Default: yes ("tested = default"), with `-xlatest` removed.

## 8. Not in this spec

- What the 4.3 s startup hiccup itself is (a guess: the card's display moving to independent flip
  after the window opens). Latest wins makes it harmless; finding it is its own item.
- The `:vsync` modes. They pace on the buffer coming back, so a queue cannot build the same way.
- `-d3d9`. Its timer presents were latest wins through DWM; it costs more, has no `-wgc` and no
  `o`, and is due for removal.
